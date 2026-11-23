//! Zephium ad blocking: C ABI shim around Brave's adblock-rust engine.
//!
//! The Win32/WebView2 shell links the static archive built from this crate and
//! calls it from its WebResourceRequested handler (see blocker/adblock_ffi.h and
//! docs/ADBLOCK.md).
//!
//! Design notes that matter for callers:
//!
//! * adblock-rust is compiled with its default "single-thread" feature. That is
//!   the fastest mode but the Engine is deliberately !Send/!Sync, so this shim
//!   makes every handle thread affine: the creating thread is recorded and every
//!   entry point verifies it before touching the engine. No locks on the hot
//!   path, no undefined behaviour if a caller misuses the handle.
//! * Every extern "C" function contains panics with catch_unwind. A contained
//!   panic poisons the handle so that later checks fail open (allow) instead of
//!   crashing the browser or blocking traffic.
//! * The binary cache carries our own magic/ABI header plus the adblock-rust
//!   flatbuffer dump, so a cache written by another build is rejected instead of
//!   being fed into an incompatible deserializer.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::thread::{self, ThreadId};
use std::time::Instant;

use adblock::lists::{FilterSet, ParseOptions};
use adblock::request::Request;
use adblock::resources::{MimeType, PermissionMask, Resource, ResourceType};
use adblock::Engine as AdblockEngine;
use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine as Base64Engine;

/* ------------------------------------------------------------------ constants */

const ABI_VERSION: u32 = 1;

const STATUS_OK: u32 = 0;
const STATUS_ARGUMENT: u32 = 1;
const STATUS_STATE: u32 = 2;
const STATUS_PANIC: u32 = 3;
const STATUS_FILTERS: u32 = 4;
const STATUS_CACHE: u32 = 5;
const STATUS_THREAD: u32 = 6;

const VERDICT_ALLOW: u32 = 0;
const VERDICT_BLOCK: u32 = 1;
const VERDICT_REDIRECT: u32 = 2;
const VERDICT_REWRITE: u32 = 3;

const FLAG_DEBUG_RULES: u32 = 0x1;

/* Refuse absurd inputs before allocating anything: a filter list bigger than
 * this is a bug or a corrupted download, not a real list. */
const MAX_LIST_BYTES: usize = 512 * 1024 * 1024;
const MAX_CACHE_BYTES: u64 = 512 * 1024 * 1024;

const CACHE_MAGIC: &[u8; 8] = b"ZEPHADB1";
const CACHE_HEADER_BYTES: usize = 32;

const VERSION_CSTR: &[u8] = b"zephium-adblock-ffi 1.0.0 (adblock-rust 0.13)\0";

/* ------------------------------------------------------------------- built-ins */

/* Redirect bodies for the handful of placeholder resources that uBlock/EasyList
 * rules reference most often, so "$redirect=noop.js"-style rules actually work
 * instead of degrading to a plain block. Content is base64 because that is what
 * the Resource struct expects; everything here is content we generate ourselves
 * (a transparent pixel, an empty no-op script, an empty HTML frame). */
const RES_1X1_GIF: &str = "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
const RES_1X1_PNG: &str = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
const RES_NOOP_JS: &str = "KGZ1bmN0aW9uKCkge30pKCk7"; /* (function() {})(); */
const RES_EMPTY: &str = "";
const RES_NOOP_FRAME: &str = "PCFET0NUWVBFIGh0bWw+PGh0bWw+PGhlYWQ+PC9oZWFkPjxib2R5PjwvYm9keT48L2h0bWw+";

/* ------------------------------------------------------------------ C structs */

#[repr(C)]
pub struct zephium_adblock_result_t {
    pub verdict: u32,
    pub important: u32,
    pub redirect_body: *const c_char,
    pub redirect_len: usize,
    pub redirect_mime: *const c_char,
    pub rewritten_url: *const c_char,
    pub rewritten_len: usize,
    pub matched_rule: *const c_char,
    pub elapsed_ns: u64,
}

#[repr(C)]
pub struct zephium_adblock_stats_t {
    pub abi_version: u32,
    pub struct_size: u32,
    pub rule_count: u64,
    pub list_bytes: u64,
    pub checks: u64,
    pub blocked: u64,
    pub redirected: u64,
    pub rewritten: u64,
    pub panics: u64,
    pub last_check_ns: u64,
    pub total_check_ns: u64,
}

/* Opaque in the C header; never instantiated directly. */
#[repr(C)]
pub struct zephium_adblock_engine {
    _private: [u8; 0],
}

/* ------------------------------------------------------------------- internals */

#[derive(Default)]
struct Stats {
    checks: u64,
    blocked: u64,
    redirected: u64,
    rewritten: u64,
    panics: u64,
    last_check_ns: u64,
    total_check_ns: u64,
}

struct EngineHandle {
    owner: ThreadId,
    debug_rules: bool,
    engine: Option<AdblockEngine>,
    poisoned: bool,
    rule_count: u64,
    list_bytes: u64,
    /* Buffers backing zephium_adblock_result_t pointers. */
    redirect_body: Vec<u8>,
    redirect_mime: CString,
    rewritten: CString,
    matched: CString,
    stats: Stats,
}

