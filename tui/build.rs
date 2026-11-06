//! Links libxksm (static, from the CMake build) and the system libraries it needs.
//! XKSM_LIB_DIR selects the directory holding libxksm.a; default ../build-install, then ../build.
use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let dir = std::env::var("XKSM_LIB_DIR").map(PathBuf::from).unwrap_or_else(|_| {
        let a = root.join("build-install");
        if a.join("libxksm.a").exists() { a } else { root.join("build") }
    });
    let lib = dir.join("libxksm.a");
    if !lib.exists() {
        panic!("libxksm.a not found in {} (build the C library first, or set XKSM_LIB_DIR)", dir.display());
    }
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=static=xksm");
    println!("cargo:rustc-link-lib=dylib=sqlite3");
    println!("cargo:rustc-link-lib=dylib=m");
    println!("cargo:rustc-link-lib=dylib=pthread");
    // the optional OpenCL backend: link the loader only when the archive contains it
    let bytes = std::fs::read(&lib).unwrap_or_default();
    if bytes.windows(14).any(|w| w == b"xksm__ocl_rsvd") {
        println!("cargo:rustc-link-lib=dylib=OpenCL");
    }
    println!("cargo:rerun-if-changed={}", lib.display());
    println!("cargo:rerun-if-env-changed=XKSM_LIB_DIR");
}
