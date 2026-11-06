//! xksm-tui: terminal interface for XKS-Memory: sessions per harness and their turns, recall
//! and word search, memory detail and deletion, the word-vector table and embedding coverage,
//! the port, and per-harness logging state. Store: argv[1], else $XKSM_STORE, else
//! $XDG_DATA_HOME/xksm/agents.xksm, else ~/.local/share/xksm/agents.xksm (as the xksm CLI).
mod ffi;
mod store;

use ratatui::crossterm::event::{self, Event, KeyCode, KeyEventKind, KeyModifiers};
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span, Text};
use ratatui::widgets::{Block, Borders, Clear, List, ListItem, ListState, Paragraph, Row, Table, Tabs, Wrap};
use ratatui::Frame;
use std::time::Duration;
use store::{uri_parts, Hit, Memory, Session, Store};

// ---- local time with UTC offset, through the C library (no time crate) -----------------
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

fn default_store() -> String {
    if let Ok(p) = std::env::var("XKSM_STORE") {
        if !p.is_empty() {
            return p;
        }
    }
    let base = std::env::var("XDG_DATA_HOME")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| format!("{}/.local/share", std::env::var("HOME").unwrap_or_default()));
    format!("{base}/xksm/agents.xksm")
}

// ---- application state ---------------------------------------------------------------------
#[derive(PartialEq, Clone, Copy)]
enum Tab {
    Sessions,
    Search,
    Embedding,
    Store,
}
const TABS: [Tab; 4] = [Tab::Sessions, Tab::Search, Tab::Embedding, Tab::Store];

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

enum Input {
    None,
    Query,
    Port,
}

enum Confirm {
    None,
    ForgetSession(String, String),
    DeleteMemory(i64),
}

struct Turn {
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
    // sessions
    harnesses: Vec<String>,
    harness_ix: usize, // 0 = all
    sessions: Vec<Session>,
    sess_state: ListState,
    turns: Vec<Turn>,
    turn_state: ListState,
    turns_focus: bool,
    // search
    query: String,
    mode: SearchMode,
    scope: Scope,
    results: Vec<Result_>,
    res_state: ListState,
    // detail
    detail: Option<Memory>,
    detail_scroll: u16,
    // input, confirm
    input: Input,
    buf: String,
    confirm: Confirm,
    harness_state: ListState,
}

impl App {
    fn new(st: Store) -> App {
        let mut a = App {
            st,
            tab: Tab::Sessions,
            status: "q quit  Tab/1-4 views  arrows or j k move  Enter open  ? help".into(),
            harnesses: vec![],
            harness_ix: 0,
            sessions: vec![],
            sess_state: ListState::default(),
            turns: vec![],
            turn_state: ListState::default(),
            turns_focus: false,
            query: String::new(),
            mode: SearchMode::Recall,
            scope: Scope::Dialogue,
            results: vec![],
            res_state: ListState::default(),
            detail: None,
            detail_scroll: 0,
            input: Input::None,
            buf: String::new(),
            confirm: Confirm::None,
            harness_state: ListState::default(),
        };
        a.reload_sessions();
        a
    }

    fn current_harness(&self) -> Option<&str> {
        if self.harness_ix == 0 { None } else { self.harnesses.get(self.harness_ix - 1).map(|s| s.as_str()) }
    }

