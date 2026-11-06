//! term.rs: the whole terminal layer, std + libc only (no terminal crates).
//! Raw mode through termios, size through TIOCGWINSZ, input through poll + read with an own
//! key decoder, output through a double-buffered cell grid that emits only the cells that
//! changed since the last frame (one write per frame). A headless screen (fixed size, no
//! terminal) renders the same frames for the script runner and the golden tests.
use std::io::Write;
use std::os::raw::{c_char, c_int};

// ---- libc --------------------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
struct Termios {
    c_iflag: u32,
    c_oflag: u32,
    c_cflag: u32,
    c_lflag: u32,
    c_line: u8,
    c_cc: [u8; 32],
    c_ispeed: u32,
    c_ospeed: u32,
}

#[repr(C)]
struct Winsize {
    ws_row: u16,
    ws_col: u16,
    ws_xpixel: u16,
    ws_ypixel: u16,
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

extern "C" {
    fn tcgetattr(fd: c_int, t: *mut Termios) -> c_int;
    fn tcsetattr(fd: c_int, act: c_int, t: *const Termios) -> c_int;
    fn cfmakeraw(t: *mut Termios);
    fn ioctl(fd: c_int, req: u64, ...) -> c_int;
    fn poll(fds: *mut PollFd, n: u64, timeout: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut std::ffi::c_void, n: usize) -> isize;
    fn setlocale(cat: c_int, loc: *const c_char) -> *mut c_char;
    fn wcwidth(c: i32) -> c_int;
}

const TCSANOW: c_int = 0;
const TIOCGWINSZ: u64 = 0x5413;
const POLLIN: i16 = 1;
const LC_ALL: c_int = 6;

/// East Asian Wide / Fullwidth blocks (and emoji): two cells. Used when the C library cannot
/// tell (a locale that is not UTF-8 makes wcwidth return -1 for every non-ASCII character).
const WIDE: [(u32, u32); 12] = [
    (0x1100, 0x115f),
    (0x2e80, 0x303e),
    (0x3041, 0x33ff),
    (0x3400, 0x4dbf),
    (0x4e00, 0x9fff),
    (0xa000, 0xa4cf),
    (0xac00, 0xd7a3),
    (0xf900, 0xfaff),
    (0xfe30, 0xfe4f),
    (0xff00, 0xff60),
    (0x1f300, 0x1faff),
    (0x20000, 0x3fffd),
];

fn fallback_width(c: u32) -> usize {
    if (0x300..=0x36f).contains(&c) || (0x200b..=0x200f).contains(&c) || (0xfe00..=0xfe0f).contains(&c) {
        return 0; /* combining marks, zero-width spaces, variation selectors */
    }
    if (0x80..0xa0).contains(&c) {
        return 0; /* C1 controls */
    }
    if WIDE.iter().any(|&(lo, hi)| c >= lo && c <= hi) || (0xffe0..=0xffe6).contains(&c) { 2 } else { 1 }
}

/// display width of a character in cells (0 for combining / control characters)
pub fn char_width(c: char) -> usize {
    if (c as u32) < 0x7f {
        return if (c as u32) < 0x20 { 0 } else { 1 }; /* ASCII fast path */
    }
    let w = unsafe { wcwidth(c as i32) };
    if w < 0 { fallback_width(c as u32) } else { w as usize }
}

pub fn str_width(s: &str) -> usize {
    s.chars().map(char_width).sum()
}

// ---- styles and cells -------------------------------------------------------------------------
/// colours: 0 = terminal default, 1..=15 = the 16 ANSI colours (8 + n = bright)
#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub struct Style {
    pub fg: u8,
    pub bg: u8,
    pub bold: bool,
}

pub const RED: u8 = 2; /* stored as ANSI index + 1, so 0 can mean "default" */
pub const GREEN: u8 = 3;
pub const YELLOW: u8 = 4;
pub const MAGENTA: u8 = 6;
pub const CYAN: u8 = 7;
pub const GRAY: u8 = 8; /* white (37), shown as light gray by most themes */
pub const DARK: u8 = 9; /* bright black (90) */
pub const SELBG: u8 = 9; /* bright black background for the selected line */