thread_local! {
    /* CString::default() is empty; const-init is not possible for it because
     * CString::new is not a const fn. */
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_last_error(message: &str) {
    let cleaned = message.replace('\0', " ");
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new(cleaned).unwrap_or_default();
    });
}

fn cstring(text: &str) -> CString {
    CString::new(text.replace('\0', " ")).unwrap_or_default()
}

unsafe fn read_cstr(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    std::ffi::CStr::from_ptr(pointer).to_string_lossy().into_owned()
}

unsafe fn read_bytes(pointer: *const c_char, length: usize) -> String {
    if pointer.is_null() || length == 0 {
        return String::new();
    }
    let slice = std::slice::from_raw_parts(pointer as *const u8, length);
    String::from_utf8_lossy(slice).into_owned()
}

fn null_result() -> zephium_adblock_result_t {
    zephium_adblock_result_t {
        verdict: VERDICT_ALLOW,
        important: 0,
        redirect_body: std::ptr::null(),
        redirect_len: 0,
        redirect_mime: std::ptr::null(),
        rewritten_url: std::ptr::null(),
        rewritten_len: 0,
        matched_rule: std::ptr::null(),
        elapsed_ns: 0,
    }
}

fn panic_text(payload: &(dyn std::any::Any + Send)) -> String {
    if let Some(text) = payload.downcast_ref::<&str>() {
        (*text).to_string()
    } else if let Some(text) = payload.downcast_ref::<String>() {
        text.clone()
    } else {
        "non-string panic payload".to_string()
    }
}

/* Validates the handle and that we are on its owning thread. The returned raw
 * pointer must be dereferenced only inside this thread. */
unsafe fn acquire(engine: *mut zephium_adblock_engine) -> Result<*mut EngineHandle, u32> {
    if engine.is_null() {
        set_last_error("NULL engine handle");
        return Err(STATUS_ARGUMENT);
    }
    let handle = engine as *mut EngineHandle;
    if (*handle).owner != thread::current().id() {
        set_last_error("engine handle used from a thread other than its owner");
        return Err(STATUS_THREAD);
    }
    Ok(handle)
}

/* Runs one engine call with panic containment. A panic poisons the handle so the
 * browser keeps working with ad blocking disabled instead of crashing. */
fn catch_engine_call(handle: *mut EngineHandle, what: &str, body: impl FnOnce() -> u32) -> u32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(status) => status,
        Err(payload) => {
            let message = panic_text(payload.as_ref());
            unsafe {
                let handle = &mut *handle;
                handle.poisoned = true;
                handle.stats.panics = handle.stats.panics.saturating_add(1);
            }
            set_last_error(&format!("{what}: contained Rust panic: {message}"));
            STATUS_PANIC
        }
    }
}

fn request_type_name(resource_type: u32) -> &'static str {
    match resource_type {
        1 => "document",
        2 => "sub_frame",
        3 => "script",
        4 => "stylesheet",
        5 => "image",
        6 => "font",
        7 => "media",
        8 => "xhr",
        9 => "ping",
        10 => "websocket",
        11 => "manifest",
        12 => "object",
        _ => "other",
    }
}

fn builtin_resources() -> Vec<Resource> {
    fn resource(name: &str, aliases: &[&str], mime: MimeType, content: &str) -> Resource {
        Resource {
            name: name.to_string(),
            aliases: aliases.iter().map(|alias| alias.to_string()).collect(),
            kind: ResourceType::Mime(mime),
            content: content.to_string(),
            dependencies: Vec::new(),
            permission: PermissionMask::default(),
        }
    }

    vec![
        resource("1x1-transparent.gif", &[], MimeType::ImageGif, RES_1X1_GIF),
        resource("1x1-transparent.png", &[], MimeType::ImagePng, RES_1X1_PNG),
        resource("2x2-transparent.png", &[], MimeType::ImagePng, RES_1X1_PNG),
        resource("3x2-transparent.png", &[], MimeType::ImagePng, RES_1X1_PNG),
        resource("32x32-transparent.png", &[], MimeType::ImagePng, RES_1X1_PNG),
        resource("noop.js", &["noopjs"], MimeType::ApplicationJavascript, RES_NOOP_JS),
        resource("noop.css", &["noopcss"], MimeType::TextCss, RES_EMPTY),
        resource("noop.txt", &["nooptext"], MimeType::TextPlain, RES_EMPTY),
        resource("noopframe", &["noop.html"], MimeType::TextHtml, RES_NOOP_FRAME),
        resource("empty", &["empty.js", "empty.txt", "empty.css"], MimeType::TextPlain, RES_EMPTY),
    ]
}

fn build_engine(lists: &[String], debug_rules: bool) -> AdblockEngine {
    let mut set = FilterSet::new(debug_rules);
    for list in lists {
        /* add_filter_list never fails: unparseable rules are counted and skipped,
         * which is exactly the behaviour we want for third-party lists. */
        set.add_filter_list(list.clone(), ParseOptions::default());
    }
    let mut engine = AdblockEngine::new_with_filter_set(set);
    engine.use_resources(builtin_resources());
    engine
}

