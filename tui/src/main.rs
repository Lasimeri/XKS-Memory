//! xksm-tui: terminal management interface for XKS-Memory (std only: term.rs is the whole
//! terminal layer, no terminal crates).
//! Views: 1 Sessions (per harness, their turns), 2 Search (recall or words, three scopes),
//! 3 Memories (every namespace, newest first), 4 Embedding (word-vector table, port,
//! coverage), 5 Store (file, cache, logging per harness). Actions: open, delete, forget a
//! session, export a session to Markdown, add a note, import a file, rebuild the table, embed
//! missing memories, set the port, pause or resume a harness. Live: the store's change counter
//! is polled every 200 ms and everything on screen reloads when an agent or another process
//! commits. `--script` runs a key script headless (tests/tui_golden.sh).
//! Store: argv[1], else $XKSM_STORE, else ROOT/.index/agents.xksm with ROOT = $XKSM_HOME or
//! ~/XKS-memories (as the xksm CLI); session files live under ROOT.
mod ffi;
mod store;
mod term;

use std::time::Instant;
use term::{list, paragraph, str_width, Item, Key, Rect, Screen, Sel, Style, CYAN, DARK, GRAY, GREEN, MAGENTA, RED, YELLOW};
use store::{uri_parts, Hit, Memory, NsCount, Session, Store};
use ffi::xksm_verify_report;

const PAGE: usize = 200;
const POLL_MS: i32 = 200; /* key wait per loop = the longest a store change waits to show */

// ---- local time with UTC offset, through the C library -------------------------------------
#[repr(C)]
struct Tm {
    tm_sec: i32,
    tm_min: i32,
    tm_hour: i32,
    tm_mday: i32,
    tm_mon: i32,
    tm_year: i32,
    tm_wday: i32,
    tm_yday: i32,
    tm_isdst: i32,
    tm_gmtoff: i64,
    tm_zone: *const std::os::raw::c_char,
}
extern "C" {
    fn localtime_r(t: *const i64, out: *mut Tm) -> *mut Tm;
    fn isatty(fd: std::os::raw::c_int) -> std::os::raw::c_int;
}

/// test mode (--script): measured durations print as # so screen snapshots are exact
static DETERMINISTIC: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

fn fmt_ms(ms: f64) -> String {
    if DETERMINISTIC.load(std::sync::atomic::Ordering::Relaxed) { "# ms".into() } else { format!("{ms:.1} ms") }
}

fn fmt_s(s: f64) -> String {
    if DETERMINISTIC.load(std::sync::atomic::Ordering::Relaxed) { "# s".into() } else { format!("{s:.3} s") }
}

fn fmt_time(us: i64) -> String {
    let s = us.div_euclid(1_000_000);
    let ms = us.rem_euclid(1_000_000) / 1000;
    let mut tm: Tm = unsafe { std::mem::zeroed() };
    if unsafe { localtime_r(&s, &mut tm) }.is_null() {
        return format!("{us} us");
    }
    let off = tm.tm_gmtoff / 60;
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {}{:02}:{:02}",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        ms,
        if off < 0 { '-' } else { '+' },
        off.abs() / 60,
        off.abs() % 60
    )
}

/// the memory text without our header line "TIME | role | session SID"
fn body(text: &str) -> &str {
    match text.split_once('\n') {
        Some((first, rest)) if first.contains(" | session ") => rest,
        _ => text,
    }
}

fn one_line(s: &str, max: usize) -> String {
    let flat: String = s.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
    let mut out: String = flat.chars().take(max).collect();
    if flat.chars().count() > max {
        out.push_str(" ...");
    }
    out
}

fn home() -> String {
    std::env::var("HOME").unwrap_or_default()
}

/// the memory root: $XKSM_HOME, else ~/XKS-memories (as the xksm CLI)
fn memory_root() -> String {
    match std::env::var("XKSM_HOME") {
        Ok(p) if !p.is_empty() => p,
        _ => format!("{}/XKS-memories", home()),
    }
}

/// the index: $XKSM_STORE, else ROOT/.index/agents.xksm
fn default_store() -> String {
    match std::env::var("XKSM_STORE") {
        Ok(p) if !p.is_empty() => p,
        _ => format!("{}/.index/agents.xksm", memory_root()),
    }
}

/// ~ at the start of a typed path means the home directory
fn expand(p: &str) -> String {
    match p.strip_prefix("~/") {
        Some(rest) => format!("{}/{rest}", home()),
        None => p.to_string(),
    }
}

// ---- application state -----------------------------------------------------------------------
#[derive(PartialEq, Clone, Copy)]
enum Tab {
    Sessions,
    Search,
    Memories,
    Embedding,
    Store,
}
const TABS: [Tab; 5] = [Tab::Sessions, Tab::Search, Tab::Memories, Tab::Embedding, Tab::Store];
const TAB_NAMES: [&str; 5] = ["1 Sessions", "2 Search", "3 Memories", "4 Embedding", "5 Store"];

#[derive(PartialEq, Clone, Copy)]
enum SearchMode {
    Recall,
    Words,
}

#[derive(PartialEq, Clone, Copy)]
enum Scope {
    Dialogue,
    WithTrace,
    Everything,
}

#[derive(PartialEq)]
enum Input {
    None,
    Query,
    Port,
    Note,
    Import,
}

enum Confirm {
    None,
    ForgetSession(String, String),
    DeleteMemory(i64),
}

/// one line of a memory list (session turns, namespace browser)
struct Row_ {
    id: i64,
    when: String,
    role: String,
    line: String,
    new: bool, // arrived with the latest live update
}

struct Result_ {
    when_us: i64,
    hit: Hit,
    when: String,
    harness: String,
    role: String,
    line: String,
    new: bool,
}

/// figures shown in the header and the Embedding / Store views, read once per store change
/// (not once per frame)
#[derive(Default)]
struct Snap {
    total: i64,
    built: usize,
    dim: usize,
    port: String,
    harness: Vec<(String, i64, i64, bool)>, // name, dialogue, trace, paused
    size: u64,
    mode: u32,
}

struct App {
    st: Store,
    tab: Tab,
    status: String,
    help: bool,
    // live updates: the store's change counter, polled every loop
    live: bool,
    change: i64,
    next_sync: Instant,
    last_change_us: i64,
    last_delta: i64,
    snap: Snap,
    // sessions
    harnesses: Vec<String>,
    harness_ix: usize, // 0 = all
    sessions: Vec<Session>,
    sess_state: Sel,
    turns: Vec<Row_>,
    turns_of: Option<(String, String)>, // the session whose turns are shown
    turn_state: Sel,
    turns_focus: bool,
    // search
    query: String,
    mode: SearchMode,
    scope: Scope,
    results: Vec<Result_>,
    res_state: Sel,
    // memories (namespace browser)
    nss: Vec<NsCount>,
    ns_state: Sel,
    mems: Vec<Row_>,
    mems_of: Option<String>, // the namespace whose memories are shown
    mem_state: Sel,
    mems_focus: bool,
    mems_done: bool, // every memory of the namespace is loaded
    // detail, input, confirm
    detail: Option<Memory>,
    detail_gone: bool, // deleted by another process while open
    detail_hash: Option<HashInfo>,
    verified: Option<(i64, xksm_verify_report)>, // last integrity check: when, result
    sort_new: bool, // search results newest first instead of by relevance
    detail_scroll: u16,
    input: Input,
    buf: String,
    confirm: Confirm,
    harness_state: Sel,
}

fn row_of(m: &Memory) -> Row_ {
    let (_, _, role) = uri_parts(&m.uri);
    Row_ { id: m.id, when: fmt_time(m.when_us), role: if role.is_empty() { "-".into() } else { role }, line: one_line(body(&m.text), 200), new: false }
}

/// the hashes of an open memory, recomputed when it is opened (and at every live update)
struct HashInfo {
    text: String,  // the stored checksum, hex
    text_ok: bool, // the text as read now hashes to it
    source: Option<(String, i64)>, // the source's SHA-256 (hex) and size
    file: Option<(String, Result<bool, ()>)>, // session file (relative to the root): matches the source?
}