impl Style {
    pub const fn fg(c: u8) -> Style {
        Style { fg: c, bg: 0, bold: false }
    }
    pub const fn bold(self) -> Style {
        Style { fg: self.fg, bg: self.bg, bold: true }
    }
    pub const fn on(self, bg: u8) -> Style {
        Style { fg: self.fg, bg, bold: self.bold }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct Cell {
    ch: char, /* '\0' = the right half of a wide character */
    st: Style,
}

const BLANK: Cell = Cell { ch: ' ', st: Style { fg: 0, bg: 0, bold: false } };

#[derive(Clone, Copy)]
pub struct Rect {
    pub x: u16,
    pub y: u16,
    pub w: u16,
    pub h: u16,
}

impl Rect {
    pub fn inner(self) -> Rect {
        Rect { x: self.x + 1, y: self.y + 1, w: self.w.saturating_sub(2), h: self.h.saturating_sub(2) }
    }
    /// split into a top part of `h` rows and the rest
    pub fn split_v(self, h: u16) -> (Rect, Rect) {
        let h = h.min(self.h);
        (Rect { h, ..self }, Rect { y: self.y + h, h: self.h - h, ..self })
    }
    /// split into a left part of `pct` percent and the rest
    pub fn split_h(self, pct: u16) -> (Rect, Rect) {
        let w = (self.w as u32 * pct as u32 / 100) as u16;
        (Rect { w, ..self }, Rect { x: self.x + w, w: self.w - w, ..self })
    }
    pub fn centered(self, w: u16, h: u16) -> Rect {
        let w = w.min(self.w.saturating_sub(2));
        let h = h.min(self.h.saturating_sub(2));
        Rect { x: self.x + (self.w - w) / 2, y: self.y + (self.h - h) / 2, w, h }
    }
}

// ---- the screen ---------------------------------------------------------------------------------
pub struct Screen {
    pub w: u16,
    pub h: u16,
    back: Vec<Cell>,
    front: Vec<Cell>, /* what the terminal shows; empty = repaint everything */
    out: Vec<u8>,
    saved: Option<Termios>, /* None: headless (no terminal behind it) */
    no_color: bool,         /* NO_COLOR set: bold and reverse video only */
    pub bytes_written: u64,
    pub frames: u64,
}

/// https://no-color.org: any non-empty NO_COLOR turns colour output off
fn no_color_env() -> bool {
    std::env::var_os("NO_COLOR").is_some_and(|v| !v.is_empty())
}

impl Screen {
    pub fn open() -> std::io::Result<Screen> {
        unsafe { setlocale(LC_ALL, b"\0".as_ptr() as *const c_char) }; /* wcwidth needs the locale */
        let mut t: Termios = unsafe { std::mem::zeroed() };
        if unsafe { tcgetattr(0, &mut t) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
        let saved = t;
        unsafe { cfmakeraw(&mut t) };
        t.c_oflag |= 0o4 | 0o1; /* keep ONLCR | OPOST: \n still moves to the line start */
        if unsafe { tcsetattr(0, TCSANOW, &t) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
        SAVED.with(|s| *s.borrow_mut() = Some(saved));
        let mut s = Screen::blank(Some(saved));
        s.raw_write(b"\x1b[?1049h\x1b[?25l\x1b[2J");
        s.resize();
        Ok(s)
    }

    /// a screen of a fixed size that renders into memory only (script runner, tests)
    pub fn headless(w: u16, h: u16) -> Screen {
        unsafe { setlocale(LC_ALL, b"\0".as_ptr() as *const c_char) };
        let mut s = Screen::blank(None);
        s.set_size(w, h);
        s
    }

    fn blank(saved: Option<Termios>) -> Screen {
        Screen { w: 0, h: 0, back: vec![], front: vec![], out: Vec::with_capacity(1 << 16), saved, no_color: no_color_env(), bytes_written: 0, frames: 0 }
    }

    fn raw_write(&mut self, b: &[u8]) {
        self.bytes_written += b.len() as u64;
        if self.saved.is_none() {
            return;
        }
        let mut o = std::io::stdout().lock();
        let _ = o.write_all(b);
        let _ = o.flush();
    }

    fn set_size(&mut self, w: u16, h: u16) -> bool {
        if w == self.w && h == self.h {
            return false;
        }
        self.w = w;
        self.h = h;
        self.back = vec![BLANK; w as usize * h as usize];
        self.front.clear(); /* repaint everything */
        true
    }

    /// pick up a terminal resize; returns true when the size changed
    pub fn resize(&mut self) -> bool {
        if self.saved.is_none() {
            return false;
        }
        let mut ws = Winsize { ws_row: 24, ws_col: 80, ws_xpixel: 0, ws_ypixel: 0 };
        unsafe { ioctl(1, TIOCGWINSZ, &mut ws as *mut Winsize) };
        self.set_size(ws.ws_col.max(20), ws.ws_row.max(6))
    }

    pub fn area(&self) -> Rect {
        Rect { x: 0, y: 0, w: self.w, h: self.h }
    }

    pub fn clear(&mut self) {
        self.back.fill(BLANK);
    }

    pub fn fill(&mut self, r: Rect, st: Style) {
        for y in r.y..(r.y + r.h).min(self.h) {
            for x in r.x..(r.x + r.w).min(self.w) {
                self.back[y as usize * self.w as usize + x as usize] = Cell { ch: ' ', st };
            }
        }
    }

    /// text at (x, y), at most `max` cells wide; returns the cells used
    pub fn text(&mut self, x: u16, y: u16, s: &str, max: u16, st: Style) -> u16 {
        if y >= self.h {
            return 0;
        }
        let limit = (x as u32 + max as u32).min(self.w as u32) as u16;
        let mut cx = x;
        for c in s.chars() {
            let c = if (c as u32) < 0x20 || c == '\u{7f}' { ' ' } else { c };
            let cw = char_width(c) as u16;
            if cw == 0 {
                continue;
            }
            if cx + cw > limit {
                break;
            }
            let i = y as usize * self.w as usize + cx as usize;
            let row_end = (y as usize + 1) * self.w as usize;
            /* never leave half of a wide character behind: writing over its right half blanks
             * the left one, writing over its left half (below) blanks the right one */
            if self.back[i].ch == '\0' && cx > 0 {
                self.back[i - 1].ch = ' ';
            }
            self.back[i] = Cell { ch: c, st };
            if cw == 2 {
                self.back[i + 1] = Cell { ch: '\0', st };
            }
            if i + (cw as usize) < row_end && self.back[i + cw as usize].ch == '\0' {
                self.back[i + cw as usize].ch = ' ';
            }
            cx += cw;
        }
        cx - x
    }

    /// a box with a title in the top border
    pub fn boxed(&mut self, r: Rect, title: &str, st: Style) {
        if r.w < 2 || r.h < 2 {
            return;
        }
        let (x0, y0, x1, y1) = (r.x, r.y, r.x + r.w - 1, r.y + r.h - 1);
        for x in x0 + 1..x1 {
            self.text(x, y0, "─", 1, st);
            self.text(x, y1, "─", 1, st);
        }
        for y in y0 + 1..y1 {
            self.text(x0, y, "│", 1, st);
            self.text(x1, y, "│", 1, st);
        }
        self.text(x0, y0, "┌", 1, st);
        self.text(x1, y0, "┐", 1, st);
        self.text(x0, y1, "└", 1, st);
        self.text(x1, y1, "┘", 1, st);
        if !title.is_empty() && r.w > 4 {
            self.text(x0 + 1, y0, &format!(" {title} "), r.w - 3, st.bold());
        }
    }

    /// the SGR sequence of a style (always from a reset, so it does not depend on the last one)
    fn sgr(&mut self, st: Style) {
        self.out.extend_from_slice(b"\x1b[0");
        if st.bold {
            self.out.extend_from_slice(b";1");
        }
        if self.no_color {
            if st.bg != 0 {
                self.out.extend_from_slice(b";7"); /* the selection stays visible as reverse video */
            }
        } else {
            if st.fg != 0 {
                let n = st.fg - 1;
                let _ = write!(self.out, ";{}", if n < 8 { 30 + n as u32 } else { 90 + n as u32 - 8 });
            }
            if st.bg != 0 {
                let n = st.bg - 1;
                let _ = write!(self.out, ";{}", if n < 8 { 40 + n as u32 } else { 100 + n as u32 - 8 });
            }
        }
        self.out.push(b'm');
    }

    /// emit the difference between the frame drawn and what the terminal shows, in one write
    pub fn flush(&mut self) {
        let full = self.front.len() != self.back.len();
        let w = self.w as usize;
        self.out.clear();
        let mut cur_st: Option<Style> = None;
        let mut at: Option<usize> = None; /* index where the terminal cursor is */
        for i in 0..self.back.len() {
            let c = self.back[i];
            if c.ch == '\0' || (!full && self.front[i] == c) {
                continue;
            }
            if at != Some(i) {
                let _ = write!(self.out, "\x1b[{};{}H", i / w + 1, i % w + 1);
            }
            if cur_st != Some(c.st) {
                self.sgr(c.st);
                cur_st = Some(c.st);
            }
            let mut b = [0u8; 4];
            self.out.extend_from_slice(c.ch.encode_utf8(&mut b).as_bytes());
            let cw = char_width(c.ch).max(1);
            at = Some(i + cw);
            if cw == 2 && i + 1 < self.back.len() && !full {
                self.front[i + 1] = self.back[i + 1];
            }
        }
        if !self.out.is_empty() {
            self.out.extend_from_slice(b"\x1b[0m");
            let o = std::mem::take(&mut self.out);
            self.raw_write(&o);
            self.out = o;
        }
        self.front.clone_from(&self.back);
        self.frames += 1;
    }

    /// the bytes the last flush emitted (empty when nothing changed)
    #[cfg(test)]
    pub fn last_output(&self) -> &[u8] {
        &self.out
    }

    /// the frame as text: one line per row (wide characters once), then the styled runs of
    /// every row that has any, as `row: x+len=fg,bg[,b]` with the colour numbers of Style
    pub fn dump(&self) -> String {
        let w = self.w as usize;
        let mut s = String::new();
        for row in self.back.chunks(w) {
            let line: String = row.iter().filter(|c| c.ch != '\0').map(|c| c.ch).collect();
            s.push_str(line.trim_end());
            s.push('\n');
        }
        s.push_str("--- styles\n");
        for (y, row) in self.back.chunks(w).enumerate() {
            let mut runs = String::new();
            let mut x = 0;
            while x < w {
                let st = row[x].st;
                let mut e = x + 1;
                while e < w && row[e].st == st {
                    e += 1;
                }
                if st != Style::default() {
                    runs.push_str(&format!(" {}+{}={},{}{}", x, e - x, st.fg, st.bg, if st.bold { ",b" } else { "" }));
                }
                x = e;
            }
            if !runs.is_empty() {
                s.push_str(&format!("{y}:{runs}\n"));
            }
        }
        s
    }
}

impl Drop for Screen {
    fn drop(&mut self) {
        if let Some(t) = self.saved {
            self.raw_write(b"\x1b[0m\x1b[?25h\x1b[?1049l");
            unsafe { tcsetattr(0, TCSANOW, &t) };
            SAVED.with(|s| *s.borrow_mut() = None);
        }
    }
}

thread_local! {
    static SAVED: std::cell::RefCell<Option<Termios>> = const { std::cell::RefCell::new(None) };
}

/// restore the terminal if the program panics while in raw mode
pub fn install_panic_restore() {
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        SAVED.with(|s| {
            if let Some(t) = *s.borrow() {
                let _ = std::io::stdout().write_all(b"\x1b[0m\x1b[?25h\x1b[?1049l");
                let _ = std::io::stdout().flush();
                unsafe { tcsetattr(0, TCSANOW, &t) };
            }
        });
        prev(info);
    }));
}

// ---- input ----------------------------------------------------------------------------------------
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Key {
    Char(char),
    Enter,
    Esc,
    Backspace,
    Delete,
    Tab,
    BackTab,
    Up,
    Down,
    Left,
    Right,
    PgUp,
    PgDn,
    Home,
    End,
    CtrlC,
    CtrlU,
    CtrlW,
}

/// keys available within `timeout_ms` (empty on timeout). A read that ends inside an escape
/// sequence (the terminal split it) waits up to 30 ms for the rest before decoding.
pub fn read_keys(timeout_ms: i32) -> Vec<Key> {
    let mut p = PollFd { fd: 0, events: POLLIN, revents: 0 };
    if unsafe { poll(&mut p, 1, timeout_ms) } <= 0 {
        return vec![];
    }
    let mut buf = [0u8; 1024];
    let mut n = 0usize;
    loop {
        let r = unsafe { read(0, buf[n..].as_mut_ptr() as *mut std::ffi::c_void, buf.len() - n) };
        if r <= 0 {
            break;
        }
        n += r as usize;
        if n == buf.len() || !incomplete(&buf[..n]) {
            break;
        }
        let mut p = PollFd { fd: 0, events: POLLIN, revents: 0 };
        if unsafe { poll(&mut p, 1, 30) } <= 0 {
            break;
        }
    }
    decode(&buf[..n])
}

/// true when the bytes end inside an escape sequence: ESC, ESC [ or ESC O, then only
/// parameter bytes (digits, ';')
pub fn incomplete(b: &[u8]) -> bool {
    let Some(e) = b.iter().rposition(|&c| c == 0x1b) else { return false };
    let t = &b[e + 1..];
    match t.first() {
        None => true,
        Some(b'[') | Some(b'O') => t[1..].iter().all(|c| c.is_ascii_digit() || *c == b';'),
        Some(_) => false,
    }
}

/// decode one read's bytes into keys; a lone ESC (not followed by '[' or 'O') is Esc.
/// CSI parameters: only the first one selects the key (`ESC [ 1 ; 5 C` = Ctrl-Right = Right).
pub fn decode(b: &[u8]) -> Vec<Key> {
    let mut keys = vec![];
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        match c {
            0x1b => {
                if i + 1 < b.len() && (b[i + 1] == b'[' || b[i + 1] == b'O') {
                    let mut j = i + 2;
                    let mut first = 0u32;
                    let mut in_first = true;
                    while j < b.len() && (b[j].is_ascii_digit() || b[j] == b';') {
                        if b[j] == b';' {
                            in_first = false;
                        } else if in_first {
                            first = first.saturating_mul(10).saturating_add((b[j] - b'0') as u32);
                        }
                        j += 1;
                    }
                    if j < b.len() {
                        let k = match (b[j], first) {
                            (b'A', _) => Some(Key::Up),
                            (b'B', _) => Some(Key::Down),
                            (b'C', _) => Some(Key::Right),
                            (b'D', _) => Some(Key::Left),
                            (b'H', _) => Some(Key::Home),
                            (b'F', _) => Some(Key::End),
                            (b'Z', _) => Some(Key::BackTab),
                            (b'~', 3) => Some(Key::Delete),
                            (b'~', 5) => Some(Key::PgUp),
                            (b'~', 6) => Some(Key::PgDn),
                            (b'~', 1) | (b'~', 7) => Some(Key::Home),
                            (b'~', 4) | (b'~', 8) => Some(Key::End),
                            _ => None, /* function keys and others: ignored */
                        };
                        if let Some(k) = k {
                            keys.push(k);
                        }
                        i = j + 1;
                        continue;
                    }
                }
                keys.push(Key::Esc); /* lone ESC, or ESC then an ordinary key */
                i += 1;
            }
            b'\r' | b'\n' => {
                keys.push(Key::Enter);
                i += 1;
            }
            b'\t' => {
                keys.push(Key::Tab);
                i += 1;
            }
            0x7f | 0x08 => {
                keys.push(Key::Backspace);
                i += 1;
            }
            0x03 => {
                keys.push(Key::CtrlC);
                i += 1;
            }
            0x15 => {
                keys.push(Key::CtrlU);
                i += 1;
            }
            0x17 => {
                keys.push(Key::CtrlW);
                i += 1;
            }
            _ if c < 0x20 => i += 1, /* other control bytes: ignored */
            _ => {
                /* UTF-8: the sequence length from the lead byte */
                let len = if c < 0x80 { 1 } else if c >> 5 == 0b110 { 2 } else if c >> 4 == 0b1110 { 3 } else if c >> 3 == 0b11110 { 4 } else { 1 };
                let end = (i + len).min(b.len());
                if let Ok(s) = std::str::from_utf8(&b[i..end]) {
                    if let Some(ch) = s.chars().next() {
                        keys.push(Key::Char(ch));
                    }
                }
                i = end;
            }
        }
    }
    keys
}