/* adblock-rust does not expose the compiled rule count, so count the source
 * lines that are rules (not blank, not ! comments, not [Adblock Plus] headers).
 * Good enough for logs and for the shell's "N rules loaded" line. */
fn count_rules(lists: &[String]) -> u64 {
    let mut count: u64 = 0;
    for list in lists {
        for line in list.lines() {
            let trimmed = line.trim();
            if trimmed.is_empty() || trimmed.starts_with('!') || trimmed.starts_with('[') {
                continue;
            }
            count += 1;
        }
    }
    count
}

/* A matched $redirect is always a data: URL (adblock-rust builds it from the
 * resource's bytes). Decode it so the shell can answer the request with real
 * bytes and a Content-Type it can pass to WebView2. */
fn decode_redirect(uri: &str) -> (Vec<u8>, String) {
    if let Some(rest) = uri.strip_prefix("data:") {
        if let Some((meta, payload)) = rest.split_once(',') {
            let mime = meta.split(';').next().unwrap_or("text/plain").trim().to_string();
            let mime = if mime.is_empty() { "text/plain".to_string() } else { mime };
            if meta.contains(";base64") {
                if let Ok(bytes) = BASE64.decode(payload) {
                    return (bytes, mime);
                }
            } else {
                return (payload.as_bytes().to_vec(), mime);
            }
        }
    }
    (uri.as_bytes().to_vec(), "text/plain".to_string())
}

fn cache_header(rule_count: u64, list_bytes: u64) -> [u8; CACHE_HEADER_BYTES] {
    let mut header = [0u8; CACHE_HEADER_BYTES];
    header[..8].copy_from_slice(CACHE_MAGIC);
    header[8..12].copy_from_slice(&ABI_VERSION.to_le_bytes());
    /* bytes 12..16 are reserved */
    header[16..24].copy_from_slice(&rule_count.to_le_bytes());
    header[24..32].copy_from_slice(&list_bytes.to_le_bytes());
    header
}

fn save_cache_file(engine: &AdblockEngine, rule_count: u64, list_bytes: u64, path: &str) -> Result<(), String> {
    let payload = engine.serialize();
    if payload.is_empty() {
        return Err("serialize() produced an empty buffer".to_string());
    }

    let mut file_bytes = Vec::with_capacity(CACHE_HEADER_BYTES + payload.len());
    file_bytes.extend_from_slice(&cache_header(rule_count, list_bytes));
    file_bytes.extend_from_slice(&payload);

    let target = Path::new(path);
    if let Some(parent) = target.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).map_err(|error| format!("create_dir_all({parent:?}): {error}"))?;
        }
    }
    /* Write next to the target and rename, so a reader never sees a partial file
     * and a crash mid-write only loses the cache. */
    let temporary = format!("{path}.tmp");
    std::fs::write(&temporary, &file_bytes).map_err(|error| format!("write({temporary}): {error}"))?;
    if target.exists() {
        let _ = std::fs::remove_file(target);
    }
    std::fs::rename(&temporary, target).map_err(|error| format!("rename({temporary} -> {path}): {error}"))?;
    Ok(())
}

fn load_cache_file(path: &str) -> Result<(AdblockEngine, u64, u64), String> {
    let metadata = std::fs::metadata(path).map_err(|error| format!("metadata({path}): {error}"))?;
    if metadata.len() <= CACHE_HEADER_BYTES as u64 {
        return Err(format!("cache file {path} is empty"));
    }
    if metadata.len() > MAX_CACHE_BYTES {
        return Err(format!("cache file {path} is implausibly large"));
    }

    let bytes = std::fs::read(path).map_err(|error| format!("read({path}): {error}"))?;
    if &bytes[..8] != CACHE_MAGIC {
        return Err(format!("cache file {path} has a foreign magic header"));
    }
    let mut version_bytes = [0u8; 4];
    version_bytes.copy_from_slice(&bytes[8..12]);
    let abi = u32::from_le_bytes(version_bytes);
    if abi != ABI_VERSION {
        return Err(format!("cache file {path} was written for ABI {abi}, this build is {ABI_VERSION}"));
    }
    let mut count_bytes = [0u8; 8];
    count_bytes.copy_from_slice(&bytes[16..24]);
    let rule_count = u64::from_le_bytes(count_bytes);
    count_bytes.copy_from_slice(&bytes[24..32]);
    let list_bytes = u64::from_le_bytes(count_bytes);

    let mut engine = AdblockEngine::default();
    engine
        .deserialize(&bytes[CACHE_HEADER_BYTES..])
        .map_err(|error| format!("deserialize({path}): {error:?}"))?;
    Ok((engine, rule_count, list_bytes))
}

fn fill_stats(handle: &EngineHandle, out: &mut zephium_adblock_stats_t) {
    out.abi_version = ABI_VERSION;
    out.struct_size = std::mem::size_of::<zephium_adblock_stats_t>() as u32;
    out.rule_count = handle.rule_count;
    out.list_bytes = handle.list_bytes;
    out.checks = handle.stats.checks;
    out.blocked = handle.stats.blocked;
    out.redirected = handle.stats.redirected;
    out.rewritten = handle.stats.rewritten;
    out.panics = handle.stats.panics;
    out.last_check_ns = handle.stats.last_check_ns;
    out.total_check_ns = handle.stats.total_check_ns;
}