fn hex(b: &[u8]) -> String {
    const D: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(b.len() * 2);
    for &x in b {
        s.push(D[(x >> 4) as usize] as char);
        s.push(D[(x & 15) as usize] as char);
    }
    s
}

fn hash_info(st: &Store, m: &Memory) -> HashInfo {
    let src = if m.source_id != 0 { st.source_sha256(m.source_id) } else { None };
    let file = match (&src, st.turn_file(m.source_id)) {
        (Some((sha, _)), Some(rel)) if m.source_id != 0 => {
            let state = Store::file_matches(&format!("{}/{rel}", memory_root()), sha);
            Some((rel, state))
        }
        _ => None,
    };
    HashInfo {
        text: hex(&m.checksum),
        text_ok: Store::sha256(m.text.as_bytes()) == m.checksum,
        source: src.map(|(sha, n)| (hex(&sha), n)),
        file,
    }
}

/// the time a note is stored with: now, or a fixed instant in script mode (exact snapshots)
fn clock_us() -> i64 {
    if DETERMINISTIC.load(std::sync::atomic::Ordering::Relaxed) { 1793887380000000 } else { now_us() }
}

fn now_us() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_micros() as i64).unwrap_or(0)
}

/// a list reloaded by a live update: rows absent before are marked new (unless the list was
/// empty), the selection stays on the same memory id; a selection on the newest end (the last
/// row with `follow_tail`, else the first) stays on that end as rows arrive
fn merge_rows(old: &[Row_], mut new: Vec<Row_>, sel: &mut Sel, follow_tail: bool) -> Vec<Row_> {
    let was: std::collections::HashSet<i64> = old.iter().map(|r| r.id).collect();
    let sel_id = sel.selected().and_then(|i| old.get(i)).map(|r| r.id);
    let at_end = match sel.selected() {
        Some(i) => if follow_tail { i + 1 == old.len() } else { i == 0 },
        None => false,
    };
    if !old.is_empty() {
        for r in new.iter_mut() {
            r.new = !was.contains(&r.id);
        }
    }
    let pos = if at_end && !new.is_empty() {
        Some(if follow_tail { new.len() - 1 } else { 0 })
    } else {
        sel_id.and_then(|id| new.iter().position(|r| r.id == id)).or(if new.is_empty() { None } else { Some(sel.selected().unwrap_or(0).min(new.len() - 1)) })
    };
    sel.select(pos);
    new
}
impl App {
    fn new(st: Store) -> App {
        let mut a = App {
            st,
            tab: Tab::Sessions,
            status: "? help  q quit  Tab or 1-5 views  Enter open  a live updates on/off".into(),
            help: false,
            live: true,
            change: 0,
            next_sync: Instant::now(),
            last_change_us: 0,
            last_delta: 0,
            snap: Snap::default(),
            harnesses: vec![],
            harness_ix: 0,
            sessions: vec![],
            sess_state: Sel::default(),
            turns: vec![],
            turns_of: None,
            turn_state: Sel::default(),
            turns_focus: false,
            query: String::new(),
            mode: SearchMode::Recall,
            scope: Scope::Dialogue,
            results: vec![],
            res_state: Sel::default(),
            nss: vec![],
            ns_state: Sel::default(),
            mems: vec![],
            mems_of: None,
            mem_state: Sel::default(),
            mems_focus: false,
            mems_done: false,
            detail: None,
            detail_gone: false,
            detail_hash: None,
            verified: None,
            sort_new: false,
            detail_scroll: 0,
            input: Input::None,
            buf: String::new(),
            confirm: Confirm::None,
            harness_state: Sel::default(),
        };
        a.change = a.st.change_counter();
        a.reload_sessions();
        a.reload_ns();
        a.reload_snap();
        a
    }

    fn current_harness(&self) -> Option<&str> {
        if self.harness_ix == 0 { None } else { self.harnesses.get(self.harness_ix - 1).map(|s| s.as_str()) }
    }

    /// reload the session list, keeping the selected session (by harness + id)
    fn reload_sessions(&mut self) {
        let keep = self.sess_state.selected().and_then(|i| self.sessions.get(i)).map(|s| (s.harness.clone(), s.id.clone()));
        match self.st.sessions(None) {
            Ok(all) => {
                let mut hs: Vec<String> = all.iter().map(|s| s.harness.clone()).collect();
                hs.sort();
                hs.dedup();
                self.harnesses = hs;
                if self.harness_ix > self.harnesses.len() {
                    self.harness_ix = 0;
                }
                let h = self.current_harness().map(|s| s.to_string());
                self.sessions = all.into_iter().filter(|s| h.as_deref().map_or(true, |x| x == s.harness)).collect();
                let pos = keep.and_then(|(kh, ki)| self.sessions.iter().position(|s| s.harness == kh && s.id == ki));
                self.sess_state.select(if self.sessions.is_empty() { None } else { Some(pos.unwrap_or(0)) });
                if self.harness_state.selected().map_or(true, |i| i >= self.harnesses.len()) {
                    self.harness_state.select(if self.harnesses.is_empty() { None } else { Some(0) });
                }
            }
            Err(e) => self.status = format!("sessions: {e}"),
        }
    }

    /// reload the namespace list, keeping the selected namespace
    fn reload_ns(&mut self) {
        let keep = self.ns_state.selected().and_then(|i| self.nss.get(i)).map(|n| n.ns.clone());
        match self.st.ns_counts() {
            Ok(v) => {
                self.nss = v;
                let pos = keep.and_then(|k| self.nss.iter().position(|n| n.ns == k));
                self.ns_state.select(if self.nss.is_empty() { None } else { Some(pos.unwrap_or(0)) });
            }
            Err(e) => self.status = format!("namespaces: {e}"),
        }
    }

    /// the figures of the header, Embedding and Store views
    fn reload_snap(&mut self) {
        let (built, dim) = self.st.table();
        let meta = std::fs::metadata(&self.st.path).ok();
        let harness = self
            .harnesses
            .iter()
            .map(|h| (h.clone(), self.st.count(Some(h)), self.st.count(Some(&format!("{h}-trace"))), self.st.paused(h)))
            .collect();
        self.snap = Snap {
            total: self.st.count(None),
            built,
            dim,
            port: self.st.port(),
            harness,
            size: meta.as_ref().map(|m| m.len()).unwrap_or(0),
            mode: meta
                .as_ref()
                .map(|m| {
                    use std::os::unix::fs::PermissionsExt;
                    m.permissions().mode() & 0o777
                })
                .unwrap_or(0),
        };
    }

    /// poll the store's change counter; when anything was committed (an agent turn, a note,
    /// another TUI, a delete) reload everything that is on screen. `force` reloads regardless
    /// (key r). Returns true when the screen needs a redraw.
    fn sync(&mut self, force: bool) -> bool {
        if !force && (!self.live || Instant::now() < self.next_sync) {
            return false;
        }
        let c = self.st.change_counter();
        if !force && c == self.change {
            return false;
        }
        let t0 = Instant::now();
        let before = self.snap.total;
        self.change = c;
        self.reload_sessions();
        self.reload_ns();
        self.reload_snap();
        if self.turns_of.is_some() {
            self.load_turns(false);
        }
        if self.mems_of.is_some() {
            self.load_mems_live();
        }
        if !self.results.is_empty() && self.input != Input::Query {
            self.search(true);
        }
        if let Some(m) = &self.detail {
            match self.st.memory(m.id) {
                Some(now) => self.detail_hash = Some(hash_info(&self.st, &now)),
                None => self.detail_gone = true,
            }
        }
        if !force {
            self.last_change_us = now_us();
            self.last_delta = self.snap.total - before;
        }
        /* a writer committing without pause (a bulk import elsewhere) never keeps this view more
         * than a fifth of the time busy reloading: the next poll waits four reload times */
        self.next_sync = Instant::now() + t0.elapsed() * 4;
        true
    }