// ---- widgets ---------------------------------------------------------------------------------------
/// selection + scroll offset of a list
#[derive(Default, Clone, Copy)]
pub struct Sel {
    pub sel: Option<usize>,
    pub off: usize,
}

impl Sel {
    pub fn selected(&self) -> Option<usize> {
        self.sel
    }
    pub fn select(&mut self, s: Option<usize>) {
        self.sel = s;
        if s.is_none() {
            self.off = 0;
        }
    }
}

/// one list item: one or more lines, each a sequence of styled segments
pub type Item = Vec<Vec<(String, Style)>>;

/// a list in `r` (inside a box) with the selection kept visible; `focus` shows the highlight
pub fn list(scr: &mut Screen, r: Rect, items: &[Item], sel: &mut Sel, focus: bool) {
    if r.h == 0 || items.is_empty() {
        return;
    }
    let per = items[0].len().max(1); /* rows per item (uniform in each list) */
    let vis = (r.h as usize / per).max(1);
    if let Some(s) = sel.sel {
        if s < sel.off {
            sel.off = s;
        } else if s >= sel.off + vis {
            sel.off = s + 1 - vis;
        }
    }
    sel.off = sel.off.min(items.len().saturating_sub(1));
    for (k, item) in items.iter().enumerate().skip(sel.off).take(vis) {
        let is_sel = focus && sel.sel == Some(k);
        for (li, line) in item.iter().enumerate() {
            let y = r.y + ((k - sel.off) * per + li) as u16;
            if y >= r.y + r.h {
                break;
            }
            if is_sel {
                scr.fill(Rect { x: r.x, y, w: r.w, h: 1 }, Style::default().on(SELBG));
            }
            let mut x = r.x;
            let prefix = if li == 0 { if is_sel { "> " } else { "  " } } else { "  " };
            x += scr.text(x, y, prefix, r.w, Style::default().on(if is_sel { SELBG } else { 0 }));
            for (s, st) in line {
                let st = if is_sel { st.on(SELBG).bold() } else { *st };
                let used = scr.text(x, y, s, (r.x + r.w).saturating_sub(x), st);
                x += used;
            }
        }
    }
}

