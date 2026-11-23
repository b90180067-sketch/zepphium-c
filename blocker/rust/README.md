# blockers/rust - adblock-rust C ABI shim

This crate is the only Rust in the tree. It wraps Brave's
[`adblock-rust`](https://github.com/brave/adblock-rust) engine in a small C ABI
(`blocker/adblock_ffi.h`) so the Win32/WebView2 shell can filter network
requests natively, without browser extensions.

## Build

The shell is built with MinGW, so the shim is built for the matching GNU target:

```powershell
# one-time, see windows/fetch_rust.ps1
rustup toolchain install stable-x86_64-pc-windows-gnu
cargo build --release --manifest-path blocker/rust/Cargo.toml
```

Output: `blocker/rust/target/release/libzephium_adblock.a` (about 9 MB) on the
GNU host toolchain, linked into `windows/zephium_win.exe` by `windows/Makefile`.
(CI builds it with `--target x86_64-pc-windows-gnu`, where the path gains that
target triple.)

## Test

```powershell
cargo test --manifest-path blocker/rust/Cargo.toml
```

The tests exercise the FFI the same way the C++ shell does: block/allow decisions,
`$redirect` bodies, binary engine cache, thread affinity, panic containment and
cosmetic CSS.

## Rules of the road

- Every `extern "C"` entry point catches panics (`catch_unwind`) and never
  unwinds across the FFI boundary.
- The engine is built with adblock-rust's `single-thread` feature for speed; the
  handle is therefore *thread affine* and every call checks it.
- The cache format is owned by this crate (magic + ABI header + flatbuffer dump);
  a mismatched cache is rejected and the caller rebuilds.

The filter lists themselves are **not** vendored here: they are downloaded at
runtime and cached (see `docs/ADBLOCK.md`).
