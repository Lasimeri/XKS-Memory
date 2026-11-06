//! xksm-tui: terminal management interface for XKS-Memory (std only: term.rs is the whole
//! terminal layer, no terminal crates).
//! Views: 1 Sessions (per harness, their turns), 2 Search (recall or words, three scopes),
//! 3 Memories (every namespace, newest first), 4 Embedding (word-vector table, port,
//! coverage), 5 Store (file, cache, logging per harness). Actions: open, delete, forget a
//! session, export a session to Markdown, add a note, import a file, rebuild the table, embed
//! missing memories, set the port, pause or resume a harness. The store refreshes itself every
//! 5 s (agents write to it while it is open).
//! Store: argv[1], else $XKSM_STORE, else $XDG_DATA_HOME/xksm/agents.xksm, else
//! ~/.local/share/xksm/agents.xksm (as the xksm CLI).
mod ffi;
mod store;
mod term;

use std::time::{Duration, Instant};
use term::{list, paragraph, str_width, Item, Key, Rect, Screen, Sel, Style, CYAN, DARK, GRAY, GREEN, MAGENTA, RED, YELLOW};
use store::{uri_parts, Hit, Memory, NsCount, Session, Store};

const PAGE: usize = 200;
const REFRESH: Duration = Duration::from_secs(5);

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

fn default_store() -> String {
    if let Ok(p) = std::env::var("XKSM_STORE") {
        if !p.is_empty() {
            return p;
        }
    }
    let base = std::env::var("XDG_DATA_HOME").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| format!("{}/.local/share", home()));
    format!("{base}/xksm/agents.xksm")
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
}

struct Result_ {
    hit: Hit,
    when: String,
    harness: String,
    role: String,
    line: String,
}

struct App {
    st: Store,
    tab: Tab,
    status: String,
    help: bool,
    auto: bool,
    last_refresh: Instant,
    // sessions
    harnesses: Vec<String>,
    harness_ix: usize, // 0 = all
    sessions: Vec<Session>,
    sess_state: Sel,
    turns: Vec<Row_>,
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
    mem_state: Sel,
    mems_focus: bool,
    mems_done: bool, // every memory of the namespace is loaded
    // detail, input, confirm
    detail: Option<Memory>,
    detail_scroll: u16,
    input: Input,
    buf: String,
    confirm: Confirm,
    harness_state: Sel,
}

fn row_of(m: &Memory) -> Row_ {
    let (_, _, role) = uri_parts(&m.uri);
    Row_ { id: m.id, when: fmt_time(m.when_us), role: if role.is_empty() { "-".into() } else { role }, line: one_line(body(&m.text), 200) }
}