/// wrapped text in `r`, starting `scroll` rows down
pub fn paragraph(scr: &mut Screen, r: Rect, text: &str, scroll: usize, st: Style) {
    let mut rows: Vec<String> = vec![];
    for line in text.split('\n') {
        let mut cur = String::new();
        let mut cw = 0usize;
        for c in line.chars() {
            let w = char_width(c);
            if cw + w > r.w as usize && !cur.is_empty() {
                rows.push(std::mem::take(&mut cur));
                cw = 0;
            }
            cur.push(c);
            cw += w;
        }
        rows.push(cur);
        if rows.len() > scroll + r.h as usize + 1 {
            break; /* nothing further is visible */
        }
    }
    for (i, row) in rows.iter().skip(scroll).take(r.h as usize).enumerate() {
        scr.text(r.x, r.y + i as u16, row, r.w, st);
    }
}

// ---- tests (cargo test --offline) ----------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decode_every_key() {
        let cases: &[(&[u8], &[Key])] = &[
            (b"\x1b[A\x1b[B\x1b[C\x1b[D", &[Key::Up, Key::Down, Key::Right, Key::Left]),
            (b"\x1bOA\x1bOB\x1bOC\x1bOD\x1bOH\x1bOF", &[Key::Up, Key::Down, Key::Right, Key::Left, Key::Home, Key::End]),
            (b"\x1b[5~\x1b[6~\x1b[3~\x1b[Z", &[Key::PgUp, Key::PgDn, Key::Delete, Key::BackTab]),
            (b"\x1b[H\x1b[F\x1b[1~\x1b[4~\x1b[7~\x1b[8~", &[Key::Home, Key::End, Key::Home, Key::End, Key::Home, Key::End]),
            (b"\x1b[1;5C\x1b[5;5~", &[Key::Right, Key::PgUp]), /* modifiers: the first parameter decides */
            (b"\x1b[15~x", &[Key::Char('x')]),                   /* F5: ignored, the rest still decoded */
            (b"\x1b", &[Key::Esc]),
            (b"\x1b3", &[Key::Esc, Key::Char('3')]), /* ESC then a key, never Alt+key */
            (b"\r\n\t\x7f\x08", &[Key::Enter, Key::Enter, Key::Tab, Key::Backspace, Key::Backspace]),
            (b"\x03\x15\x17\x01", &[Key::CtrlC, Key::CtrlU, Key::CtrlW]),
            ("é漢🙂".as_bytes(), &[Key::Char('é'), Key::Char('漢'), Key::Char('🙂')]),
        ];
        for (bytes, want) in cases {
            assert_eq!(decode(bytes), want.to_vec(), "{bytes:?}");
        }
    }

    #[test]
    fn incomplete_sequences() {
        for b in [&b"\x1b"[..], b"\x1b[", b"\x1b[5", b"\x1b[1;", b"ab\x1bO"] {
            assert!(incomplete(b), "{b:?}");
        }
        for b in [&b""[..], b"a", b"\x1b[5~", b"\x1bx", b"\x1b[A"] {
            assert!(!incomplete(b), "{b:?}");
        }
        /* a sequence split over two reads decodes as one key once joined */
        let mut v = b"\x1b[".to_vec();
        assert!(incomplete(&v));
        v.extend_from_slice(b"6~");
        assert_eq!(decode(&v), vec![Key::PgDn]);
    }

    #[test]
    fn widths() {
        assert_eq!(char_width('a'), 1);
        assert_eq!(char_width('\u{7}'), 0);
        assert_eq!(char_width('─'), 1);
        assert_eq!(char_width('漢'), 2);
        assert_eq!(char_width('\u{301}'), 0);
        assert_eq!(str_width("a漢─"), 4);
        assert_eq!(fallback_width('漢' as u32), 2);
        assert_eq!(fallback_width('─' as u32), 1);
        assert_eq!(fallback_width(0x301), 0);
    }

    #[test]
    fn flush_emits_only_changes() {
        let mut s = Screen::headless(10, 3);
        s.text(0, 0, "hi", 10, Style::default());
        s.flush();
        assert!(!s.last_output().is_empty()); /* first frame: everything */
        s.flush();
        assert!(s.last_output().is_empty()); /* identical frame: nothing */
        s.text(0, 0, "X", 10, Style::default());
        s.flush();
        assert_eq!(s.last_output(), b"\x1b[1;1H\x1b[0mX\x1b[0m");
        s.text(4, 2, "y", 10, Style::fg(CYAN).bold());
        s.flush();
        assert_eq!(s.last_output(), b"\x1b[3;5H\x1b[0;1;36my\x1b[0m");
        assert_eq!(s.frames, 4);
    }

    #[test]
    fn wide_characters_take_two_cells() {
        let mut s = Screen::headless(6, 1);
        assert_eq!(s.text(0, 0, "漢a漢", 6, Style::default()), 5);
        assert_eq!(s.text(0, 0, "漢漢漢漢", 5, Style::default()), 4); /* the third would cross the limit */
        let d = s.dump();
        assert_eq!(d.lines().next(), Some("漢漢")); /* the old third character is gone, not half left */
        s.text(1, 0, "b", 6, Style::default()); /* over the right half of the first */
        assert_eq!(s.dump().lines().next(), Some(" b漢"));
    }

    #[test]
    fn dump_lists_styled_runs() {
        let mut s = Screen::headless(8, 2);
        s.text(1, 1, "ab", 8, Style::fg(GREEN).on(SELBG).bold());
        assert_eq!(s.dump(), "\n ab\n--- styles\n1: 1+2=3,9,b\n");
    }

    #[test]
    fn list_keeps_the_selection_visible() {
        let mut s = Screen::headless(20, 7);
        let items: Vec<Item> = (0..30).map(|i| vec![vec![(format!("row {i}"), Style::default())]]).collect();
        let mut sel = Sel { sel: Some(15), off: 0 };
        list(&mut s, Rect { x: 0, y: 0, w: 20, h: 5 }, &items, &mut sel, true);
        assert_eq!(sel.off, 11);
        let d = s.dump();
        assert_eq!(d.lines().nth(4), Some("> row 15"));
        sel.sel = Some(3);
        list(&mut s, Rect { x: 0, y: 0, w: 20, h: 5 }, &items, &mut sel, true);
        assert_eq!(sel.off, 3);
    }

    #[test]
    fn paragraph_wraps_by_cells() {
        let mut s = Screen::headless(4, 4);
        let r = s.area();
        paragraph(&mut s, r, "abcdefgh\n漢漢漢", 0, Style::default());
        let d = s.dump();
        let rows: Vec<&str> = d.lines().take(4).collect();
        assert_eq!(rows, ["abcd", "efgh", "漢漢", "漢"]);
    }

    #[test]
    fn no_color_keeps_bold_and_selection() {
        let mut s = Screen::headless(4, 1);
        s.no_color = true;
        s.text(0, 0, "a", 4, Style::fg(CYAN).on(SELBG).bold());
        s.flush();
        assert_eq!(s.last_output(), b"\x1b[1;1H\x1b[0;1;7ma\x1b[0m   \x1b[0m"); /* no colour numbers at all */
    }
}
