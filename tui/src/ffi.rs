//! The subset of include/xksm.h the interface uses. Every struct is #[repr(C)] and matches
//! the C definition field for field (C enums are int).
#![allow(non_camel_case_types, dead_code)]
use std::os::raw::{c_char, c_int};

pub type xksm_status = c_int;
pub const XKSM_OK: xksm_status = 0;
pub const XKSM_ENOTFOUND: xksm_status = 2;

#[repr(C)]
pub struct xksm {
    _p: [u8; 0],
}

#[repr(C)]
pub struct xksm_memory {
    pub id: i64,
    pub ns: *mut c_char,
    pub category: c_int,
    pub state: c_int,
    pub text: *mut c_char,
    pub text_len: usize,
    pub created_us: i64,
    pub modified_us: i64,
    pub event_us: i64,
    pub importance: f64,
    pub pinned: c_int,
    pub source_id: i64,
    pub checksum: [u8; 32],
}

#[repr(C)]
pub struct xksm_search_opts {
    pub ns: *const c_char,
    pub categories: u32,
    pub match_: c_int,
    pub limit: usize,
}

#[repr(C)]
pub struct xksm_search_hit {
    pub id: i64,
    pub score: f64,
    pub snippet: *mut c_char,
}

#[repr(C)]
pub struct xksm_search_result {
    pub hits: *mut xksm_search_hit,
    pub count: usize,
}

#[repr(C)]
pub struct xksm_hybrid_opts {
    pub ns: *const c_char,
    pub categories: u32,
    pub match_: c_int,
    pub limit: usize,
    pub depth: usize,
    pub k: f64,
    pub w_lexical: f64,
    pub w_vector: f64,
}

#[repr(C)]
pub struct xksm_hybrid_hit {
    pub id: i64,
    pub score: f64,
    pub lexical_rank: usize,
    pub vector_rank: usize,
    pub lexical_score: f64,
    pub vector_score: f64,
    pub snippet: *mut c_char,
}

#[repr(C)]
pub struct xksm_hybrid_result {
    pub hits: *mut xksm_hybrid_hit,
    pub count: usize,
}

#[repr(C)]
pub struct xksm_semantic_report {
    pub memories: usize,
    pub embedded: usize,
    pub tokens: usize,
    pub vocab: usize,
    pub pairs: u64,
    pub nnz: u64,
    pub t_tokens: f64,
    pub t_cooc: f64,
    pub t_ppmi: f64,
    pub t_svd: f64,
    pub t_embed: f64,
    pub t_store: f64,
    pub t_total: f64,
    pub gpu_used: c_int,
    pub device: [c_char; 64],
}

#[repr(C)]
pub struct xksm_cache_info {
    pub records: i64,
    pub capacity: u32,
    pub resident: u32,
    pub hits: u64,
    pub misses: u64,
    pub evictions: u64,
    pub bytes: usize,
    pub byte_limit: usize,
    pub percent: c_int,
}

#[repr(C)]
pub struct xksm_session_info {
    pub harness: *mut c_char,
    pub id: *mut c_char,
    pub cwd: *mut c_char,
    pub first_us: i64,
    pub last_us: i64,
    pub turns: i64,
}

#[repr(C)]
pub struct xksm_ns_count {
    pub ns: *mut c_char,
    pub memories: i64,
    pub embedded: i64,
}

#[repr(C)]
pub struct xksm_memory_in {
    pub ns: *const c_char,
    pub category: c_int,
    pub text: *const c_char,
    pub text_len: usize,
    pub event_us: i64,
    pub importance: f64,
    pub source_id: i64,
    pub source_off: i64,
    pub source_len: i64,
}

#[repr(C)]
pub struct xksm_import_opts {
    pub ns: *const c_char,
    pub category: c_int,
    pub format: c_int,
    pub chunk_target: usize,
    pub event_us: i64,
    pub header: *const c_char,
}

#[repr(C)]
pub struct xksm_import_result {
    pub source_id: i64,
    pub chunks: usize,
    pub added: usize,
    pub duplicates: usize,
}

pub const XKSM_EXISTS: xksm_status = 6;
pub const XKSM_ECORRUPT: xksm_status = 7;
pub const XKSM_CAT_SEMANTIC: c_int = 2;
pub const XKSM_CAT_PROJECT: c_int = 4;

#[repr(C)]
pub struct xksm_verify_report {
    pub memories: usize,
    pub memory_bad: usize,
    pub sources: usize,
    pub source_bad: usize,
    pub sources_without_body: usize,
    pub files: usize,
    pub file_bad: usize,
    pub file_missing: usize,
    pub first_bad_memory: i64,
    pub first_bad_source: i64,
    pub first_bad_file_source: i64,
    pub seconds: f64,
}