unsafe fn check_impl(
    handle: &mut EngineHandle,
    url: *const c_char,
    source_url: *const c_char,
    resource_type: u32,
    method: *const c_char,
    out: *mut zephium_adblock_result_t,
) -> u32 {
    if out.is_null() {
        set_last_error("check: out is NULL");
        return STATUS_ARGUMENT;
    }
    *out = null_result();

    if url.is_null() {
        set_last_error("check: url is NULL");
        return STATUS_ARGUMENT;
    }
    let url = read_cstr(url);
    if url.is_empty() {
        set_last_error("check: url is empty");
        return STATUS_ARGUMENT;
    }
    let source_url = read_cstr(source_url);
    let method = {
        let raw = read_cstr(method);
        if raw.is_empty() { "GET".to_string() } else { raw }
    };

    if handle.poisoned || handle.engine.is_none() {
        /* Still count the check so the shell's transparency log shows the
         * requests that were waved through after a failure. */
        handle.stats.checks = handle.stats.checks.saturating_add(1);
        set_last_error("check: engine unavailable (poisoned or empty), allowing request");
        return STATUS_OK;
    }

    let request_type = request_type_name(resource_type);
    let request = match Request::new(&url, &source_url, request_type, &method) {
        Ok(request) => request,
        Err(_) => {
            /* Unparseable/unsupported URL scheme: not something we can filter. */
            return STATUS_OK;
        }
    };

    let debug_rules = handle.debug_rules;
    let (blocked, important, redirect, rewritten, matched, elapsed_ns) = {
        let engine = match handle.engine.as_ref() {
            Some(engine) => engine,
            None => return STATUS_OK,
        };
        let started = Instant::now();
        let verdict = engine.check_network_request(&request);
        let elapsed_ns = started.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64;
        let matched = if debug_rules {
            verdict.filter.as_ref().map(|info| info.to_string())
        } else {
            None
        };
        (
            verdict.should_block(),
            verdict.important,
            verdict.redirect.clone(),
            verdict.rewritten_url.clone(),
            matched,
            elapsed_ns,
        )
    };

    let mut decision = VERDICT_ALLOW;
    if blocked {
        match redirect {
            Some(redirect) => {
                let (body, mime) = decode_redirect(&redirect);
                handle.redirect_mime = cstring(&mime);
                handle.redirect_body = body;
                decision = VERDICT_REDIRECT;
            }
            None => decision = VERDICT_BLOCK,
        }
    } else if let Some(rewritten_url) = rewritten {
        handle.rewritten = cstring(&rewritten_url);
        decision = VERDICT_REWRITE;
    }
    handle.matched = cstring(matched.as_deref().unwrap_or(""));

    handle.stats.checks = handle.stats.checks.saturating_add(1);
    handle.stats.last_check_ns = elapsed_ns;
    handle.stats.total_check_ns = handle.stats.total_check_ns.saturating_add(elapsed_ns);
    match decision {
        VERDICT_BLOCK => handle.stats.blocked = handle.stats.blocked.saturating_add(1),
        VERDICT_REDIRECT => handle.stats.redirected = handle.stats.redirected.saturating_add(1),
        VERDICT_REWRITE => handle.stats.rewritten = handle.stats.rewritten.saturating_add(1),
        _ => {}
    }

    let result = &mut *out;
    result.verdict = decision;
    result.important = if important { 1 } else { 0 };
    result.elapsed_ns = elapsed_ns;
    if decision == VERDICT_REDIRECT {
        result.redirect_body = handle.redirect_body.as_ptr() as *const c_char;
        result.redirect_len = handle.redirect_body.len();
        result.redirect_mime = handle.redirect_mime.as_ptr();
    }
    if decision == VERDICT_REWRITE {
        result.rewritten_url = handle.rewritten.as_ptr();
        result.rewritten_len = handle.rewritten.as_bytes().len();
    }
    result.matched_rule = handle.matched.as_ptr();
    STATUS_OK
}

unsafe fn cosmetic_impl(handle: &EngineHandle, url: *const c_char, out: *mut c_char, out_cap: usize) -> usize {
    if url.is_null() || handle.poisoned {
        return 0;
    }
    let engine = match handle.engine.as_ref() {
        Some(engine) => engine,
        None => return 0,
    };
    let url = read_cstr(url);
    if url.is_empty() {
        return 0;
    }

    let resources = engine.url_cosmetic_resources(&url);
    let mut selectors: Vec<String> = resources.hide_selectors.into_iter().collect();
    if selectors.is_empty() {
        return 0;
    }
    /* HashSet iteration order is random; sort so the injected CSS is stable and
     * diffable across runs. */
    selectors.sort();

    let mut css = String::with_capacity(selectors.iter().map(|s| s.len() + 1).sum::<usize>() + 32);
    for (index, selector) in selectors.iter().enumerate() {
        if index > 0 {
            css.push(',');
        }
        css.push_str(selector);
    }
    css.push_str("{display:none!important}");

    let bytes = css.as_bytes();
    let needed = bytes.len();
    if !out.is_null() && out_cap > 0 {
        let copy = needed.min(out_cap - 1);
        std::ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, out, copy);
        *out.add(copy) = 0;
    }
    needed
}

