//! term.rs: the whole terminal layer, std + libc only (no terminal crates).
//! Raw mode through termios, size through TIOCGWINSZ, input through poll + read with an own
//! key decoder, output through a double-buffered cell grid that emits only the cells that
//! changed since the last frame (one write per frame).
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

/// display width of a character in cells (0 for combining / control characters)
pub fn char_width(c: char) -> usize {
    if (c as u32) < 0x7f {
        return if (c as u32) < 0x20 { 0 } else { 1 }; /* ASCII fast path */
    }
    let w = unsafe { wcwidth(c as i32) };
    if w < 0 { 0 } else { w as usize }
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
    saved: Termios,
    pub bytes_written: u64,
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
        let mut s = Screen { w: 0, h: 0, back: vec![], front: vec![], out: Vec::with_capacity(1 << 16), saved, bytes_written: 0 };
        s.raw_write(b"\x1b[?1049h\x1b[?25l\x1b[2J");
        s.resize();
        Ok(s)
    }

    fn raw_write(&mut self, b: &[u8]) {
        let mut o = std::io::stdout().lock();
        let _ = o.write_all(b);
        let _ = o.flush();
        self.bytes_written += b.len() as u64;
    }

    /// pick up a terminal resize; returns true when the size changed
    pub fn resize(&mut self) -> bool {
        let mut ws = Winsize { ws_row: 24, ws_col: 80, ws_xpixel: 0, ws_ypixel: 0 };
        unsafe { ioctl(1, TIOCGWINSZ, &mut ws as *mut Winsize) };
        let (w, h) = (ws.ws_col.max(20), ws.ws_row.max(6));
        if w == self.w && h == self.h {
            return false;
        }
        self.w = w;
        self.h = h;
        self.back = vec![BLANK; w as usize * h as usize];
        self.front.clear(); /* repaint everything */
        true
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
            self.back[i] = Cell { ch: c, st };
            if cw == 2 {
                self.back[i + 1] = Cell { ch: '\0', st };
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
                self.out.extend_from_slice(b"\x1b[0");
                if c.st.bold {
                    self.out.extend_from_slice(b";1");
                }
                if c.st.fg != 0 {
                    let n = c.st.fg - 1;
                    let _ = write!(self.out, ";{}", if n < 8 { 30 + n as u32 } else { 90 + n as u32 - 8 });
                }
                if c.st.bg != 0 {
                    let n = c.st.bg - 1;
                    let _ = write!(self.out, ";{}", if n < 8 { 40 + n as u32 } else { 100 + n as u32 - 8 });
                }
                self.out.push(b'm');
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
    }
}

impl Drop for Screen {
    fn drop(&mut self) {
        self.raw_write(b"\x1b[0m\x1b[?25h\x1b[?1049l");
        unsafe { tcsetattr(0, TCSANOW, &self.saved) };
        SAVED.with(|s| *s.borrow_mut() = None);
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
}

/// keys available within `timeout_ms` (empty on timeout)
pub fn read_keys(timeout_ms: i32) -> Vec<Key> {
    let mut p = PollFd { fd: 0, events: POLLIN, revents: 0 };
    if unsafe { poll(&mut p, 1, timeout_ms) } <= 0 {
        return vec![];
    }
    let mut buf = [0u8; 512];
    let n = unsafe { read(0, buf.as_mut_ptr() as *mut std::ffi::c_void, buf.len()) };
    if n <= 0 {
        return vec![];
    }
    decode(&buf[..n as usize])
}

/// decode one read's bytes into keys; a lone ESC (not followed by '[' or 'O') is Esc
pub fn decode(b: &[u8]) -> Vec<Key> {
    let mut keys = vec![];
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        match c {
            0x1b => {
                if i + 2 < b.len() + 1 && i + 1 < b.len() && (b[i + 1] == b'[' || b[i + 1] == b'O') {
                    let mut j = i + 2;
                    let mut num = 0u32;
                    while j < b.len() && (b[j].is_ascii_digit() || b[j] == b';') {
                        if b[j].is_ascii_digit() {
                            num = num * 10 + (b[j] - b'0') as u32;
                        }
                        j += 1;
                    }
                    if j < b.len() {
                        let k = match (b[j], num) {
                            (b'A', _) => Some(Key::Up),
                            (b'B', _) => Some(Key::Down),
                            (b'C', _) => Some(Key::Right),
                            (b'D', _) => Some(Key::Left),
                            (b'H', _) => Some(Key::Home),
                            (b'F', _) => Some(Key::End),
                            (b'Z', _) => Some(Key::BackTab),
                            (b'~', 5) => Some(Key::PgUp),
                            (b'~', 6) => Some(Key::PgDn),
                            (b'~', 1) | (b'~', 7) => Some(Key::Home),
                            (b'~', 4) | (b'~', 8) => Some(Key::End),
                            _ => None,
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