    /// the turns of the selected session: `fresh` (Enter) focuses them and starts at the
    /// top; a live reload keeps the selection and marks the turns that are new
    fn load_turns(&mut self, fresh: bool) {
        let target = if fresh {
            let Some(i) = self.sess_state.selected() else { return };
            let s = &self.sessions[i];
            (s.harness.clone(), s.id.clone())
        } else {
            match &self.turns_of {
                Some(t) => t.clone(),
                None => return,
            }
        };
        match self.st.session_memories(&target.0, &target.1) {
            Ok(ids) => {
                /* newest first: the latest turn on top, where new ones arrive */
                let rows: Vec<Row_> = ids.iter().rev().filter_map(|&id| self.st.memory(id)).map(|m| row_of(&m)).collect();
                if fresh {
                    self.turns = rows;
                    self.turn_state.select(if self.turns.is_empty() { None } else { Some(0) });
                    self.turns_focus = !self.turns.is_empty();
                    self.status = format!("{} session {}: {} memories", target.0, target.1, self.turns.len());
                } else {
                    let old = std::mem::take(&mut self.turns);
                    self.turns = merge_rows(&old, rows, &mut self.turn_state, false);
                }
                self.turns_of = Some(target);
            }
            Err(e) => self.status = format!("turns: {e}"),
        }
    }

    fn clear_turns(&mut self) {
        self.turns.clear();
        self.turns_of = None;
        self.turns_focus = false;
    }

    fn selected_ns(&self) -> Option<String> {
        self.ns_state.selected().and_then(|i| self.nss.get(i)).map(|n| n.ns.clone())
    }

    /// load the first page (fresh) or the next page of the selected namespace
    fn load_mems(&mut self, fresh: bool) {
        let Some(ns) = self.selected_ns() else { return };
        if fresh {
            self.mems.clear();
            self.mems_done = false;
        }
        if self.mems_done {
            return;
        }
        match self.st.memory_ids(Some(&ns), self.mems.len(), PAGE) {
            Ok(ids) => {
                self.mems_done = ids.len() < PAGE;
                let mut rows: Vec<Row_> = ids.iter().filter_map(|&id| self.st.memory(id)).map(|m| row_of(&m)).collect();
                self.mems.append(&mut rows);
                if fresh {
                    self.mem_state.select(if self.mems.is_empty() { None } else { Some(0) });
                }
                self.mems_focus = !self.mems.is_empty();
                self.mems_of = Some(ns.clone());
                self.status = format!("{ns}: {} memories shown{}", self.mems.len(), if self.mems_done { "" } else { " (more below)" });
            }
            Err(e) => self.status = format!("memories: {e}"),
        }
    }

    /// live reload of the shown namespace: as many rows as were loaded (at least a page), the
    /// newest on top, the selection kept (on top when it was on top)
    fn load_mems_live(&mut self) {
        let Some(ns) = self.mems_of.clone() else { return };
        let want = self.mems.len().max(PAGE);
        if let Ok(ids) = self.st.memory_ids(Some(&ns), 0, want) {
            self.mems_done = ids.len() < want;
            let rows: Vec<Row_> = ids.iter().filter_map(|&id| self.st.memory(id)).map(|m| row_of(&m)).collect();
            let old = std::mem::take(&mut self.mems);
            self.mems = merge_rows(&old, rows, &mut self.mem_state, false);
        }
    }

    fn clear_mems(&mut self) {
        self.mems.clear();
        self.mems_of = None;
        self.mems_focus = false;
    }

    /// namespaces to query: the selected harness, or every harness when none is selected
    fn scope_ns(&self) -> Vec<Option<String>> {
        if self.scope == Scope::Everything {
            return vec![None];
        }
        let hs: Vec<String> = match self.current_harness() {
            Some(h) => vec![h.to_string()],
            None => self.harnesses.clone(),
        };
        let mut v = vec![];
        for h in hs {
            if self.scope == Scope::WithTrace {
                v.push(Some(format!("{h}-trace")));
            }
            v.push(Some(h));
        }
        if v.is_empty() {
            v.push(None); /* no harness sessions yet: the whole store */
        }
        v
    }

    fn run_search(&mut self) {
        self.search(false);
    }

    /// run the query; `live` (a store change) keeps the selection, marks new hits and leaves
    /// the status line alone
    fn search(&mut self, live: bool) {
        if self.query.trim().is_empty() {
            return;
        }
        let t0 = Instant::now();
        let mut hits: Vec<Hit> = vec![];
        for ns in self.scope_ns() {
            let r = match self.mode {
                SearchMode::Recall => self.st.recall(&self.query, ns.as_deref(), 30),
                SearchMode::Words => self.st.words(&self.query, ns.as_deref(), 30),
            };
            match r {
                Ok(mut v) => hits.append(&mut v),
                Err(e) => {
                    self.status = format!("search: {e}");
                    return;
                }
            }
        }
        /* several lists (one per namespace): merge by score, best first */
        hits.sort_by(|x, y| y.score.partial_cmp(&x.score).unwrap_or(std::cmp::Ordering::Equal));
        hits.truncate(30);
        let ms = t0.elapsed().as_secs_f64() * 1000.0;
        let was: std::collections::HashSet<i64> = self.results.iter().map(|r| r.hit.id).collect();
        let sel_id = self.res_state.selected().and_then(|i| self.results.get(i)).map(|r| r.hit.id);
        self.results = hits
            .into_iter()
            .filter_map(|h| {
                let m = self.st.memory(h.id)?;
                let (har, _, role) = uri_parts(&m.uri);
                Some(Result_ {
                    when_us: m.when_us,
                    when: fmt_time(m.when_us),
                    harness: if har.is_empty() { m.ns.clone() } else { har },
                    role: if role.is_empty() { "-".into() } else { role },
                    line: one_line(body(&m.text), 200),
                    new: live && !was.contains(&h.id),
                    hit: h,
                })
            })
            .collect();
        if self.sort_new {
            /* newest first by event time, ties by id (the order of the Memories view) */
            self.results.sort_by(|x, y| y.when_us.cmp(&x.when_us).then(y.hit.id.cmp(&x.hit.id)));
        }
        let pos = if live { sel_id.and_then(|id| self.results.iter().position(|r| r.hit.id == id)) } else { None };
        self.res_state.select(if self.results.is_empty() { None } else { Some(pos.unwrap_or(0)) });
        if !live {
            self.status = format!(
                "{} results in {} ({}, {})",
                self.results.len(),
                fmt_ms(ms),
                if self.mode == SearchMode::Recall { "recall: words + meaning" } else { "words: BM25" },
                scope_name(self.scope)
            );
        }
    }

    fn open_detail(&mut self, id: i64) {
        self.detail = self.st.memory(id);
        self.detail_hash = self.detail.as_ref().map(|m| hash_info(&self.st, m));
        self.detail_scroll = 0;
        self.detail_gone = false;
    }

    /// embed what is new, report it in the status line
    fn embed_new(&mut self, what: String) {
        self.status = match self.st.update() {
            Ok(n) => format!("{what}; {n} new memories embedded"),
            Err(e) => format!("{what}; not embedded yet ({e})"),
        };
    }

    fn export_session(&mut self) {
        let Some(i) = self.sess_state.selected() else { return };
        let s = self.sessions[i].clone();
        let ids = match self.st.session_memories(&s.harness, &s.id) {
            Ok(v) => v,
            Err(e) => {
                self.status = format!("export: {e}");
                return;
            }
        };
        let short: String = s.id.chars().take(8).collect();
        let path = format!("{}/xksm-export-{}-{}.md", home(), s.harness, short);
        let mut out = format!(
            "# {} session {}\n\n{} turns, {} .. {}{}\n",
            s.harness,
            s.id,
            s.turns,
            fmt_time(s.first_us),
            fmt_time(s.last_us),
            if s.cwd.is_empty() { String::new() } else { format!(", {}", s.cwd) }
        );
        for id in &ids {
            if let Some(m) = self.st.memory(*id) {
                let (_, _, role) = uri_parts(&m.uri);
                out.push_str(&format!("\n## {} | {}\n\n{}\n", fmt_time(m.when_us), role, body(&m.text)));
            }
        }
        /* the export holds private conversation text: create it readable by this user only */
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let r = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&path).and_then(|mut f| f.write_all(out.as_bytes()));
        self.status = match r {
            Ok(()) => format!("exported {} memories to {path} ({} bytes, mode 600)", ids.len(), out.len()),
            Err(e) => format!("export {path}: {e}"),
        };
    }
}