    fn reload_sessions(&mut self) {
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
                if self.sessions.is_empty() {
                    self.sess_state.select(None);
                } else if self.sess_state.selected().map_or(true, |i| i >= self.sessions.len()) {
                    self.sess_state.select(Some(0));
                }
                if self.harness_state.selected().is_none() && !self.harnesses.is_empty() {
                    self.harness_state.select(Some(0));
                }
            }
            Err(e) => self.status = format!("sessions: {e}"),
        }
        self.turns.clear();
        self.turns_focus = false;
    }

    fn load_turns(&mut self) {
        let Some(i) = self.sess_state.selected() else { return };
        let s = self.sessions[i].clone();
        match self.st.session_memories(&s.harness, &s.id) {
            Ok(ids) => {
                self.turns = ids
                    .iter()
                    .filter_map(|&id| self.st.memory(id))
                    .map(|m| {
                        let (_, _, role) = uri_parts(&m.uri);
                        Turn { id: m.id, when: fmt_time(m.when_us), role, line: one_line(body(&m.text), 200) }
                    })
                    .collect();
                self.turn_state.select(if self.turns.is_empty() { None } else { Some(0) });
                self.turns_focus = !self.turns.is_empty();
                self.status = format!("session {}: {} memories", s.id, self.turns.len());
            }
            Err(e) => self.status = format!("turns: {e}"),
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
            v.push(None); /* no harness sessions yet: search the whole store */
        }
        v
    }

    fn run_search(&mut self) {
        if self.query.trim().is_empty() {
            return;
        }
        let t0 = std::time::Instant::now();
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
            match self.scope {
                Scope::Dialogue => "dialogue",
                Scope::WithTrace => "dialogue + trace",
                Scope::Everything => "every harness",
            }
        );
    }

    fn open_detail(&mut self, id: i64) {
        self.detail = self.st.memory(id);
        self.detail_scroll = 0;
    }
}

fn move_sel(state: &mut ListState, len: usize, delta: i64) {
    if len == 0 {
        state.select(None);
        return;
    }
    let cur = state.selected().unwrap_or(0) as i64;
    let next = (cur + delta).clamp(0, len as i64 - 1);
    state.select(Some(next as usize));
}

// ---- drawing ----------------------------------------------------------------------------------
fn title(s: &str) -> Block<'_> {
    Block::default().borders(Borders::ALL).title(Span::styled(format!(" {s} "), Style::new().add_modifier(Modifier::BOLD)))
}

fn hl() -> Style {
    Style::new().bg(Color::DarkGray).add_modifier(Modifier::BOLD)
}