/* ------------------------------------------------------------------ public ABI */

#[no_mangle]
pub extern "C" fn zephium_adblock_abi_version() -> u32 {
    ABI_VERSION
}

#[no_mangle]
pub extern "C" fn zephium_adblock_version() -> *const c_char {
    VERSION_CSTR.as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn zephium_adblock_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_new(flags: u32) -> *mut zephium_adblock_engine {
    let handle = Box::new(EngineHandle {
        owner: thread::current().id(),
        debug_rules: flags & FLAG_DEBUG_RULES != 0,
        engine: None,
        poisoned: false,
        rule_count: 0,
        list_bytes: 0,
        redirect_body: Vec::new(),
        redirect_mime: cstring(""),
        rewritten: cstring(""),
        matched: cstring(""),
        stats: Stats::default(),
    });
    Box::into_raw(handle) as *mut zephium_adblock_engine
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_load(
    engine: *mut zephium_adblock_engine,
    list_texts: *const *const c_char,
    list_lens: *const usize,
    list_count: usize,
    out_rule_count: *mut u64,
) -> u32 {
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(status) => return status,
    };
    let texts = list_texts as usize;
    let lens = list_lens as usize;
    let out_count = out_rule_count as usize;

    catch_engine_call(handle, "engine_load", move || {
        let handle = unsafe { &mut *handle };
        if handle.poisoned {
            set_last_error("engine_load: handle is poisoned");
            return STATUS_STATE;
        }
        if list_count > 0 && texts == 0 {
            set_last_error("engine_load: list_texts is NULL");
            return STATUS_ARGUMENT;
        }

        let mut lists: Vec<String> = Vec::with_capacity(list_count.min(64));
        let mut total_bytes: usize = 0;
        for index in 0..list_count {
            let text_ptr = unsafe { *(texts as *const *const c_char).add(index) };
            if text_ptr.is_null() {
                continue;
            }
            let text = unsafe {
                if lens == 0 {
                    read_cstr(text_ptr)
                } else {
                    read_bytes(text_ptr, *(lens as *const usize).add(index))
                }
            };
            if text.is_empty() {
                continue;
            }
            total_bytes += text.len();
            if total_bytes > MAX_LIST_BYTES {
                set_last_error("engine_load: filter lists exceed the size limit");
                return STATUS_ARGUMENT;
            }
            lists.push(text);
        }
        if lists.is_empty() {
            set_last_error("engine_load: no filter list data");
            return STATUS_FILTERS;
        }

        let debug_rules = handle.debug_rules;
        let built = build_engine(&lists, debug_rules);
        let rule_count = count_rules(&lists);
        let list_bytes = total_bytes as u64;

        /* Only replace the working engine once the new one is fully built. */
        handle.engine = Some(built);
        handle.rule_count = rule_count;
        handle.list_bytes = list_bytes;
        handle.poisoned = false;
        if out_count != 0 {
            unsafe { *(out_count as *mut u64) = rule_count };
        }
        set_last_error("");
        STATUS_OK
    })
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_save_cache(
    engine: *mut zephium_adblock_engine,
    utf8_path: *const c_char,
) -> u32 {
    if utf8_path.is_null() {
        set_last_error("save_cache: path is NULL");
        return STATUS_ARGUMENT;
    }
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(status) => return status,
    };
    let path_ptr = utf8_path as usize;

    catch_engine_call(handle, "save_cache", move || {
        let handle = unsafe { &*handle };
        if handle.poisoned {
            set_last_error("save_cache: handle is poisoned");
            return STATUS_STATE;
        }
        let engine = match handle.engine.as_ref() {
            Some(engine) => engine,
            None => {
                set_last_error("save_cache: no engine loaded");
                return STATUS_STATE;
            }
        };
        let path = unsafe { read_cstr(path_ptr as *const c_char) };
        if path.is_empty() {
            set_last_error("save_cache: path is empty");
            return STATUS_ARGUMENT;
        }
        match save_cache_file(engine, handle.rule_count, handle.list_bytes, &path) {
            Ok(()) => {
                set_last_error("");
                STATUS_OK
            }
            Err(message) => {
                set_last_error(&format!("save_cache: {message}"));
                STATUS_CACHE
            }
        }
    })
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_load_cache(
    engine: *mut zephium_adblock_engine,
    utf8_path: *const c_char,
) -> u32 {
    if utf8_path.is_null() {
        set_last_error("load_cache: path is NULL");
        return STATUS_ARGUMENT;
    }
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(status) => return status,
    };
    let path_ptr = utf8_path as usize;

    catch_engine_call(handle, "load_cache", move || {
        let handle = unsafe { &mut *handle };
        if handle.poisoned {
            set_last_error("load_cache: handle is poisoned");
            return STATUS_STATE;
        }
        let path = unsafe { read_cstr(path_ptr as *const c_char) };
        if path.is_empty() {
            set_last_error("load_cache: path is empty");
            return STATUS_ARGUMENT;
        }
        match load_cache_file(&path) {
            Ok((loaded, rule_count, list_bytes)) => {
                handle.engine = Some(loaded);
                handle.rule_count = rule_count;
                handle.list_bytes = list_bytes;
                set_last_error("");
                STATUS_OK
            }
            Err(message) => {
                set_last_error(&format!("load_cache: {message}"));
                STATUS_CACHE
            }
        }
    })
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_stats(
    engine: *mut zephium_adblock_engine,
    out: *mut zephium_adblock_stats_t,
) -> u32 {
    if out.is_null() {
        set_last_error("engine_stats: out is NULL");
        return STATUS_ARGUMENT;
    }
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(status) => return status,
    };
    let out_ptr = out as usize;
    catch_engine_call(handle, "engine_stats", move || {
        let handle = unsafe { &*handle };
        let out = unsafe { &mut *(out_ptr as *mut zephium_adblock_stats_t) };
        fill_stats(handle, out);
        STATUS_OK
    })
}

#[no_mangle]
pub extern "C" fn zephium_adblock_check(
    engine: *mut zephium_adblock_engine,
    url: *const c_char,
    source_url: *const c_char,
    resource_type: u32,
    method: *const c_char,
    out: *mut zephium_adblock_result_t,
) -> u32 {
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(status) => {
            /* Fail open: a NULL/misused handle must not block traffic. */
            if !out.is_null() {
                unsafe { *out = null_result() };
            }
            return status;
        }
    };
    let url_ptr = url as usize;
    let source_ptr = source_url as usize;
    let method_ptr = method as usize;
    let out_ptr = out as usize;

    catch_engine_call(handle, "check", move || unsafe {
        check_impl(
            &mut *handle,
            url_ptr as *const c_char,
            source_ptr as *const c_char,
            resource_type,
            method_ptr as *const c_char,
            out_ptr as *mut zephium_adblock_result_t,
        )
    })
}