fn scope_name(s: Scope) -> &'static str {
    match s {
        Scope::Dialogue => "dialogue",
        Scope::WithTrace => "dialogue + tool calls, results, thinking",
        Scope::Everything => "every harness and document",
    }
}

fn move_sel(state: &mut Sel, len: usize, delta: i64) {
    if len == 0 {
        state.select(None);
        return;
    }
    let cur = state.selected().unwrap_or(0) as i64;
    let next = (cur + delta).clamp(0, len as i64 - 1);
    state.select(Some(next as usize));
}

// ---- drawing ------------------------------------------------------------------------------------

// ---- drawing --------------------------------------------------------------------------------------
const BORDER: Style = Style::fg(DARK);

fn role_color(role: &str) -> u8 {
    match role {
        "user" => GREEN,
        "assistant" => CYAN,
        "thinking" => MAGENTA,
        "note" => YELLOW,
        _ => GRAY,
    }
}

fn seg(s: impl Into<String>, st: Style) -> (String, Style) {
    (s.into(), st)
}

fn draw(scr: &mut Screen, a: &mut App) {
    scr.clear();
    if scr.w < 80 || scr.h < 20 {
        let msg = format!("terminal too small: {}x{}, needs 80x20", scr.w, scr.h);
        let x = scr.w.saturating_sub(msg.len() as u16) / 2;
        scr.text(x, scr.h / 2, &msg, scr.w, Style::fg(YELLOW).bold());
        return;
    }
    let (top, rest) = scr.area().split_v(3);
    let (mid, bottom) = rest.split_v(rest.h.saturating_sub(2));
    let harness = a.current_harness().unwrap_or("all harnesses").to_string();
    let live = if !a.live {
        "live off".to_string()
    } else if a.last_change_us == 0 {
        "live".to_string()
    } else {
        let d = a.last_delta;
        let delta = if d == 0 { String::new() } else { format!(" {d:+}") };
        let clock = if DETERMINISTIC.load(std::sync::atomic::Ordering::Relaxed) { "##:##:##".to_string() } else { fmt_time(a.last_change_us)[11..19].to_string() };
        format!("live, last change {clock}{delta}")
    };
    scr.boxed(top, &format!("XKS-Memory  [{harness}]  {} memories  {live}  {}", a.snap.total, a.st.path), BORDER);
    let mut x = top.x + 2;
    for (i, name) in TAB_NAMES.iter().enumerate() {
        let st = if TABS[i] == a.tab { Style::fg(CYAN).bold() } else { Style::default() };
        x += scr.text(x, top.y + 1, name, top.w.saturating_sub(x - top.x + 2), st);
        x += scr.text(x, top.y + 1, " | ", 3, Style::fg(DARK));
    }
    match a.tab {
        Tab::Sessions => draw_sessions(scr, a, mid),
        Tab::Search => draw_search(scr, a, mid),
        Tab::Memories => draw_memories(scr, a, mid),
        Tab::Embedding => draw_embedding(scr, a, mid),
        Tab::Store => draw_store(scr, a, mid),
    }
    let keys = if a.input != Input::None {
        "type, Enter accept, Esc cancel, Ctrl-U clear"
    } else if !matches!(a.confirm, Confirm::None) {
        "y confirm, any other key cancels"
    } else if a.detail.is_some() {
        "j k / PgUp PgDn scroll  d delete this memory  Esc close"
    } else {
        match a.tab {
            Tab::Sessions => "Enter turns / open  Left back  h harness  x forget  e export  n note  ? help",
            Tab::Search => "/ query  m recall|words  s scope  o newest|best first  h harness  Enter open  d delete  n note  ? help",
            Tab::Memories => "Enter memories / open  Left back  d delete  i import file  n note  ? help",
            Tab::Embedding => "b rebuild table  u embed missing  p set port  ? help",
            Tab::Store => "Up Down harness  P pause or resume logging  V verify every hash  ? help",
        }
    };
    scr.text(bottom.x, bottom.y, &a.status, bottom.w, Style::fg(YELLOW));
    scr.text(bottom.x, bottom.y + 1, keys, bottom.w, Style::fg(DARK));
    if let Some(m) = &a.detail {
        draw_detail(scr, m, a.detail_scroll, a.detail_gone, a.detail_hash.as_ref());
    }
    match a.input {
        Input::Query => draw_input(scr, "search query", &a.buf),
        Input::Port => draw_input(scr, "port file path (tools/port_build output)", &a.buf),
        Input::Note => draw_input(scr, "note (stored in namespace notes with the current time)", &a.buf),
        Input::Import => {
            let ns = import_ns(a);
            draw_input(scr, &format!("file to import into namespace {ns} (text, Markdown, JSON, JSONL, CSV)"), &a.buf)
        }
        Input::None => {}
    }
    match &a.confirm {
        Confirm::ForgetSession(h, s) => draw_confirm(scr, &format!("Forget {h} session {s}? Every memory of it is deleted (secure delete). y / n")),
        Confirm::DeleteMemory(id) => draw_confirm(scr, &format!("Delete memory {id}? y / n")),
        Confirm::None => {}
    }
    if a.help {
        draw_help(scr);
    }
}

/// target namespace of an import: the selected namespace on the Memories tab, unless it belongs
/// to an agent harness (its dialogue or -trace namespace: documents there would be recalled
/// into that harness's prompts); otherwise "documents"
fn import_ns(a: &App) -> String {
    let harness_ns = |ns: &str| a.harnesses.iter().any(|h| ns == h || ns.strip_suffix("-trace") == Some(h.as_str()));
    match (a.tab, a.selected_ns()) {
        (Tab::Memories, Some(ns)) if !harness_ns(&ns) => ns,
        _ => "documents".into(),
    }
}

fn rows_items(rows: &[Row_]) -> Vec<Item> {
    rows.iter()
        .map(|r| {
            vec![vec![
                if r.new { seg("+", Style::fg(YELLOW).bold()) } else { seg(" ", Style::default()) },
                seg(format!("{} ", &r.when[..23.min(r.when.len())]), Style::fg(DARK)),
                seg(format!("{:<11} ", r.role), Style::fg(role_color(&r.role))),
                seg(r.line.clone(), Style::default()),
            ]]
        })
        .collect()
}

fn draw_sessions(scr: &mut Screen, a: &mut App, area: Rect) {
    let (left, right) = area.split_h(42);
    let items: Vec<Item> = a
        .sessions
        .iter()
        .map(|s| {
            vec![
                vec![seg(format!("{:<12}", s.harness), Style::fg(CYAN)), seg(format!(" {}", one_line(&s.id, 40)), Style::default())],
                vec![seg(
                    format!("{} turns  {} .. {}  {}", s.turns, fmt_time(s.first_us), &fmt_time(s.last_us)[11..19], one_line(&s.cwd, 40)),
                    Style::fg(GRAY),
                )],
            ]
        })
        .collect();
    scr.boxed(left, &format!("Sessions ({})", a.sessions.len()), BORDER);
    list(scr, left.inner(), &items, &mut a.sess_state, !a.turns_focus);
    let rt = if a.turns.is_empty() { "Turns (Enter on a session)".to_string() } else { format!("Turns ({})", a.turns.len()) };
    scr.boxed(right, &rt, BORDER);
    list(scr, right.inner(), &rows_items(&a.turns), &mut a.turn_state, a.turns_focus);
}

fn rank(r: usize) -> String {
    if r == 0 { "-".into() } else { r.to_string() }
}

