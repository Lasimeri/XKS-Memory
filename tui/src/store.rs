//! Safe wrapper over the libxksm C API: every C allocation is copied into owned Rust values
//! and released in the same function, so no raw pointer leaves this module.
use crate::ffi::*;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

pub struct Store {
    db: *mut xksm,
    pub path: String,
}

#[derive(Clone)]
pub struct Session {
    pub harness: String,
    pub id: String,
    pub cwd: String,
    pub first_us: i64,
    pub last_us: i64,
    pub turns: i64,
}

#[derive(Clone)]
pub struct Memory {
    pub id: i64,
    pub ns: String,
    pub text: String,
    pub when_us: i64,
    pub uri: String,
}

#[derive(Clone)]
pub struct Hit {
    pub id: i64,
    pub score: f64,
    pub lexical_rank: usize,
    pub vector_rank: usize,
    pub vector_score: f64,
}

pub struct NsCount {
    pub ns: String,
    pub memories: i64,
    pub embedded: i64,
}

pub struct BuildReport {
    pub memories: usize,
    pub embedded: usize,
    pub vocab: usize,
    pub nnz: u64,
    pub t_svd: f64,
    pub t_total: f64,
}

pub struct CacheInfo {
    pub capacity: u32,
    pub resident: u32,
    pub hits: u64,
    pub misses: u64,
    pub bytes: usize,
    pub percent: i32,
}

fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}

fn c(s: &str) -> CString {
    CString::new(s.replace('\0', " ")).expect("no interior NUL after replacement")
}

/// role and session of a source uri xksm-turn://HARNESS/SESSION/ROLE/TIME
pub fn uri_parts(uri: &str) -> (String, String, String) {
    let rest = match uri.strip_prefix("xksm-turn://") {
        Some(r) => r,
        None => return (String::new(), String::new(), String::new()),
    };
    let mut it = rest.splitn(4, '/');
    let h = it.next().unwrap_or("").to_string();
    let s = it.next().unwrap_or("").to_string();
    let r = it.next().unwrap_or("").to_string();
    (h, s, r)
}

impl Store {
    pub fn open(path: &str) -> Result<Store, String> {
        let cp = c(path);
        let mut db: *mut xksm = ptr::null_mut();
        let rc = unsafe { xksm_open(cp.as_ptr(), &mut db) };
        if rc != XKSM_OK {
            let msg = if db.is_null() { format!("status {rc}") } else { cstr(unsafe { xksm_errmsg(db) }) };
            unsafe { xksm_close(db) };
            return Err(format!("{path}: {msg}"));
        }
        Ok(Store { db, path: path.to_string() })
    }

    pub fn err(&self) -> String {
        cstr(unsafe { xksm_errmsg(self.db) })
    }

    fn check(&self, rc: xksm_status) -> Result<(), String> {
        if rc == XKSM_OK { Ok(()) } else { Err(self.err()) }
    }

    pub fn version() -> String {
        cstr(unsafe { xksm_version() })
    }

    pub fn schema(&self) -> i32 {
        unsafe { xksm_schema_version(self.db) }
    }

    pub fn sessions(&self, harness: Option<&str>) -> Result<Vec<Session>, String> {
        let h = harness.map(c);
        let mut list: *mut xksm_session_info = ptr::null_mut();
        let mut n = 0usize;
        self.check(unsafe {
            xksm_session_list(self.db, h.as_ref().map_or(ptr::null(), |x| x.as_ptr()), 0, &mut list, &mut n)
        })?;
        let mut out = Vec::with_capacity(n);
        for i in 0..n {
            let s = unsafe { &*list.add(i) };
            out.push(Session {
                harness: cstr(s.harness),
                id: cstr(s.id),
                cwd: cstr(s.cwd),
                first_us: s.first_us,
                last_us: s.last_us,
                turns: s.turns,
            });
        }
        unsafe { xksm_session_list_free(list, n) };
        Ok(out)
    }

    pub fn session_memories(&self, harness: &str, id: &str) -> Result<Vec<i64>, String> {
        let (h, s) = (c(harness), c(id));
        let mut ids: *mut i64 = ptr::null_mut();
        let mut n = 0usize;
        self.check(unsafe { xksm_session_memories(self.db, h.as_ptr(), s.as_ptr(), &mut ids, &mut n) })?;
        let v = (0..n).map(|i| unsafe { *ids.add(i) }).collect();
        unsafe { free(ids as *mut _) };
        Ok(v)
    }