#[no_mangle]
pub extern "C" fn zephium_adblock_cosmetic_css(
    engine: *mut zephium_adblock_engine,
    url: *const c_char,
    out: *mut c_char,
    out_cap: usize,
) -> usize {
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(_) => return 0,
    };
    let url_ptr = url as usize;
    let out_ptr = out as usize;
    let result = catch_unwind(AssertUnwindSafe(move || unsafe {
        cosmetic_impl(&*handle, url_ptr as *const c_char, out_ptr as *mut c_char, out_cap)
    }));
    match result {
        Ok(written) => written,
        Err(_) => {
            unsafe {
                (*handle).poisoned = true;
            }
            set_last_error("cosmetic_css: contained Rust panic; element hiding disabled");
            0
        }
    }
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_free(engine: *mut zephium_adblock_engine) {
    if engine.is_null() {
        return;
    }
    let handle = engine as *mut EngineHandle;
    unsafe {
        if (*handle).owner != thread::current().id() {
            /* Freeing the engine (and its non-Send internals) from a foreign
             * thread would be undefined behaviour; leaking one handle is the
             * safe failure mode. */
            set_last_error("engine_free: called from a non-owning thread; leaking the handle");
            return;
        }
    }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        drop(Box::from_raw(handle));
    }));
}

#[no_mangle]
pub extern "C" fn zephium_adblock_selftest_panic() -> u32 {
    let contained = catch_unwind(AssertUnwindSafe(|| -> u32 {
        panic!("zephium adblock panic containment self test");
    }));
    match contained {
        Ok(status) => status,
        Err(payload) => {
            set_last_error(&format!(
                "selftest_panic: contained: {}",
                panic_text(payload.as_ref())
            ));
            STATUS_PANIC
        }
    }
}

#[no_mangle]
pub extern "C" fn zephium_adblock_engine_selftest_panic(engine: *mut zephium_adblock_engine) -> u32 {
    let handle = match unsafe { acquire(engine) } {
        Ok(handle) => handle,
        Err(status) => return status,
    };
    catch_engine_call(handle, "engine_selftest_panic", || -> u32 {
        panic!("zephium adblock engine panic containment self test");
    })
}

/* --------------------------------------------------------------------- tests */

#[cfg(test)]
mod tests {
    use super::*;

    const TEST_LISTS: &[&str] = &[
        "! Title: zephium test list\n\
         [Adblock Plus 2.0]\n\
         ||doubleclick.net^\n\
         ||ads.example^$script\n\
         @@||good.ads.example^\n\
         ||tracker.example^$image\n\
         ||redirect.example/ads.js$script,redirect=1x1-transparent.gif\n\
         example.com##.ad-banner\n\
         example.com##.sponsor-block\n\
         ##.generic-ad\n",
    ];

    fn last_error() -> String {
        unsafe { read_cstr(zephium_adblock_last_error()) }
    }