fn draw_search(scr: &mut Screen, a: &mut App, area: Rect) {
    let (q, res) = area.split_v(3);
    let mode = if a.mode == SearchMode::Recall { "recall (words + meaning, port expansion)" } else { "words (BM25)" };
    scr.boxed(q, &format!("Query  [{mode}]  [{}]", scope_name(a.scope)), BORDER);
    let qi = q.inner();
    let used = scr.text(qi.x, qi.y, "> ", 2, Style::fg(CYAN));
    scr.text(qi.x + used, qi.y, &a.query, qi.w.saturating_sub(used), Style::default());
    let items: Vec<Item> = a
        .results
        .iter()
        .map(|r| {
            let score = if a.mode == SearchMode::Recall {
                format!("rrf {:.4}  lex #{}  vec #{} cos {:.3}", r.hit.score, rank(r.hit.lexical_rank), rank(r.hit.vector_rank), r.hit.vector_score)
            } else {
                format!("bm25 {:.2}", r.hit.score)
            };
            vec![
                vec![
                    if r.new { seg("+", Style::fg(YELLOW).bold()) } else { seg(" ", Style::default()) },
                    seg(format!("{} ", r.when), Style::fg(DARK)),
                    seg(format!("{} ", r.harness), Style::fg(CYAN)),
                    seg(format!("{} ", r.role), Style::fg(role_color(&r.role))),
                    seg(score, Style::fg(GRAY)),
                ],
                vec![seg(r.line.clone(), Style::default())],
            ]
        })
        .collect();
    let order = if a.sort_new { "newest first" } else { "best first" };
    scr.boxed(res, &format!("Results ({}, {order})", a.results.len()), BORDER);
    list(scr, res.inner(), &items, &mut a.res_state, true);
}

fn draw_memories(scr: &mut Screen, a: &mut App, area: Rect) {
    let (left, right) = area.split_h(34);
    let items: Vec<Item> = a
        .nss
        .iter()
        .map(|n| {
            let pct = if n.memories > 0 { n.embedded as f64 * 100.0 / n.memories as f64 } else { 0.0 };
            vec![vec![
                seg(format!("{:<22}", n.ns), Style::fg(CYAN)),
                seg(format!("{:>9}", n.memories), Style::default()),
                seg(format!("  {pct:>6.2} % embedded"), Style::fg(GRAY)),
            ]]
        })
        .collect();
    scr.boxed(left, &format!("Namespaces ({})", a.nss.len()), BORDER);
    list(scr, left.inner(), &items, &mut a.ns_state, !a.mems_focus);
    let t = match a.selected_ns() {
        Some(ns) if !a.mems.is_empty() => format!("{ns}: newest first ({}{})", a.mems.len(), if a.mems_done { "" } else { "+" }),
        Some(_) => "Memories (Enter on a namespace)".to_string(),
        None => "Memories".to_string(),
    };
    scr.boxed(right, &t, BORDER);
    list(scr, right.inner(), &rows_items(&a.mems), &mut a.mem_state, a.mems_focus);
}

fn draw_embedding(scr: &mut Screen, a: &mut App, area: Rect) {
    let (info, cov) = area.split_v(9);
    let (built, dim) = (a.snap.built, a.snap.dim);
    let live = a.snap.total;
    let port = a.snap.port.clone();
    let growth = if built > 0 { (live as f64 / built as f64 - 1.0) * 100.0 } else { 0.0 };
    scr.boxed(info, "Embedding", BORDER);
    let i = info.inner();
    let lines: [(&str, String); 6] = [
        ("Word-vector table  ", if built > 0 { format!("built from {built} memories, dimension {dim}") } else { "not built yet (the hooks build it at 200 memories; b builds it now)".into() }),
        ("", format!("  store now holds {live} memories ({growth:+.1} % since the build; the hooks rebuild at +25 %)")),
        ("", "  count-based: PPMI over word co-occurrence, randomized SVD on the CPU (AVX2/FMA, 4-core pool); no neural model".into()),
        ("Port  ", if port.is_empty() { "none recorded (recall works without expansion; p sets one)".into() } else { port }),
        ("", "  static word vectors made once offline by an embedding model; used to relate query words to store words".into()),
        ("Recall  ", "hybrid: BM25 on the typed query (own thread) + RRF with the lattice vector scan over the port-expanded query".into()),
    ];
    for (k, (head, text)) in lines.iter().enumerate() {
        let y = i.y + k as u16;
        let used = scr.text(i.x, y, head, i.w, Style::default().bold());
        scr.text(i.x + used, y, text, i.w.saturating_sub(used), Style::default());
    }
    scr.boxed(cov, "Embedding coverage (memories without a vector have no word known to the table)", BORDER);
    let c = cov.inner();
    let cols = [0u16, c.w * 40 / 100, c.w * 60 / 100, c.w * 80 / 100];
    let head = ["namespace", "memories", "embedded", "coverage"];
    for (j, h) in head.iter().enumerate() {
        scr.text(c.x + cols[j], c.y, h, c.w / 4, Style::default().bold());
    }
    for (k, n) in a.nss.iter().enumerate().take(c.h.saturating_sub(1) as usize) {
        let pct = if n.memories > 0 { n.embedded as f64 * 100.0 / n.memories as f64 } else { 0.0 };
        let vals = [n.ns.clone(), n.memories.to_string(), n.embedded.to_string(), format!("{pct:.2} %")];
        for (j, v) in vals.iter().enumerate() {
            scr.text(c.x + cols[j], c.y + 1 + k as u16, v, c.w / 4, Style::default());
        }
    }
}

fn draw_store(scr: &mut Screen, a: &mut App, area: Rect) {
    let (info, hs) = area.split_v(8);
    let ci = a.st.cache();
    let (size, mode) = (a.snap.size, a.snap.mode);
    let private = mode & 0o077 == 0;
    scr.boxed(info, "Store", BORDER);
    let i = info.inner();
    let lines = [
        format!("store {}   session files {}", a.st.path, memory_root()),
        format!("library {}, schema {}, file {:.1} MB", Store::version(), a.st.schema(), size as f64 / 1e6),
        format!("{} memories now, {} sessions in view", a.snap.total, a.sessions.len()),
        format!(
            "RAM cache of this process: {} % of records, capacity {}, resident {}, {:.1} MB, hits {}, misses {}",
            ci.percent, ci.capacity, ci.resident, ci.bytes as f64 / 1e6, ci.hits, ci.misses
        ),
    ];
    for (k, l) in lines.iter().enumerate() {
        scr.text(i.x, i.y + k as u16, l, i.w, Style::default());
    }
    scr.text(
        i.x,
        i.y + 4,
        &format!("everything stays on this machine; store file mode {mode:o}: {}", if private { "private to this user" } else { "READABLE BY OTHER USERS (chmod 600 it)" }),
        i.w,
        Style::fg(if private { GREEN } else { RED }),
    );
    let integrity = match &a.verified {
        None => ("integrity: V recomputes the SHA-256 of every memory, source and session file".to_string(), Style::fg(GRAY)),
        Some((at, r)) => {
            let clock = if DETERMINISTIC.load(std::sync::atomic::Ordering::Relaxed) { "##:##:##".to_string() } else { fmt_time(*at)[11..19].to_string() };
            let bad = r.memory_bad + r.source_bad + r.file_bad + r.file_missing;
            let text = format!(
                "integrity {clock}: {} memories, {} sources, {} session files: {} in {}",
                r.memories,
                r.sources,
                r.files,
                if bad == 0 { "every SHA-256 matches".to_string() } else { format!("MISMATCH: {} memories, {} sources, {} files differ, {} files missing", r.memory_bad, r.source_bad, r.file_bad, r.file_missing) },
                fmt_s(r.seconds)
            );
            (text, if bad == 0 { Style::fg(GREEN) } else { Style::fg(RED).bold() })
        }
    };
    scr.text(i.x, i.y + 5, &integrity.0, i.w, integrity.1);
    let items: Vec<Item> = a
        .snap
        .harness
        .iter()
        .map(|(h, dialogue, trace, p)| {
            vec![vec![
                seg(format!("{h:<20}"), Style::fg(CYAN)),
                seg(format!("{dialogue:>9} dialogue  {trace:>9} trace  "), Style::fg(GRAY)),
                seg(if *p { "logging PAUSED" } else { "logging active" }, Style::fg(if *p { RED } else { GREEN })),
            ]]
        })
        .collect();
    scr.boxed(hs, "Harnesses", BORDER);
    list(scr, hs.inner(), &items, &mut a.harness_state, true);
}