impl App {
    fn new(st: Store) -> App {
        let mut a = App {
            st,
            tab: Tab::Sessions,
            status: "? help  q quit  Tab or 1-5 views  Enter open  a auto-refresh".into(),
            help: false,
            auto: true,
            last_refresh: Instant::now(),
            harnesses: vec![],
            harness_ix: 0,
            sessions: vec![],
            sess_state: Sel::default(),
            turns: vec![],
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
            mem_state: Sel::default(),
            mems_focus: false,
            mems_done: false,
            detail: None,
            detail_scroll: 0,
            input: Input::None,
            buf: String::new(),
            confirm: Confirm::None,
            harness_state: Sel::default(),
        };
        a.reload_sessions();
        a.reload_ns();
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

    fn refresh(&mut self) {
        self.reload_sessions();
        self.reload_ns();
        self.last_refresh = Instant::now();
    }

    fn load_turns(&mut self) {
        let Some(i) = self.sess_state.selected() else { return };
        let s = self.sessions[i].clone();
        match self.st.session_memories(&s.harness, &s.id) {
            Ok(ids) => {
                self.turns = ids.iter().filter_map(|&id| self.st.memory(id)).map(|m| row_of(&m)).collect();
                self.turn_state.select(if self.turns.is_empty() { None } else { Some(0) });
                self.turns_focus = !self.turns.is_empty();
                self.status = format!("{} session {}: {} memories", s.harness, s.id, self.turns.len());
            }
            Err(e) => self.status = format!("turns: {e}"),
        }
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
                self.status = format!("{ns}: {} memories shown{}", self.mems.len(), if self.mems_done { "" } else { " (more below)" });
            }
            Err(e) => self.status = format!("memories: {e}"),
        }
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
        self.results = hits
            .into_iter()
            .filter_map(|h| {
                let m = self.st.memory(h.id)?;
                let (har, _, role) = uri_parts(&m.uri);
                Some(Result_ {
                    when: fmt_time(m.when_us),
                    harness: if har.is_empty() { m.ns.clone() } else { har },
                    role: if role.is_empty() { "-".into() } else { role },
                    line: one_line(body(&m.text), 200),
                    hit: h,
                })
            })
            .collect();
        self.res_state.select(if self.results.is_empty() { None } else { Some(0) });
        self.status = format!(
            "{} results in {:.1} ms ({}, {})",
            self.results.len(),
            ms,
            if self.mode == SearchMode::Recall { "recall: words + meaning" } else { "words: BM25" },
            scope_name(self.scope)
        );
    }

    fn open_detail(&mut self, id: i64) {
        self.detail = self.st.memory(id);
        self.detail_scroll = 0;
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
    let (top, rest) = scr.area().split_v(3);
    let (mid, bottom) = rest.split_v(rest.h.saturating_sub(2));
    let harness = a.current_harness().unwrap_or("all harnesses").to_string();
    let live = a.st.count(None);
    scr.boxed(
        top,
        &format!("XKS-Memory  [{harness}]  {live} memories  {}  {}", if a.auto { "auto-refresh 5 s" } else { "auto-refresh off" }, a.st.path),
        BORDER,
    );
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
            Tab::Search => "/ query  m recall|words  s scope  h harness  Enter open  d delete  n note  ? help",
            Tab::Memories => "Enter memories / open  Left back  d delete  i import file  n note  ? help",
            Tab::Embedding => "b rebuild table  u embed missing  p set port  ? help",
            Tab::Store => "Up Down harness  P pause or resume logging  ? help",
        }
    };
    scr.text(bottom.x, bottom.y, &a.status, bottom.w, Style::fg(YELLOW));
    scr.text(bottom.x, bottom.y + 1, keys, bottom.w, Style::fg(DARK));
    if let Some(m) = &a.detail {
        draw_detail(scr, m, a.detail_scroll);
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
                    seg(format!("{} ", r.when), Style::fg(DARK)),
                    seg(format!("{} ", r.harness), Style::fg(CYAN)),
                    seg(format!("{} ", r.role), Style::fg(role_color(&r.role))),
                    seg(score, Style::fg(GRAY)),
                ],
                vec![seg(r.line.clone(), Style::default())],
            ]
        })
        .collect();
    scr.boxed(res, &format!("Results ({})", a.results.len()), BORDER);
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
    let (built, dim) = a.st.table();
    let live = a.st.count(None);
    let port = a.st.port();
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
    let meta = std::fs::metadata(&a.st.path).ok();
    let size = meta.as_ref().map(|m| m.len()).unwrap_or(0);
    let mode = meta
        .as_ref()
        .map(|m| {
            use std::os::unix::fs::PermissionsExt;
            m.permissions().mode() & 0o777
        })
        .unwrap_or(0);
    let private = mode & 0o077 == 0;
    scr.boxed(info, "Store", BORDER);
    let i = info.inner();
    let lines = [
        format!("store {}", a.st.path),
        format!("library {}, schema {}, file {:.1} MB", Store::version(), a.st.schema(), size as f64 / 1e6),
        format!("{} memories now, {} sessions in view", a.st.count(None), a.sessions.len()),
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
    let items: Vec<Item> = a
        .harnesses
        .iter()
        .map(|h| {
            let p = a.st.paused(h);
            vec![vec![
                seg(format!("{h:<20}"), Style::fg(CYAN)),
                seg(format!("{:>9} dialogue  {:>9} trace  ", a.st.count(Some(h)), a.st.count(Some(&format!("{h}-trace")))), Style::fg(GRAY)),
                seg(if p { "logging PAUSED" } else { "logging active" }, Style::fg(if p { RED } else { GREEN })),
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

fn draw_detail(scr: &mut Screen, m: &Memory, scroll: u16) {
    let (h, s, r) = uri_parts(&m.uri);
    let head = format!(
        "memory {}  |  {}  |  {}  |  {}  |  session {}",
        m.id,
        fmt_time(m.when_us),
        if h.is_empty() { m.ns.as_str() } else { h.as_str() },
        if r.is_empty() { "-" } else { r.as_str() },
        if s.is_empty() { "-" } else { s.as_str() }
    );
    let (w, hh) = (scr.w * 9 / 10, scr.h * 9 / 10);
    let i = popup(scr, w, hh, &head, BORDER);
    paragraph(scr, i, body(&m.text), scroll as usize, Style::default());
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
    ("Everywhere", "n add a note   a auto-refresh on/off   r reload now   h cycle harness   q quit   ? this help"),
    ("Sessions", "Enter: turns of the session, Enter again: full text   x forget the session   e export it to ~/xksm-export-*.md"),
    ("Search", "/ edit the query   m recall (words + meaning) or words (BM25)   s scope: dialogue, + trace, every harness"),
    ("Memories", "Enter: memories of the namespace, newest first (more load at the end)   i import a file (selected namespace, or documents for harness namespaces)"),
    ("Detail", "j k PgUp PgDn scroll   d delete the memory (asks first)   Esc close"),
    ("Embedding", "b rebuild the word-vector table   u embed memories that have no vector   p record a port file"),
    ("Store", "P pause or resume logging for the selected harness"),
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
                        a.turns.clear();
                        a.turns_focus = false;
                        a.refresh();
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
            a.refresh();
            a.status = "reloaded".into();
        }
        Key::Char('a') => {
            a.auto = !a.auto;
            a.status = format!("auto-refresh {}", if a.auto { "on (every 5 s)" } else { "off" });
        }
        Key::Char('n') => {
            a.buf.clear();
            a.input = Input::Note;
        }
        Key::Char('h') if a.tab != Tab::Store => {
            a.harness_ix = (a.harness_ix + 1) % (a.harnesses.len() + 1);
            a.reload_sessions();
            a.turns.clear();
            a.turns_focus = false;
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
            match a.st.add_note("notes", t) {
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
            a.turns.clear();
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
                a.load_turns();
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
            a.mems.clear();
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
                    "built from {} memories ({} embedded), vocabulary {}, {} non-zeros; SVD {:.3} s, total {:.3} s",
                    r.memories, r.embedded, r.vocab, r.nnz, r.t_svd, r.t_total
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
    if let Key::Char('P') | Key::Char('p') = k {
        if let Some(i) = a.harness_state.selected() {
            let h = a.harnesses[i].clone();
            let on = !a.st.paused(&h);
            a.status = match a.st.set_paused(&h, on) {
                Ok(()) => format!("{h}: logging {}", if on { "paused" } else { "active" }),
                Err(e) => format!("pause: {e}"),
            };
        }
    }
}

fn idle(a: &App) -> bool {
    a.input == Input::None && matches!(a.confirm, Confirm::None) && a.detail.is_none() && !a.help
}

fn main() {
    let path = std::env::args().nth(1).unwrap_or_else(default_store);
    if path == "--help" || path == "-h" {
        println!("usage: xksm-tui [STORE]   (default: $XKSM_STORE or ~/.local/share/xksm/agents.xksm); press ? inside for keys");
        return;
    }
    let st = match Store::open(&path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("xksm-tui: {e}");
            std::process::exit(1);
        }
    };
    let mut app = App::new(st);
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
        if app.auto && idle(&app) && app.last_refresh.elapsed() >= REFRESH {
            app.refresh(); /* agents write to the store while it is open */
            dirty = true;
        }
        if scr.resize() {
            dirty = true;
        }
        if dirty {
            draw(&mut scr, &mut app);
            scr.flush();
            dirty = false;
        }
        let keys = term::read_keys(250);
        for k in keys {
            dirty = true;
            /* long actions: show the "working" line before the work starts */
            if app.tab == Tab::Embedding && k == Key::Char('b') && idle(&app) {
                app.status = "building the word-vector table (about 1 s per 14,000 memories on 4 cores) ...".into();
                draw(&mut scr, &mut app);
                scr.flush();
            }
            if !key(&mut app, k) {
                let bytes = scr.bytes_written;
                drop(scr);
                if std::env::var("XKSM_TUI_STATS").is_ok() {
                    eprintln!("xksm-tui: {bytes} bytes written to the terminal");
                }
                return;
            }
        }
    }
}