    struct Fixture {
        handle: *mut zephium_adblock_engine,
        _lists: Vec<CString>,
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            zephium_adblock_engine_free(self.handle);
        }
    }

    fn fixture(flags: u32) -> Fixture {
        let handle = zephium_adblock_engine_new(flags);
        assert!(!handle.is_null());
        let lists: Vec<CString> = TEST_LISTS.iter().map(|list| CString::new(*list).unwrap()).collect();
        let pointers: Vec<*const c_char> = lists.iter().map(|list| list.as_ptr()).collect();
        let lengths: Vec<usize> = TEST_LISTS.iter().map(|list| list.len()).collect();
        let mut rule_count: u64 = 0;
        let status = zephium_adblock_engine_load(
            handle,
            pointers.as_ptr(),
            lengths.as_ptr(),
            pointers.len(),
            &mut rule_count,
        );
        assert_eq!(status, STATUS_OK, "load failed: {}", last_error());
        assert!(rule_count > 0, "expected some rules, got {rule_count}");
        Fixture { handle, _lists: lists }
    }

    fn check(handle: *mut zephium_adblock_engine, url: &str, source: &str, resource_type: u32, method: &str) -> zephium_adblock_result_t {
        let url_c = CString::new(url).unwrap();
        let source_c = CString::new(source).unwrap();
        let method_c = CString::new(method).unwrap();
        let mut out = null_result();
        let status = zephium_adblock_check(
            handle,
            url_c.as_ptr(),
            source_c.as_ptr(),
            resource_type,
            method_c.as_ptr(),
            &mut out,
        );
        assert_eq!(status, STATUS_OK, "check failed: {}", last_error());
        out
    }

    fn stats(handle: *mut zephium_adblock_engine) -> zephium_adblock_stats_t {
        let mut out = zephium_adblock_stats_t {
            abi_version: 0,
            struct_size: 0,
            rule_count: 0,
            list_bytes: 0,
            checks: 0,
            blocked: 0,
            redirected: 0,
            rewritten: 0,
            panics: 0,
            last_check_ns: 0,
            total_check_ns: 0,
        };
        let status = zephium_adblock_engine_stats(handle, &mut out);
        assert_eq!(status, STATUS_OK, "stats failed: {}", last_error());
        assert_eq!(out.abi_version, ABI_VERSION);
        assert_eq!(out.struct_size, std::mem::size_of::<zephium_adblock_stats_t>() as u32);
        out
    }

    #[test]
    fn blocks_and_allows_like_the_rules_say() {
        let fixture = fixture(0);
        assert_eq!(check(fixture.handle, "https://doubleclick.net/pixel", "", 3, "GET").verdict, VERDICT_BLOCK);
        assert_eq!(check(fixture.handle, "https://sub.doubleclick.net/x.js", "", 3, "GET").verdict, VERDICT_BLOCK);
        assert_eq!(check(fixture.handle, "https://example.com/", "", 1, "GET").verdict, VERDICT_ALLOW);
        /* $script only: the same host as an image is not blocked. */
        assert_eq!(check(fixture.handle, "https://ads.example/banner.png", "", 5, "GET").verdict, VERDICT_ALLOW);
        assert_eq!(check(fixture.handle, "https://ads.example/a.js", "", 3, "GET").verdict, VERDICT_BLOCK);
        /* Exception rule. */
        assert_eq!(check(fixture.handle, "https://good.ads.example/a.js", "", 3, "GET").verdict, VERDICT_ALLOW);
        /* $image rule. */
        assert_eq!(check(fixture.handle, "https://tracker.example/t.gif", "", 5, "GET").verdict, VERDICT_BLOCK);
        assert_eq!(stats(fixture.handle).blocked >= 4, true);
    }

    #[test]
    fn redirect_rules_produce_real_bytes() {
        let fixture = fixture(0);
        let out = check(fixture.handle, "https://redirect.example/ads.js", "https://example.com/", 3, "GET");
        assert_eq!(out.verdict, VERDICT_REDIRECT, "expected a $redirect verdict");
        assert!(out.redirect_len > 0);
        let body = unsafe { std::slice::from_raw_parts(out.redirect_body as *const u8, out.redirect_len) };
        assert!(body.starts_with(b"GIF89a"), "decoded redirect body should be a GIF, got {:?}", &body[..body.len().min(8)]);
        let mime = unsafe { read_cstr(out.redirect_mime) };
        assert_eq!(mime, "image/gif");
        assert_eq!(stats(fixture.handle).redirected, 1);
    }

    #[test]
    fn unparseable_url_never_blocks() {
        let fixture = fixture(0);
        let out = check(fixture.handle, "not a url at all", "", 0, "");
        assert_eq!(out.verdict, VERDICT_ALLOW);
        let mut out = null_result();
        let status = zephium_adblock_check(fixture.handle, std::ptr::null(), std::ptr::null(), 0, std::ptr::null(), &mut out);
        assert_eq!(status, STATUS_ARGUMENT);
        assert_eq!(out.verdict, VERDICT_ALLOW);
    }

    #[test]
    fn cache_roundtrip_keeps_blocking() {
        let fixture = fixture(0);
        /* Keep the cache inside the checkout: the verification harness that builds
         * this repository allows writes only under the workspace, and a test that
         * depends on the system temp directory would fail for the wrong reason. */
        let directory = std::path::Path::new("target").join("test-tmp");
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join(format!("cache-{}.bin", std::process::id()));
        let path_text = path.to_string_lossy().into_owned();
        let path_c = CString::new(path_text.clone()).unwrap();
        let saved = zephium_adblock_engine_save_cache(fixture.handle, path_c.as_ptr());
        assert_eq!(saved, STATUS_OK, "save_cache failed: {}", last_error());
        assert!(path.exists());

        let reloaded = zephium_adblock_engine_new(0);
        assert!(!reloaded.is_null());
        let status = zephium_adblock_engine_load_cache(reloaded, path_c.as_ptr());
        assert_eq!(status, STATUS_OK, "load_cache failed: {}", last_error());
        assert_eq!(check(reloaded, "https://doubleclick.net/x", "", 3, "GET").verdict, VERDICT_BLOCK);
        let out = check(reloaded, "https://example.com/", "", 1, "GET");
        assert_eq!(out.verdict, VERDICT_ALLOW);
        /* Stats survive the cache header. */
        assert!(stats(reloaded).rule_count > 0);
        zephium_adblock_engine_free(reloaded);

        /* A corrupted cache must be rejected, not crash. */
        std::fs::write(&path, b"not a cache file at all").unwrap();
        let broken = zephium_adblock_engine_new(0);
        let status = zephium_adblock_engine_load_cache(broken, path_c.as_ptr());
        assert_eq!(status, STATUS_CACHE);
        assert_eq!(check(broken, "https://doubleclick.net/x", "", 3, "GET").verdict, VERDICT_ALLOW);
        zephium_adblock_engine_free(broken);
        let _ = std::fs::remove_file(&path);
        let _ = std::fs::remove_dir(&directory);
    }

    #[test]
    fn thread_affinity_is_enforced() {
        let fixture = fixture(0);
        let address = fixture.handle as usize;
        let other_thread_status = thread::spawn(move || {
            let handle = address as *mut zephium_adblock_engine;
            let url = CString::new("https://doubleclick.net/x").unwrap();
            let mut out = null_result();
            let status = zephium_adblock_check(handle, url.as_ptr(), std::ptr::null(), 0, std::ptr::null(), &mut out);
            (status, out.verdict)
        })
        .join()
        .unwrap();
        assert_eq!(other_thread_status.0, STATUS_THREAD);
        assert_eq!(other_thread_status.1, VERDICT_ALLOW, "misuse must fail open");
        /* The owning thread still has a working engine. */
        assert_eq!(check(fixture.handle, "https://doubleclick.net/x", "", 0, "GET").verdict, VERDICT_BLOCK);
    }

    #[test]
    fn contained_panic_poisons_and_fails_open() {
        assert_eq!(zephium_adblock_selftest_panic(), STATUS_PANIC);
        let fixture = fixture(0);
        assert_eq!(check(fixture.handle, "https://doubleclick.net/x", "", 3, "GET").verdict, VERDICT_BLOCK);
        assert_eq!(zephium_adblock_engine_selftest_panic(fixture.handle), STATUS_PANIC);
        assert_eq!(stats(fixture.handle).panics, 1);
        assert_eq!(
            check(fixture.handle, "https://doubleclick.net/x", "", 3, "GET").verdict,
            VERDICT_ALLOW,
            "a poisoned engine must allow everything instead of crashing"
        );
        /* The handle is still usable (stats/free) after the contained panic, and
         * the fail-open check above was counted. */
        let after = stats(fixture.handle);
        assert_eq!(after.checks, 2);
        assert_eq!(after.blocked, 1);
    }

    #[test]
    fn cosmetic_css_is_sorted_and_bounded() {
        let fixture = fixture(0);
        let url = CString::new("https://example.com/article").unwrap();
        let mut buffer = Vec::<u8>::with_capacity(256);
        let needed = zephium_adblock_cosmetic_css(fixture.handle, url.as_ptr(), buffer.as_mut_ptr() as *mut c_char, buffer.capacity());
        assert!(needed > 0, "expected cosmetic selectors");
        let mut filled = vec![0u8; needed + 1];
        let needed_again = zephium_adblock_cosmetic_css(fixture.handle, url.as_ptr(), filled.as_mut_ptr() as *mut c_char, filled.len());
        assert_eq!(needed, needed_again);
        let css = String::from_utf8_lossy(&filled[..needed]).into_owned();
        assert!(css.ends_with("{display:none!important}"));
        assert!(css.contains(".ad-banner"));
        assert!(css.contains(".sponsor-block"));
        /* Deterministic order: .ad-banner sorts before .sponsor-block. */
        assert!(css.find(".ad-banner").unwrap() < css.find(".sponsor-block").unwrap());
        /* Nothing to hide on a URL with no matching cosmetic rules -> 0. */
        let other = CString::new("https://nothing.example/").unwrap();
        let mut small = [0i8; 8];
        assert_eq!(zephium_adblock_cosmetic_css(fixture.handle, other.as_ptr(), small.as_mut_ptr(), small.len()), 0);
    }

    #[test]
    fn debug_flag_reports_matched_rule() {
        let fixture = fixture(FLAG_DEBUG_RULES);
        let out = check(fixture.handle, "https://doubleclick.net/x", "", 3, "GET");
        assert_eq!(out.verdict, VERDICT_BLOCK);
        let matched = unsafe { read_cstr(out.matched_rule) };
        assert!(matched.contains("doubleclick.net"), "matched rule was '{matched}'");
    }
}