fn popup(scr: &mut Screen, w: u16, h: u16, title: &str, st: Style) -> Rect {
    let r = scr.area().centered(w, h);
    scr.fill(r, Style::default());
    scr.boxed(r, title, st);
    r.inner()
}

fn draw_detail(scr: &mut Screen, m: &Memory, scroll: u16, gone: bool, hash: Option<&HashInfo>) {
    let (h, s, r) = uri_parts(&m.uri);
    let head = format!(
        "memory {}  |  {}  |  {}  |  {}  |  session {}{}",
        m.id,
        fmt_time(m.when_us),
        if h.is_empty() { m.ns.as_str() } else { h.as_str() },
        if r.is_empty() { "-" } else { r.as_str() },
        if s.is_empty() { "-" } else { s.as_str() },
        if gone { "  |  DELETED meanwhile (by another process)" } else { "" }
    );
    let (w, hh) = (scr.w * 9 / 10, scr.h * 9 / 10);
    let i = popup(scr, w, hh, &head, if gone { Style::fg(RED) } else { BORDER });
    /* three hash lines, a rule, then the text */
    let ok = Style::fg(GREEN);
    let bad = Style::fg(RED).bold();
    let mut rows: Vec<Vec<(String, Style)>> = vec![];
    if let Some(hi) = hash {
        rows.push(vec![
            seg("memory SHA-256  ", Style::fg(DARK)),
            seg(hi.text.clone(), Style::default()),
            if hi.text_ok { seg("  verified: the text hashes to it", ok) } else { seg("  MISMATCH: the text does not hash to it", bad) },
        ]);
        rows.push(match &hi.source {
            Some((sha, n)) => vec![seg("source SHA-256  ", Style::fg(DARK)), seg(sha.clone(), Style::default()), seg(format!("  {n} bytes"), Style::fg(GRAY))],
            None => vec![seg("source          ", Style::fg(DARK)), seg("none (a note or a direct memory: the checksum above is its only hash)", Style::fg(GRAY))],
        });
        rows.push(match &hi.file {
            Some((rel, st)) => vec![
                seg("session file    ", Style::fg(DARK)),
                seg(rel.clone(), Style::default()),
                match st {
                    Ok(true) => seg("  verified: the file hashes to the source", ok),
                    Ok(false) => seg("  DIFFERS from the source (xksm mirror rewrites it)", bad),
                    Err(()) => seg("  MISSING (xksm mirror rewrites it)", bad),
                },
            ],
            None => vec![seg("session file    ", Style::fg(DARK)), seg("none", Style::fg(GRAY))],
        });
    }
    let mut y = i.y;
    for row in &rows {
        let mut x = i.x;
        for (t, st) in row {
            x += scr.text(x, y, t, (i.x + i.w).saturating_sub(x), *st);
        }
        y += 1;
    }
    if !rows.is_empty() {
        for x in i.x..i.x + i.w {
            scr.text(x, y, "─", 1, Style::fg(DARK));
        }
        y += 1;
    }
    let text = Rect { x: i.x, y, w: i.w, h: (i.y + i.h).saturating_sub(y) };
    paragraph(scr, text, body(&m.text), scroll as usize, Style::default());
}

fn draw_input(scr: &mut Screen, label: &str, buf: &str) {
    let i = popup(scr, 100, 3, label, Style::fg(CYAN));
    /* keep the end of a long entry visible */
    let w = str_width(buf) + 1;
    let tail: String = if w > i.w as usize {
        let mut acc = 0usize;
        let mut chars: Vec<char> = buf.chars().rev().take_while(|c| { acc += term::char_width(*c); acc < i.w as usize }).collect();
        chars.reverse();
        chars.into_iter().collect()
    } else {
        buf.to_string()
    };
    let used = scr.text(i.x, i.y, &tail, i.w, Style::default());
    scr.text(i.x + used, i.y, "_", 1, Style::fg(CYAN));
}

fn draw_confirm(scr: &mut Screen, msg: &str) {
    let i = popup(scr, 90, 4, "Confirm", Style::fg(RED));
    paragraph(scr, i, msg, 0, Style::fg(RED));
}

const HELP: &[(&str, &str)] = &[
    ("Views", "Tab / Shift-Tab or 1-5: Sessions, Search, Memories, Embedding, Store"),
    ("Move", "Up Down or j k, PgUp PgDn, Home End; Enter opens; Left or Esc goes back"),
    ("Everywhere", "n add a note   a live updates (200 ms poll, new rows +)   r reload   h cycle harness   q quit   ? this help"),
    ("Sessions", "Enter: turns of the session, newest first (new ones arrive on top), Enter again: full text   x forget   e export"),
    ("Search", "/ query   m recall (words + meaning) or words (BM25)   s scope: dialogue, + trace, every harness   o newest / best first"),
    ("Memories", "Enter: memories of the namespace, newest first (more load at the end)   i import a file (selected namespace, or documents for harness namespaces)"),
    ("Detail", "SHA-256 of the text, its source and session file, each recomputed and checked   j k PgUp PgDn scroll   d delete"),
    ("Embedding", "b rebuild the word-vector table   u embed memories that have no vector   p record a port file"),
    ("Store", "P pause or resume logging for the selected harness   V verify: recompute every SHA-256 of the store"),
    ("Data", "everything stays on this machine; deletes are secure deletes; exports are created mode 600"),
];

fn draw_help(scr: &mut Screen) {
    let i = popup(scr, 130, HELP.len() as u16 * 2 + 3, "XKS-Memory: keys (any key closes)", Style::fg(CYAN));
    for (k, (name, text)) in HELP.iter().enumerate() {
        let y = i.y + 2 * k as u16;
        let used = scr.text(i.x, y, &format!("{name:<11}"), 11, Style::fg(CYAN).bold());
        scr.text(i.x + used, y, text, i.w.saturating_sub(used), Style::default());
    }
}

// ---- input handling -------------------------------------------------------------------------------
/// returns false to quit
fn key(a: &mut App, k: Key) -> bool {
    if k == Key::CtrlC {
        return false;
    }
    if a.help {
        a.help = false;
        return true;
    }
    if a.input != Input::None {
        match k {
            Key::Esc => a.input = Input::None,
            Key::Enter => {
                let v = std::mem::take(&mut a.buf);
                let which = std::mem::replace(&mut a.input, Input::None);
                accept_input(a, which, v);
            }
            Key::Backspace => {
                a.buf.pop();
            }
            Key::CtrlU => a.buf.clear(),
            Key::CtrlW => {
                /* delete the last word and the spaces before it */
                let t = a.buf.trim_end().len();
                let cut = a.buf[..t].rfind(char::is_whitespace).map_or(0, |p| p + 1);
                a.buf.truncate(cut);
            }
            Key::Char(ch) => a.buf.push(ch),
            Key::Tab => a.buf.push(' '),
            _ => {}
        }
        return true;
    }
    if !matches!(a.confirm, Confirm::None) {
        let c = std::mem::replace(&mut a.confirm, Confirm::None);
        if k == Key::Char('y') {
            match c {
                Confirm::ForgetSession(h, s) => match a.st.forget(&h, &s) {
                    Ok(n) => {
                        a.status = format!("forgot {h} session {s}: {n} memories deleted");
                        a.clear_turns();
                        a.sync(true);
                    }
                    Err(e) => a.status = format!("forget: {e}"),
                },
                Confirm::DeleteMemory(id) => match a.st.delete(id) {
                    Ok(()) => {
                        a.status = format!("memory {id} deleted");
                        a.detail = None;
                        a.results.retain(|r| r.hit.id != id);
                        a.turns.retain(|t| t.id != id);
                        a.mems.retain(|t| t.id != id);
                        a.reload_ns();
                    }
                    Err(e) => a.status = format!("delete: {e}"),
                },
                Confirm::None => {}
            }
        } else {
            a.status = "cancelled".into();
        }
        return true;
    }
    if a.detail.is_some() {
        match k {
            Key::Esc | Key::Char('q') => a.detail = None,
            Key::Down | Key::Char('j') => a.detail_scroll = a.detail_scroll.saturating_add(1),
            Key::Up | Key::Char('k') => a.detail_scroll = a.detail_scroll.saturating_sub(1),
            Key::PgDn => a.detail_scroll = a.detail_scroll.saturating_add(20),
            Key::PgUp => a.detail_scroll = a.detail_scroll.saturating_sub(20),
            Key::Home => a.detail_scroll = 0,
            Key::Char('d') => {
                if let Some(m) = &a.detail {
                    a.confirm = Confirm::DeleteMemory(m.id);
                }
            }
            _ => {}
        }
        return true;
    }
    match k {
        Key::Char('q') => return false,
        Key::Char('?') => a.help = true,
        Key::Tab => {
            let i = TABS.iter().position(|t| *t == a.tab).unwrap_or(0);
            a.tab = TABS[(i + 1) % TABS.len()];
        }
        Key::BackTab => {
            let i = TABS.iter().position(|t| *t == a.tab).unwrap_or(0);
            a.tab = TABS[(i + TABS.len() - 1) % TABS.len()];
        }
        Key::Char(ch @ '1'..='5') => a.tab = TABS[(ch as u8 - b'1') as usize],
        Key::Char('r') => {
            a.sync(true);
            a.status = "reloaded".into();
        }
        Key::Char('a') => {
            a.live = !a.live;
            a.status = format!("live updates {}", if a.live { "on (the store is polled every 200 ms)" } else { "off (r reloads)" });
        }
        Key::Char('n') => {
            a.buf.clear();
            a.input = Input::Note;
        }
        Key::Char('h') if a.tab != Tab::Store => {
            a.harness_ix = (a.harness_ix + 1) % (a.harnesses.len() + 1);
            a.reload_sessions();
            a.clear_turns();
            a.status = format!("harness: {}", a.current_harness().unwrap_or("all"));
        }
        _ => match a.tab {
            Tab::Sessions => sessions_key(a, k),
            Tab::Search => search_key(a, k),
            Tab::Memories => memories_key(a, k),
            Tab::Embedding => embedding_key(a, k),
            Tab::Store => store_key(a, k),
        },
    }
    true
}