    pub fn forget(&self, harness: &str, id: &str) -> Result<usize, String> {
        let (h, s) = (c(harness), c(id));
        let mut d = 0usize;
        self.check(unsafe { xksm_session_forget(self.db, h.as_ptr(), s.as_ptr(), &mut d) })?;
        Ok(d)
    }

    pub fn memory(&self, id: i64) -> Option<Memory> {
        let mut m: xksm_memory = unsafe { std::mem::zeroed() };
        if unsafe { xksm_memory_get(self.db, id, &mut m) } != XKSM_OK {
            return None;
        }
        let text = if m.text.is_null() {
            String::new()
        } else {
            let b = unsafe { std::slice::from_raw_parts(m.text as *const u8, m.text_len) };
            String::from_utf8_lossy(b).into_owned()
        };
        let mut uri = [0 as c_char; 768];
        let mut imp = 0i64;
        if m.source_id != 0 {
            unsafe { xksm_source_info(self.db, m.source_id, uri.as_mut_ptr(), uri.len(), &mut imp) };
        }
        let out = Memory {
            id: m.id,
            ns: cstr(m.ns),
            text,
            when_us: if m.event_us != i64::MIN { m.event_us } else { m.created_us },
            uri: cstr(uri.as_ptr()),
        };
        unsafe { xksm_memory_free(&mut m) };
        Some(out)
    }

    pub fn delete(&self, id: i64) -> Result<(), String> {
        self.check(unsafe { xksm_memory_delete(self.db, id) })
    }

    pub fn recall(&self, q: &str, ns: Option<&str>, limit: usize) -> Result<Vec<Hit>, String> {
        let cq = c(q);
        let cns = ns.map(c);
        let ho = xksm_hybrid_opts {
            ns: cns.as_ref().map_or(ptr::null(), |x| x.as_ptr()),
            categories: 0,
            match_: 0,
            limit,
            depth: 0,
            k: 0.0,
            w_lexical: 0.0,
            w_vector: 0.0,
        };
        let mut r = xksm_hybrid_result { hits: ptr::null_mut(), count: 0 };
        self.check(unsafe { xksm_semantic_recall(self.db, cq.as_ptr(), ptr::null(), &ho, &mut r) })?;
        let v = (0..r.count)
            .map(|i| {
                let h = unsafe { &*r.hits.add(i) };
                Hit { id: h.id, score: h.score, lexical_rank: h.lexical_rank, vector_rank: h.vector_rank, vector_score: h.vector_score }
            })
            .collect();
        unsafe { xksm_hybrid_free(&mut r) };
        Ok(v)
    }

    pub fn words(&self, q: &str, ns: Option<&str>, limit: usize) -> Result<Vec<Hit>, String> {
        let cq = c(q);
        let cns = ns.map(c);
        let so = xksm_search_opts { ns: cns.as_ref().map_or(ptr::null(), |x| x.as_ptr()), categories: 0, match_: 0, limit };
        let mut r = xksm_search_result { hits: ptr::null_mut(), count: 0 };
        self.check(unsafe { xksm_search(self.db, cq.as_ptr(), &so, &mut r) })?;
        let v = (0..r.count)
            .map(|i| {
                let h = unsafe { &*r.hits.add(i) };
                Hit { id: h.id, score: h.score, lexical_rank: i + 1, vector_rank: 0, vector_score: 0.0 }
            })
            .collect();
        unsafe { xksm_search_free(&mut r) };
        Ok(v)
    }

    pub fn ns_counts(&self) -> Result<Vec<NsCount>, String> {
        let mut l: *mut xksm_ns_count = ptr::null_mut();
        let mut n = 0usize;
        self.check(unsafe { xksm_ns_counts(self.db, ptr::null(), &mut l, &mut n) })?;
        let v = (0..n)
            .map(|i| {
                let x = unsafe { &*l.add(i) };
                NsCount { ns: cstr(x.ns), memories: x.memories, embedded: x.embedded }
            })
            .collect();
        unsafe { xksm_ns_counts_free(l, n) };
        Ok(v)
    }

    pub fn table(&self) -> (usize, usize) {
        unsafe { (xksm_semantic_built_memories(self.db), xksm_semantic_dim(self.db)) }
    }

    pub fn build(&self) -> Result<BuildReport, String> {
        let mut r: xksm_semantic_report = unsafe { std::mem::zeroed() };
        self.check(unsafe { xksm_semantic_build(self.db, ptr::null(), &mut r) })?;
        Ok(BuildReport { memories: r.memories, embedded: r.embedded, vocab: r.vocab, nnz: r.nnz, t_svd: r.t_svd, t_total: r.t_total })
    }