extern "C" {
    pub fn xksm_open(path: *const c_char, out: *mut *mut xksm) -> xksm_status;
    pub fn xksm_close(db: *mut xksm);
    pub fn xksm_errmsg(db: *const xksm) -> *const c_char;
    pub fn xksm_schema_version(db: *mut xksm) -> c_int;
    pub fn xksm_version() -> *const c_char;
    pub fn xksm_memory_get(db: *mut xksm, id: i64, out: *mut xksm_memory) -> xksm_status;
    pub fn xksm_memory_free(m: *mut xksm_memory);
    pub fn xksm_memory_delete(db: *mut xksm, id: i64) -> xksm_status;
    pub fn xksm_source_info(db: *mut xksm, source: i64, uri: *mut c_char, cap: usize, imported_us: *mut i64) -> xksm_status;
    pub fn xksm_search(db: *mut xksm, query: *const c_char, opts: *const xksm_search_opts, out: *mut xksm_search_result) -> xksm_status;
    pub fn xksm_search_free(r: *mut xksm_search_result);
    pub fn xksm_semantic_recall(db: *mut xksm, text: *const c_char, xo: *const u8, ho: *const xksm_hybrid_opts, out: *mut xksm_hybrid_result) -> xksm_status;
    pub fn xksm_hybrid_free(r: *mut xksm_hybrid_result);
    pub fn xksm_semantic_build(db: *mut xksm, opts: *const u8, rep: *mut xksm_semantic_report) -> xksm_status;
    pub fn xksm_semantic_update(db: *mut xksm, embedded: *mut usize) -> xksm_status;
    pub fn xksm_semantic_built_memories(db: *mut xksm) -> usize;
    pub fn xksm_semantic_dim(db: *mut xksm) -> usize;
    pub fn xksm_semantic_port_set(db: *mut xksm, path: *const c_char) -> xksm_status;
    pub fn xksm_semantic_port_path(db: *mut xksm, buf: *mut c_char, cap: usize) -> usize;
    pub fn xksm_cache_stats(db: *const xksm, out: *mut xksm_cache_info);
    pub fn xksm_session_list(db: *mut xksm, harness: *const c_char, limit: usize, out: *mut *mut xksm_session_info, count: *mut usize) -> xksm_status;
    pub fn xksm_session_list_free(list: *mut xksm_session_info, count: usize);
    pub fn xksm_session_forget(db: *mut xksm, harness: *const c_char, id: *const c_char, deleted: *mut usize) -> xksm_status;
    pub fn xksm_session_memories(db: *mut xksm, harness: *const c_char, id: *const c_char, ids: *mut *mut i64, count: *mut usize) -> xksm_status;
    pub fn xksm_ns_counts(db: *mut xksm, model: *const c_char, out: *mut *mut xksm_ns_count, count: *mut usize) -> xksm_status;
    pub fn xksm_ns_counts_free(list: *mut xksm_ns_count, count: usize);
    pub fn xksm_kv_get(db: *mut xksm, key: *const c_char, value: *mut u8, cap: usize, len: *mut usize) -> xksm_status;
    pub fn xksm_kv_put(db: *mut xksm, key: *const c_char, value: *const u8, len: usize) -> xksm_status;
    pub fn xksm_memory_add(db: *mut xksm, input: *const xksm_memory_in, id_out: *mut i64) -> xksm_status;
    pub fn xksm_import_file(db: *mut xksm, path: *const c_char, opts: *const xksm_import_opts, result: *mut xksm_import_result) -> xksm_status;
    pub fn xksm_memory_count(db: *mut xksm, ns: *const c_char) -> i64;
    pub fn xksm_mirror_prune(db: *mut xksm, source: i64) -> xksm_status;
    pub fn xksm_change_counter(db: *mut xksm) -> i64;
    pub fn xksm_sha256_digest(data: *const u8, n: usize, out: *mut u8);
    pub fn xksm_source_sha256(db: *mut xksm, source: i64, out: *mut u8, bytes: *mut i64) -> xksm_status;
    pub fn xksm_file_matches(path: *const c_char, sha: *const u8) -> xksm_status;
    pub fn xksm_verify(db: *mut xksm, root: *const c_char, out: *mut xksm_verify_report) -> xksm_status;
    pub fn xksm_memory_ids(db: *mut xksm, ns: *const c_char, offset: usize, limit: usize, ids: *mut *mut i64, count: *mut usize) -> xksm_status;
    pub fn free(p: *mut std::ffi::c_void);
}