fn accept_input(a: &mut App, which: Input, v: String) {
    match which {
        Input::Query => {
            a.query = v;
            a.run_search();
        }
        Input::Port => match a.st.set_port(&expand(v.trim())) {
            Ok(()) => a.status = format!("port recorded: {}", v.trim()),
            Err(e) => a.status = format!("port: {e}"),
        },
        Input::Note => {
            let t = v.trim();
            if t.is_empty() {
                a.status = "empty note: nothing stored".into();
                return;
            }
            match a.st.add_note("notes", t, clock_us()) {
                Ok((id, dup)) => {
                    let what = if dup { format!("identical note already stored (memory {id})") } else { format!("note stored as memory {id} in notes") };
                    a.embed_new(what);
                    a.reload_ns();
                }
                Err(e) => a.status = format!("note: {e}"),
            }
        }
        Input::Import => {
            let ns = import_ns(a);
            let path = expand(v.trim());
            match a.st.import_file(&path, &ns) {
                Ok((chunks, added, dups)) => {
                    a.embed_new(format!("imported {path} into {ns}: {chunks} chunks, {added} new, {dups} duplicates"));
                    a.reload_ns();
                    if a.tab == Tab::Memories && a.mems_focus {
                        a.load_mems(true);
                    }
                }
                Err(e) => a.status = format!("import: {e}"),
            }
        }
        Input::None => {}
    }
}

/// list movement shared by every list: returns the step, or None for other keys
fn step_of(k: Key) -> Option<i64> {
    match k {
        Key::Down | Key::Char('j') => Some(1),
        Key::Up | Key::Char('k') => Some(-1),
        Key::PgDn => Some(20),
        Key::PgUp => Some(-20),
        Key::Home => Some(i64::MIN / 2),
        Key::End => Some(i64::MAX / 2),
        _ => None,
    }
}

fn sessions_key(a: &mut App, k: Key) {
    let (len_s, len_t) = (a.sessions.len(), a.turns.len());
    if let Some(d) = step_of(k) {
        if a.turns_focus {
            move_sel(&mut a.turn_state, len_t, d)
        } else {
            move_sel(&mut a.sess_state, len_s, d);
            a.clear_turns();
        }
        return;
    }
    match k {
        Key::Enter | Key::Right => {
            if a.turns_focus {
                if let Some(i) = a.turn_state.selected() {
                    let id = a.turns[i].id;
                    a.open_detail(id);
                }
            } else {
                a.load_turns(true);
            }
        }
        Key::Left | Key::Esc => a.turns_focus = false,
        Key::Char('x') => {
            if let Some(i) = a.sess_state.selected() {
                let s = &a.sessions[i];
                a.confirm = Confirm::ForgetSession(s.harness.clone(), s.id.clone());
            }
        }
        Key::Char('e') => a.export_session(),
        _ => {}
    }
}

fn search_key(a: &mut App, k: Key) {
    let len = a.results.len();
    if let Some(d) = step_of(k) {
        move_sel(&mut a.res_state, len, d);
        return;
    }
    match k {
        Key::Char('/') => {
            a.buf = a.query.clone();
            a.input = Input::Query;
        }
        Key::Char('o') => {
            a.sort_new = !a.sort_new;
            a.run_search();
        }
        Key::Char('m') => {
            a.mode = if a.mode == SearchMode::Recall { SearchMode::Words } else { SearchMode::Recall };
            a.run_search();
        }
        Key::Char('s') => {
            a.scope = match a.scope {
                Scope::Dialogue => Scope::WithTrace,
                Scope::WithTrace => Scope::Everything,
                Scope::Everything => Scope::Dialogue,
            };
            a.run_search();
        }
        Key::Enter => {
            if let Some(i) = a.res_state.selected() {
                let id = a.results[i].hit.id;
                a.open_detail(id);
            }
        }
        Key::Char('d') => {
            if let Some(i) = a.res_state.selected() {
                a.confirm = Confirm::DeleteMemory(a.results[i].hit.id);
            }
        }
        _ => {}
    }
}

fn memories_key(a: &mut App, k: Key) {
    let (len_n, len_m) = (a.nss.len(), a.mems.len());
    if let Some(d) = step_of(k) {
        if a.mems_focus {
            move_sel(&mut a.mem_state, len_m, d);
            /* reaching the end loads the next page */
            if a.mem_state.selected().map_or(false, |i| i + 1 >= a.mems.len()) && !a.mems_done {
                a.load_mems(false);
            }
        } else {
            move_sel(&mut a.ns_state, len_n, d);
            a.clear_mems();
        }
        return;
    }
    match k {
        Key::Enter | Key::Right => {
            if a.mems_focus {
                if let Some(i) = a.mem_state.selected() {
                    let id = a.mems[i].id;
                    a.open_detail(id);
                }
            } else {
                a.load_mems(true);
            }
        }
        Key::Left | Key::Esc => a.mems_focus = false,
        Key::Char('d') => {
            if a.mems_focus {
                if let Some(i) = a.mem_state.selected() {
                    a.confirm = Confirm::DeleteMemory(a.mems[i].id);
                }
            }
        }
        Key::Char('i') => {
            a.buf.clear();
            a.input = Input::Import;
        }
        _ => {}
    }
}

fn embedding_key(a: &mut App, k: Key) {
    match k {
        Key::Char('b') => {
            a.status = match a.st.build() {
                Ok(r) => format!(
                    "built from {} memories ({} embedded), vocabulary {}, {} non-zeros; SVD {}, total {}",
                    r.memories, r.embedded, r.vocab, r.nnz, fmt_s(r.t_svd), fmt_s(r.t_total)
                ),
                Err(e) => format!("build: {e}"),
            };
            a.reload_ns();
        }
        Key::Char('u') => {
            a.embed_new("embedding pass".into());
            a.reload_ns();
        }
        Key::Char('p') => {
            a.buf = a.st.port();
            a.input = Input::Port;
        }
        _ => {}
    }
}