    pub fn update(&self) -> Result<usize, String> {
        let mut n = 0usize;
        let rc = unsafe { xksm_semantic_update(self.db, &mut n) };
        if rc == XKSM_ENOTFOUND {
            return Err("no word-vector table built yet (build it first)".into());
        }
        self.check(rc)?;
        Ok(n)
    }

    pub fn port(&self) -> String {
        let mut b = [0 as c_char; 4096];
        unsafe { xksm_semantic_port_path(self.db, b.as_mut_ptr(), b.len()) };
        cstr(b.as_ptr())
    }

    pub fn set_port(&self, path: &str) -> Result<(), String> {
        let p = c(path);
        self.check(unsafe { xksm_semantic_port_set(self.db, p.as_ptr()) })
    }

    pub fn cache(&self) -> CacheInfo {
        let mut ci: xksm_cache_info = unsafe { std::mem::zeroed() };
        unsafe { xksm_cache_stats(self.db, &mut ci) };
        CacheInfo { capacity: ci.capacity, resident: ci.resident, hits: ci.hits, misses: ci.misses, bytes: ci.bytes, percent: ci.percent }
    }

    /// active memories counted now (sees other processes' inserts)
    pub fn count(&self, ns: Option<&str>) -> i64 {
        let cns = ns.map(c);
        unsafe { xksm_memory_count(self.db, cns.as_ref().map_or(ptr::null(), |x| x.as_ptr())) }
    }

    /// memory ids of a namespace (None = all), newest first
    pub fn memory_ids(&self, ns: Option<&str>, offset: usize, limit: usize) -> Result<Vec<i64>, String> {
        let cns = ns.map(c);
        let mut ids: *mut i64 = ptr::null_mut();
        let mut n = 0usize;
        self.check(unsafe {
            xksm_memory_ids(self.db, cns.as_ref().map_or(ptr::null(), |x| x.as_ptr()), offset, limit, &mut ids, &mut n)
        })?;
        let v = (0..n).map(|i| unsafe { *ids.add(i) }).collect();
        unsafe { free(ids as *mut _) };
        Ok(v)
    }

    /// a note with the current time, embedded at once when a table exists
    pub fn add_note(&self, ns: &str, text: &str) -> Result<(i64, bool), String> {
        let cns = c(ns);
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_micros() as i64).unwrap_or(0);
        let input = xksm_memory_in {
            ns: cns.as_ptr(),
            category: XKSM_CAT_SEMANTIC,
            text: text.as_ptr() as *const c_char,
            text_len: text.len(),
            event_us: now,
            importance: 0.0,
            source_id: 0,
            source_off: 0,
            source_len: 0,
        };
        let mut id = 0i64;
        let rc = unsafe { xksm_memory_add(self.db, &input, &mut id) };
        if rc != XKSM_OK && rc != XKSM_EXISTS {
            return Err(self.err());
        }
        Ok((id, rc == XKSM_EXISTS))
    }

    /// import a text, Markdown, JSON, JSONL or CSV file (format from the extension): (chunks, added, duplicates)
    pub fn import_file(&self, path: &str, ns: &str) -> Result<(usize, usize, usize), String> {
        let (cp, cns) = (c(path), c(ns));
        let o = xksm_import_opts { ns: cns.as_ptr(), category: XKSM_CAT_PROJECT, format: 0, chunk_target: 0, event_us: 0, header: ptr::null() };
        let mut r = xksm_import_result { source_id: 0, chunks: 0, added: 0, duplicates: 0 };
        let rc = unsafe { xksm_import_file(self.db, cp.as_ptr(), &o, &mut r) };
        if rc == XKSM_EXISTS {
            return Err(format!("{path}: an identical file is already stored (source {})", r.source_id));
        }
        self.check(rc)?;
        Ok((r.chunks, r.added, r.duplicates))
    }

    pub fn paused(&self, harness: &str) -> bool {
        let k = c(&format!("{harness}:paused"));
        let mut v = [0u8; 4];
        let mut n = 0usize;
        unsafe { xksm_kv_get(self.db, k.as_ptr(), v.as_mut_ptr(), v.len(), &mut n) == XKSM_OK && n == 1 && v[0] == b'1' }
    }

    pub fn set_paused(&self, harness: &str, on: bool) -> Result<(), String> {
        let k = c(&format!("{harness}:paused"));
        let v = if on { b"1" } else { b"0" };
        self.check(unsafe { xksm_kv_put(self.db, k.as_ptr(), v.as_ptr(), 1) })
    }
}

impl Drop for Store {
    fn drop(&mut self) {
        unsafe { xksm_close(self.db) };
    }
}