fn draw(f: &mut Frame, a: &mut App) {
    let [top, mid, bottom] = Layout::vertical([Constraint::Length(3), Constraint::Min(5), Constraint::Length(2)]).areas(f.area());
    let names = ["1 Sessions", "2 Search", "3 Embedding", "4 Store"];
    let sel = TABS.iter().position(|t| *t == a.tab).unwrap_or(0);
    let harness = a.current_harness().unwrap_or("all harnesses").to_string();
    f.render_widget(
        Tabs::new(names.to_vec())
            .select(sel)
            .highlight_style(Style::new().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            .block(title(&format!("XKS-Memory  [{harness}]  {}", a.st.path))),
        top,
    );
    match a.tab {
        Tab::Sessions => draw_sessions(f, a, mid),
        Tab::Search => draw_search(f, a, mid),
        Tab::Embedding => draw_embedding(f, a, mid),
        Tab::Store => draw_store(f, a, mid),
    }
    let keys = match (&a.input, &a.confirm, a.detail.is_some()) {
        (Input::Query, _, _) | (Input::Port, _, _) => "type, Enter accept, Esc cancel",
        (_, Confirm::ForgetSession(..), _) | (_, Confirm::DeleteMemory(_), _) => "y confirm, any other key cancels",
        (_, _, true) => "j k / PgUp PgDn scroll  d delete this memory  Esc close",
        _ => match a.tab {
            Tab::Sessions => "h harness  Enter turns / open  Left back  x forget session  r reload  q quit",
            Tab::Search => "/ query  m recall|words  s scope  h harness  Enter open  d delete  q quit",
            Tab::Embedding => "b rebuild word-vector table  u embed missing  p set port  r reload  q quit",
            Tab::Store => "Up Down harness  P pause or resume logging  r reload  q quit",
        },
    };
    f.render_widget(
        Paragraph::new(vec![
            Line::from(Span::styled(a.status.clone(), Style::new().fg(Color::Yellow))),
            Line::from(Span::styled(keys, Style::new().fg(Color::DarkGray))),
        ]),
        bottom,
    );
    if let Some(m) = &a.detail {
        draw_detail(f, m, a.detail_scroll);
    }
    match &a.input {
        Input::Query => draw_input(f, "search query", &a.buf),
        Input::Port => draw_input(f, "port file path (tools/port_build output)", &a.buf),
        Input::None => {}
    }
    match &a.confirm {
        Confirm::ForgetSession(h, s) => draw_confirm(f, &format!("Forget {h} session {s}? Every memory of it is deleted (secure delete). y / n")),
        Confirm::DeleteMemory(id) => draw_confirm(f, &format!("Delete memory {id}? y / n")),
        Confirm::None => {}
    }
}

fn draw_sessions(f: &mut Frame, a: &mut App, area: Rect) {
    let [left, right] = Layout::horizontal([Constraint::Percentage(42), Constraint::Percentage(58)]).areas(area);
    let items: Vec<ListItem> = a
        .sessions
        .iter()
        .map(|s| {
            ListItem::new(vec![
                Line::from(vec![
                    Span::styled(format!("{:<12}", s.harness), Style::new().fg(Color::Cyan)),
                    Span::raw(format!(" {}", one_line(&s.id, 40))),
                ]),
                Line::from(Span::styled(
                    format!("  {} turns  {} .. {}  {}", s.turns, fmt_time(s.first_us), &fmt_time(s.last_us)[11..19], one_line(&s.cwd, 40)),
                    Style::new().fg(Color::Gray),
                )),
            ])
        })
        .collect();
    let lt = format!("Sessions ({})", a.sessions.len());
    f.render_stateful_widget(
        List::new(items).block(title(&lt)).highlight_style(if a.turns_focus { Style::new() } else { hl() }).highlight_symbol("> "),
        left,
        &mut a.sess_state,
    );
    let rows: Vec<ListItem> = a
        .turns
        .iter()
        .map(|t| {
            let color = match t.role.as_str() {
                "user" => Color::Green,
                "assistant" => Color::Cyan,
                "thinking" => Color::Magenta,
                _ => Color::Gray,
            };
            ListItem::new(Line::from(vec![
                Span::styled(format!("{} ", &t.when[11..23]), Style::new().fg(Color::DarkGray)),
                Span::styled(format!("{:<11} ", t.role), Style::new().fg(color)),
                Span::raw(t.line.clone()),
            ]))
        })
        .collect();
    let rt = if a.turns.is_empty() { "Turns (Enter on a session)".to_string() } else { format!("Turns ({})", a.turns.len()) };
    f.render_stateful_widget(
        List::new(rows).block(title(&rt)).highlight_style(if a.turns_focus { hl() } else { Style::new() }).highlight_symbol("> "),
        right,
        &mut a.turn_state,
    );
}

fn draw_search(f: &mut Frame, a: &mut App, area: Rect) {
    let [q, res] = Layout::vertical([Constraint::Length(3), Constraint::Min(3)]).areas(area);
    let mode = if a.mode == SearchMode::Recall { "recall (words + meaning, port expansion)" } else { "words (BM25)" };
    let scope = match a.scope {
        Scope::Dialogue => "dialogue",
        Scope::WithTrace => "dialogue + tool calls, results, thinking",
        Scope::Everything => "every harness and document",
    };
    f.render_widget(
        Paragraph::new(Line::from(vec![Span::styled("> ", Style::new().fg(Color::Cyan)), Span::raw(a.query.clone())]))
            .block(title(&format!("Query  [{mode}]  [{scope}]"))),
        q,
    );
    let items: Vec<ListItem> = a
        .results
        .iter()
        .map(|r| {
            let score = if a.mode == SearchMode::Recall {
                format!("rrf {:.4}  lex #{}  vec #{} cos {:.3}", r.hit.score, rank(r.hit.lexical_rank), rank(r.hit.vector_rank), r.hit.vector_score)
            } else {
                format!("bm25 {:.2}", r.hit.score)
            };
            ListItem::new(vec![
                Line::from(vec![
                    Span::styled(format!("{} ", r.when), Style::new().fg(Color::DarkGray)),
                    Span::styled(format!("{} ", r.harness), Style::new().fg(Color::Cyan)),
                    Span::styled(format!("{} ", r.role), Style::new().fg(Color::Green)),
                    Span::styled(score, Style::new().fg(Color::Gray)),
                ]),
                Line::from(format!("  {}", r.line)),
            ])
        })
        .collect();
    f.render_stateful_widget(
        List::new(items).block(title(&format!("Results ({})", a.results.len()))).highlight_style(hl()).highlight_symbol("> "),
        res,
        &mut a.res_state,
    );
}

fn rank(r: usize) -> String {
    if r == 0 { "-".into() } else { r.to_string() }
}

fn draw_embedding(f: &mut Frame, a: &mut App, area: Rect) {
    let [info, cov] = Layout::vertical([Constraint::Length(9), Constraint::Min(3)]).areas(area);
    let (built, dim) = a.st.table();
    let ci = a.st.cache();
    let port = a.st.port();
    let growth = if built > 0 { (ci.records as f64 / built as f64 - 1.0) * 100.0 } else { 0.0 };
    let lines = vec![
        Line::from(vec![Span::styled("Word-vector table  ", Style::new().add_modifier(Modifier::BOLD)),
            Span::raw(if built > 0 { format!("built from {built} memories, dimension {dim}") } else { "not built yet (needs about 200 memories; b builds it)".into() })]),
        Line::from(format!("  store now holds {} memories ({:+.1} % since the build; the hooks rebuild at +25 %)", ci.records, growth)),
        Line::from("  count-based: PPMI over word co-occurrence, randomized SVD on the CPU (AVX2/FMA); no neural model"),
        Line::from(vec![Span::styled("Port  ", Style::new().add_modifier(Modifier::BOLD)),
            Span::raw(if port.is_empty() { "none recorded (recall works without expansion; p sets one)".into() } else { port })]),
        Line::from("  static word vectors made once offline by an embedding model; used to relate query words to store words"),
        Line::from(vec![Span::styled("Recall  ", Style::new().add_modifier(Modifier::BOLD)),
            Span::raw("hybrid: BM25 on the typed query + RRF with the lattice vector scan over the port-expanded query")]),
    ];
    f.render_widget(Paragraph::new(lines).block(title("Embedding")).wrap(Wrap { trim: false }), info);
    let rows: Vec<Row> = match a.st.ns_counts() {
        Ok(v) => v
            .iter()
            .map(|c| {
                let pct = if c.memories > 0 { c.embedded as f64 * 100.0 / c.memories as f64 } else { 0.0 };
                Row::new(vec![c.ns.clone(), c.memories.to_string(), c.embedded.to_string(), format!("{pct:.2} %")])
            })
            .collect(),
        Err(e) => vec![Row::new(vec![format!("error: {e}")])],
    };
    f.render_widget(
        Table::new(rows, [Constraint::Percentage(40), Constraint::Percentage(20), Constraint::Percentage(20), Constraint::Percentage(20)])
            .header(Row::new(vec!["namespace", "memories", "embedded", "coverage"]).style(Style::new().add_modifier(Modifier::BOLD)))
            .block(title("Embedding coverage (memories without a vector have no word known to the table)")),
        cov,
    );
}

fn draw_store(f: &mut Frame, a: &mut App, area: Rect) {
    let [info, hs] = Layout::vertical([Constraint::Length(8), Constraint::Min(3)]).areas(area);
    let ci = a.st.cache();
    let meta = std::fs::metadata(&a.st.path).ok();
    let size = meta.as_ref().map(|m| m.len()).unwrap_or(0);
    let mode = meta.as_ref().map(|m| { use std::os::unix::fs::PermissionsExt; m.permissions().mode() & 0o777 }).unwrap_or(0);
    let private = mode & 0o077 == 0;
    let lines = vec![
        Line::from(format!("store {}", a.st.path)),
        Line::from(format!("library {}, schema {}, file {:.1} MB", Store::version(), a.st.schema(), size as f64 / 1e6)),
        Line::from(format!("{} memories, {} sessions in view", ci.records, a.sessions.len())),
        Line::from(format!(
            "RAM cache: {} % of records, capacity {}, resident {}, {:.1} MB, hits {}, misses {}",
            ci.percent, ci.capacity, ci.resident, ci.bytes as f64 / 1e6, ci.hits, ci.misses
        )),
        Line::from(Span::styled(
            format!("everything stays on this machine; store file mode {mode:o}: {}", if private { "private to this user" } else { "READABLE BY OTHER USERS (chmod 600 it)" }),
            Style::new().fg(if private { Color::Green } else { Color::Red }),
        )),
    ];
    f.render_widget(Paragraph::new(lines).block(title("Store")), info);
    let items: Vec<ListItem> = a
        .harnesses
        .iter()
        .map(|h| {
            let p = a.st.paused(h);
            ListItem::new(Line::from(vec![
                Span::styled(format!("{h:<20}"), Style::new().fg(Color::Cyan)),
                Span::styled(if p { "logging PAUSED" } else { "logging active" }, Style::new().fg(if p { Color::Red } else { Color::Green })),
            ]))
        })
        .collect();
    f.render_stateful_widget(
        List::new(items).block(title("Harnesses")).highlight_style(hl()).highlight_symbol("> "),
        hs,
        &mut a.harness_state,
    );
}

fn centered(area: Rect, w: u16, h: u16) -> Rect {
    let w = w.min(area.width.saturating_sub(2));
    let h = h.min(area.height.saturating_sub(2));
    Rect { x: area.x + (area.width - w) / 2, y: area.y + (area.height - h) / 2, width: w, height: h }
}

fn draw_detail(f: &mut Frame, m: &Memory, scroll: u16) {
    let area = centered(f.area(), f.area().width * 9 / 10, f.area().height * 9 / 10);
    f.render_widget(Clear, area);
    let (h, s, r) = uri_parts(&m.uri);
    let head = format!(
        "memory {}  |  {}  |  {}  |  {}  |  session {}",
        m.id,
        fmt_time(m.when_us),
        if h.is_empty() { m.ns.as_str() } else { h.as_str() },
        if r.is_empty() { "-" } else { r.as_str() },
        if s.is_empty() { "-" } else { s.as_str() }
    );
    let text = Text::from(body(&m.text).to_string());
    f.render_widget(Paragraph::new(text).wrap(Wrap { trim: false }).scroll((scroll, 0)).block(title(&head)), area);
}

fn draw_input(f: &mut Frame, label: &str, buf: &str) {
    let area = centered(f.area(), 90, 3);
    f.render_widget(Clear, area);
    f.render_widget(Paragraph::new(format!("{buf}_")).block(title(label)), area);
}

fn draw_confirm(f: &mut Frame, msg: &str) {
    let area = centered(f.area(), 90, 4);
    f.render_widget(Clear, area);
    f.render_widget(Paragraph::new(msg.to_string()).wrap(Wrap { trim: true }).block(title("Confirm").style(Style::new().fg(Color::Red))), area);
}

// ---- input handling ----------------------------------------------------------------------
/// returns false to quit
fn key(a: &mut App, code: KeyCode, mods: KeyModifiers) -> bool {
    if code == KeyCode::Char('c') && mods.contains(KeyModifiers::CONTROL) {
        return false;
    }
    // text input
    if !matches!(a.input, Input::None) {
        match code {
            KeyCode::Esc => a.input = Input::None,
            KeyCode::Enter => {
                let v = std::mem::take(&mut a.buf);
                match std::mem::replace(&mut a.input, Input::None) {
                    Input::Query => {
                        a.query = v;
                        a.run_search();
                    }
                    Input::Port => match a.st.set_port(v.trim()) {
                        Ok(()) => a.status = format!("port recorded: {}", v.trim()),
                        Err(e) => a.status = format!("port: {e}"),
                    },
                    Input::None => {}
                }
            }
            KeyCode::Backspace => {
                a.buf.pop();
            }
            KeyCode::Char(ch) => a.buf.push(ch),
            _ => {}
        }
        return true;
    }
    // confirmations
    if !matches!(a.confirm, Confirm::None) {
        let c = std::mem::replace(&mut a.confirm, Confirm::None);
        if code == KeyCode::Char('y') {
            match c {
                Confirm::ForgetSession(h, s) => match a.st.forget(&h, &s) {
                    Ok(n) => {
                        a.status = format!("forgot {h} session {s}: {n} memories deleted");
                        a.reload_sessions();
                    }
                    Err(e) => a.status = format!("forget: {e}"),
                },
                Confirm::DeleteMemory(id) => match a.st.delete(id) {
                    Ok(()) => {
                        a.status = format!("memory {id} deleted");
                        a.detail = None;
                        a.results.retain(|r| r.hit.id != id);
                        a.turns.retain(|t| t.id != id);
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
    // detail view
    if a.detail.is_some() {
        match code {
            KeyCode::Esc | KeyCode::Char('q') => a.detail = None,
            KeyCode::Down | KeyCode::Char('j') => a.detail_scroll = a.detail_scroll.saturating_add(1),
            KeyCode::Up | KeyCode::Char('k') => a.detail_scroll = a.detail_scroll.saturating_sub(1),
            KeyCode::PageDown => a.detail_scroll = a.detail_scroll.saturating_add(20),
            KeyCode::PageUp => a.detail_scroll = a.detail_scroll.saturating_sub(20),
            KeyCode::Char('d') => {
                if let Some(m) = &a.detail {
                    a.confirm = Confirm::DeleteMemory(m.id);
                }
            }
            _ => {}
        }
        return true;
    }
    match code {
        KeyCode::Char('q') => return false,
        KeyCode::Tab => {
            let i = TABS.iter().position(|t| *t == a.tab).unwrap_or(0);
            a.tab = TABS[(i + 1) % TABS.len()];
        }
        KeyCode::BackTab => {
            let i = TABS.iter().position(|t| *t == a.tab).unwrap_or(0);
            a.tab = TABS[(i + TABS.len() - 1) % TABS.len()];
        }
        KeyCode::Char(ch @ '1'..='4') => a.tab = TABS[(ch as u8 - b'1') as usize],
        KeyCode::Char('r') => {
            a.reload_sessions();
            a.status = "reloaded".into();
        }
        KeyCode::Char('h') if a.tab != Tab::Store => {
            a.harness_ix = (a.harness_ix + 1) % (a.harnesses.len() + 1);
            a.reload_sessions();
            a.status = format!("harness: {}", a.current_harness().unwrap_or("all"));
        }
        _ => match a.tab {
            Tab::Sessions => sessions_key(a, code),
            Tab::Search => search_key(a, code),
            Tab::Embedding => embedding_key(a, code),
            Tab::Store => store_key(a, code),
        },
    }
    true
}

fn sessions_key(a: &mut App, code: KeyCode) {
    match code {
        KeyCode::Down | KeyCode::Char('j') => {
            if a.turns_focus {
                move_sel(&mut a.turn_state, a.turns.len(), 1)
            } else {
                move_sel(&mut a.sess_state, a.sessions.len(), 1);
                a.turns.clear();
            }
        }
        KeyCode::Up | KeyCode::Char('k') => {
            if a.turns_focus {
                move_sel(&mut a.turn_state, a.turns.len(), -1)
            } else {
                move_sel(&mut a.sess_state, a.sessions.len(), -1);
                a.turns.clear();
            }
        }
        KeyCode::PageDown => move_sel(if a.turns_focus { &mut a.turn_state } else { &mut a.sess_state }, if a.turns_focus { a.turns.len() } else { a.sessions.len() }, 20),
        KeyCode::PageUp => move_sel(if a.turns_focus { &mut a.turn_state } else { &mut a.sess_state }, if a.turns_focus { a.turns.len() } else { a.sessions.len() }, -20),
        KeyCode::Enter | KeyCode::Right => {
            if a.turns_focus {
                if let Some(i) = a.turn_state.selected() {
                    let id = a.turns[i].id;
                    a.open_detail(id);
                }
            } else {
                a.load_turns();
            }
        }
        KeyCode::Left | KeyCode::Esc => a.turns_focus = false,
        KeyCode::Char('x') => {
            if let Some(i) = a.sess_state.selected() {
                let s = &a.sessions[i];
                a.confirm = Confirm::ForgetSession(s.harness.clone(), s.id.clone());
            }
        }
        _ => {}
    }
}

fn search_key(a: &mut App, code: KeyCode) {
    match code {
        KeyCode::Char('/') | KeyCode::Char('i') => {
            a.buf = a.query.clone();
            a.input = Input::Query;
        }
        KeyCode::Char('m') => {
            a.mode = if a.mode == SearchMode::Recall { SearchMode::Words } else { SearchMode::Recall };
            a.run_search();
        }
        KeyCode::Char('s') => {
            a.scope = match a.scope {
                Scope::Dialogue => Scope::WithTrace,
                Scope::WithTrace => Scope::Everything,
                Scope::Everything => Scope::Dialogue,
            };
            a.run_search();
        }
        KeyCode::Down | KeyCode::Char('j') => move_sel(&mut a.res_state, a.results.len(), 1),
        KeyCode::Up | KeyCode::Char('k') => move_sel(&mut a.res_state, a.results.len(), -1),
        KeyCode::PageDown => move_sel(&mut a.res_state, a.results.len(), 10),
        KeyCode::PageUp => move_sel(&mut a.res_state, a.results.len(), -10),
        KeyCode::Enter => {
            if let Some(i) = a.res_state.selected() {
                let id = a.results[i].hit.id;
                a.open_detail(id);
            }
        }
        KeyCode::Char('d') => {
            if let Some(i) = a.res_state.selected() {
                a.confirm = Confirm::DeleteMemory(a.results[i].hit.id);
            }
        }
        _ => {}
    }
}

fn embedding_key(a: &mut App, code: KeyCode) {
    match code {
        KeyCode::Char('b') => {
            a.status = "building the word-vector table (about 1 s per 3,000 memories; the screen waits)".into();
            // drawn by the caller before the work starts: see main loop (pending action)
            a.status = match a.st.build() {
                Ok(r) => format!(
                    "built from {} memories ({} embedded), vocabulary {}, {} non-zeros; SVD {:.3} s, total {:.3} s",
                    r.memories, r.embedded, r.vocab, r.nnz, r.t_svd, r.t_total
                ),
                Err(e) => format!("build: {e}"),
            };
        }
        KeyCode::Char('u') => {
            a.status = match a.st.update() {
                Ok(n) => format!("embedded {n} memories that had no vector"),
                Err(e) => format!("embed: {e}"),
            };
        }
        KeyCode::Char('p') => {
            a.buf = a.st.port();
            a.input = Input::Port;
        }
        _ => {}
    }
}

fn store_key(a: &mut App, code: KeyCode) {
    match code {
        KeyCode::Down | KeyCode::Char('j') => move_sel(&mut a.harness_state, a.harnesses.len(), 1),
        KeyCode::Up | KeyCode::Char('k') => move_sel(&mut a.harness_state, a.harnesses.len(), -1),
        KeyCode::Char('P') | KeyCode::Char('p') => {
            if let Some(i) = a.harness_state.selected() {
                let h = a.harnesses[i].clone();
                let on = !a.st.paused(&h);
                a.status = match a.st.set_paused(&h, on) {
                    Ok(()) => format!("{h}: logging {}", if on { "paused" } else { "active" }),
                    Err(e) => format!("pause: {e}"),
                };
            }
        }
        _ => {}
    }
}

fn main() {
    let path = std::env::args().nth(1).unwrap_or_else(default_store);
    if path == "--help" || path == "-h" {
        println!("usage: xksm-tui [STORE]   (default: $XKSM_STORE or ~/.local/share/xksm/agents.xksm)");
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
    let mut terminal = ratatui::init();
    let result = (|| -> std::io::Result<()> {
        loop {
            terminal.draw(|f| draw(f, &mut app))?;
            if event::poll(Duration::from_millis(250))? {
                if let Event::Key(k) = event::read()? {
                    if k.kind != KeyEventKind::Press {
                        continue;
                    }
                    // long actions: show the "working" line first
                    if app.tab == Tab::Embedding && k.code == KeyCode::Char('b') && matches!(app.input, Input::None) && matches!(app.confirm, Confirm::None) && app.detail.is_none() {
                        app.status = "building the word-vector table (about 1 s per 3,000 memories) ...".into();
                        terminal.draw(|f| draw(f, &mut app))?;
                    }
                    if !key(&mut app, k.code, k.modifiers) {
                        return Ok(());
                    }
                }
            }
        }
    })();
    ratatui::restore();
    if let Err(e) = result {
        eprintln!("xksm-tui: {e}");
        std::process::exit(1);
    }
}