fn store_key(a: &mut App, k: Key) {
    let len = a.harnesses.len();
    if let Some(d) = step_of(k) {
        move_sel(&mut a.harness_state, len, d);
        return;
    }
    if let Key::Char('V') | Key::Char('v') = k {
        match a.st.verify(&memory_root()) {
            Ok(r) => {
                let bad = r.memory_bad + r.source_bad + r.file_bad + r.file_missing;
                a.status = if bad == 0 { format!("verified: every SHA-256 matches ({} memories, {} sources, {} files)", r.memories, r.sources, r.files) } else { format!("verify: {bad} MISMATCHES (first memory {}, source {}, file of source {})", r.first_bad_memory, r.first_bad_source, r.first_bad_file_source) };
                a.verified = Some((now_us(), r));
            }
            Err(e) => a.status = format!("verify: {e}"),
        }
        return;
    }
    if let Key::Char('P') | Key::Char('p') = k {
        if let Some(i) = a.harness_state.selected() {
            let h = a.harnesses[i].clone();
            let on = !a.st.paused(&h);
            a.status = match a.st.set_paused(&h, on) {
                Ok(()) => format!("{h}: logging {}", if on { "paused" } else { "active" }),
                Err(e) => format!("pause: {e}"),
            };
            a.reload_snap();
        }
    }
}

fn idle(a: &App) -> bool {
    a.input == Input::None && matches!(a.confirm, Confirm::None) && a.detail.is_none() && !a.help
}

/// a key name of a script line: one character, or Enter Esc Tab BackTab Backspace Delete Up
/// Down Left Right PgUp PgDn Home End CtrlC CtrlU CtrlW
fn key_name(s: &str) -> Option<Key> {
    let mut it = s.chars();
    if let (Some(c), None) = (it.next(), it.next()) {
        return Some(Key::Char(c));
    }
    Some(match s {
        "Enter" => Key::Enter,
        "Esc" => Key::Esc,
        "Tab" => Key::Tab,
        "BackTab" => Key::BackTab,
        "Backspace" => Key::Backspace,
        "Delete" => Key::Delete,
        "Up" => Key::Up,
        "Down" => Key::Down,
        "Left" => Key::Left,
        "Right" => Key::Right,
        "PgUp" => Key::PgUp,
        "PgDn" => Key::PgDn,
        "Home" => Key::Home,
        "End" => Key::End,
        "CtrlC" => Key::CtrlC,
        "CtrlU" => Key::CtrlU,
        "CtrlW" => Key::CtrlW,
        _ => return None,
    })
}

fn hex_bytes(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

/// headless run of a key script (tests, reproducible bug reports): every line is a key name,
/// `Text:...` (typed characters), `Bytes:HEX` (raw terminal input through the decoder),
/// `Sync` (one live-update poll, as the interactive loop makes every 200 ms), `Exec:CMD`
/// (sh -c CMD, e.g. another process writing to the store), `Snap:NAME` (the screen to
/// DIR/NAME.txt) or a comment (#). Durations print as # so snapshots are exact.
fn run_script(mut app: App, script: &str, dir: &str, w: u16, h: u16) -> Result<usize, String> {
    DETERMINISTIC.store(true, std::sync::atomic::Ordering::Relaxed);
    let text = std::fs::read_to_string(script).map_err(|e| format!("{script}: {e}"))?;
    std::fs::create_dir_all(dir).map_err(|e| format!("{dir}: {e}"))?;
    let mut scr = Screen::headless(w, h);
    let mut snaps = 0usize;
    draw(&mut scr, &mut app);
    scr.flush();
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim_end();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let keys: Vec<Key> = if let Some(t) = line.strip_prefix("Text:") {
            t.chars().map(Key::Char).collect()
        } else if let Some(hx) = line.strip_prefix("Bytes:") {
            term::decode(&hex_bytes(hx).ok_or(format!("{script}:{}: bad hex", n + 1))?)
        } else if line == "Sync" {
            app.next_sync = Instant::now();
            app.sync(false);
            vec![]
        } else if let Some(cmd) = line.strip_prefix("Exec:") {
            let st = std::process::Command::new("sh").arg("-c").arg(cmd).status().map_err(|e| format!("{cmd}: {e}"))?;
            if !st.success() {
                return Err(format!("{script}:{}: {cmd}: {st}", n + 1));
            }
            vec![]
        } else if let Some(name) = line.strip_prefix("Snap:") {
            draw(&mut scr, &mut app);
            scr.flush();
            let p = format!("{dir}/{name}.txt");
            std::fs::write(&p, scr.dump()).map_err(|e| format!("{p}: {e}"))?;
            snaps += 1;
            continue;
        } else {
            vec![key_name(line).ok_or(format!("{script}:{}: unknown key {line}", n + 1))?]
        };
        for k in keys {
            if !key(&mut app, k) {
                return Ok(snaps);
            }
        }
        draw(&mut scr, &mut app);
        scr.flush();
    }
    Ok(snaps)
}

fn usage() {
    println!(
        "usage: xksm-tui [STORE]   (default: $XKSM_STORE, else ~/XKS-memories/.index/agents.xksm); press ? inside for keys\n       \
         xksm-tui --script FILE --snapshot DIR [--size WxH] [STORE]   headless run of a key script (tests)"
    );
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (mut path, mut script, mut snapdir, mut size) = (None, None, None, (200u16, 50u16));
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        let val = args.get(i + 1).cloned();
        match a {
            "-h" | "--help" => return usage(),
            "--script" | "--snapshot" | "--size" => {
                let Some(v) = val else {
                    eprintln!("xksm-tui: {a} needs a value");
                    std::process::exit(2);
                };
                match a {
                    "--script" => script = Some(v),
                    "--snapshot" => snapdir = Some(v),
                    _ => {
                        let wh: Vec<u16> = v.split('x').filter_map(|t| t.parse().ok()).collect();
                        if wh.len() != 2 || wh[0] < 20 || wh[1] < 6 {
                            eprintln!("xksm-tui: --size WxH, at least 20x6");
                            std::process::exit(2);
                        }
                        size = (wh[0], wh[1]);
                    }
                }
                i += 2;
                continue;
            }
            _ if a.starts_with("--") => {
                eprintln!("xksm-tui: unknown option {a}");
                usage();
                std::process::exit(2);
            }
            _ => path = Some(a.to_string()),
        }
        i += 1;
    }
    let path = path.unwrap_or_else(default_store);
    let st = match Store::open(&path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("xksm-tui: {e}");
            std::process::exit(1);
        }
    };
    let mut app = App::new(st);
    if let Some(sc) = script {
        let dir = snapdir.unwrap_or_else(|| ".".into());
        match run_script(app, &sc, &dir, size.0, size.1) {
            Ok(n) => println!("xksm-tui: {sc}: {n} snapshots in {dir}"),
            Err(e) => {
                eprintln!("xksm-tui: {e}");
                std::process::exit(1);
            }
        }
        return;
    }
    if std::env::var("TERM").is_ok_and(|t| t == "dumb") || !unsafe { isatty(0) == 1 && isatty(1) == 1 } {
        eprintln!("xksm-tui: needs an interactive terminal (stdin and stdout a tty, TERM not dumb)");
        std::process::exit(1);
    }
    term::install_panic_restore();
    let mut scr = match Screen::open() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("xksm-tui: the terminal cannot enter raw mode: {e}");
            std::process::exit(1);
        }
    };
    let mut dirty = true;
    loop {
        if app.sync(false) {
            dirty = true; /* an agent, a note or another process changed the store */
        }
        if scr.resize() {
            dirty = true;
        }
        if dirty {
            draw(&mut scr, &mut app);
            scr.flush();
            dirty = false;
        }
        for k in term::read_keys(POLL_MS) {
            dirty = true;
            /* long actions: show the "working" line before the work starts */
            if app.tab == Tab::Embedding && k == Key::Char('b') && idle(&app) {
                app.status = "building the word-vector table (about 1 s per 14,000 memories on 4 cores) ...".into();
                draw(&mut scr, &mut app);
                scr.flush();
            }
            if !key(&mut app, k) {
                let (bytes, frames) = (scr.bytes_written, scr.frames);
                drop(scr);
                if std::env::var("XKSM_TUI_STATS").is_ok() {
                    eprintln!("xksm-tui: {bytes} bytes written to the terminal in {frames} frames");
                }
                return;
            }
        }
    }
}
