/* =============================================================================
 * Zephium ad blocking - C interface to the adblock-rust engine
 *
 * The Windows shell (WebView2) uses this interface from its WebResourceRequested
 * handler: every network request is checked against a compiled adblock-rust
 * engine before Chromium sends it. The engine itself lives in the Rust crate in
 * blocker/rust (built as a static library) and is linked into zephium_win.exe -
 * no extension, no content script and no hook visible to page JavaScript is
 * involved.
 *
 * Threading model
 *   adblock-rust is built with its "single-thread" feature (the crate default),
 *   which is the fastest mode but deliberately makes the engine NOT Send/Sync.
 *   To keep that sound without locks on the hot path, every engine handle is
 *   *thread affine*: the thread that created it is the only thread allowed to
 *   use or free it. Every entry point verifies this and fails with
 *   ZEPHIUM_ADBLOCK_ERR_THREAD instead of touching the engine from a foreign
 *   thread. Create the engine on the thread that performs the request checks
 *   (the WebView2 UI thread) and never move the handle across threads.
 *
 * Failure model
 *   Ad blocking is an optimisation, never a correctness requirement: on any
 *   internal failure (bad rule set, unreadable cache, caught panic) the affected
 *   call returns a non-zero status and leaves the browser working. A caught
 *   panic poisons the handle, after which every check answers ALLOW - the browser
 *   keeps loading pages instead of dying with the 0xc0000005 access violation
 *   that unwinding a Rust panic across the FFI boundary used to produce.
 * =============================================================================
 */
#ifndef ZEPHIUM_ADBLOCK_FFI_H
#define ZEPHIUM_ADBLOCK_FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever the layout of a struct in this header changes. */
#define ZEPHIUM_ADBLOCK_ABI_VERSION 1u

typedef enum {
    ZEPHIUM_ADBLOCK_OK = 0,
    /* An argument was NULL, empty where it must not be, or too large. */
    ZEPHIUM_ADBLOCK_ERR_ARGUMENT = 1,
    /* The handle has no engine (nothing loaded yet). Checks answer ALLOW. */
    ZEPHIUM_ADBLOCK_ERR_STATE = 2,
    /* A Rust panic was caught. The handle is poisoned; checks answer ALLOW. */
    ZEPHIUM_ADBLOCK_ERR_PANIC = 3,
    /* The filter lists could not be parsed into an engine. */
    ZEPHIUM_ADBLOCK_ERR_FILTERS = 4,
    /* The binary engine cache could not be read or written. */
    ZEPHIUM_ADBLOCK_ERR_CACHE = 5,
    /* The handle was used from a thread other than the one that created it. */
    ZEPHIUM_ADBLOCK_ERR_THREAD = 6
} zephium_adblock_status_t;

/* What the caller should do with the request. */
typedef enum {
    /* Send the request unchanged. */
    ZEPHIUM_ADBLOCK_ALLOW = 0,
    /* Drop the request (empty response, no network traffic). */
    ZEPHIUM_ADBLOCK_BLOCK = 1,
    /* Serve redirect_body/redirect_mime instead of the network request. */
    ZEPHIUM_ADBLOCK_REDIRECT = 2,
    /* A $removeparam rule rewrote the URL; the caller may use rewritten_url.
     * WebView2 cannot replace a request URL from WebResourceRequested, so the
     * Windows shell counts and logs this and lets the request through. */
    ZEPHIUM_ADBLOCK_REWRITE = 3
} zephium_adblock_verdict_t;

/* Request type, mirroring the strings adblock-rust understands. */
typedef enum {
    ZEPHIUM_ADBLOCK_RES_OTHER = 0,
    ZEPHIUM_ADBLOCK_RES_DOCUMENT,
    ZEPHIUM_ADBLOCK_RES_SUBDOCUMENT,
    ZEPHIUM_ADBLOCK_RES_SCRIPT,
    ZEPHIUM_ADBLOCK_RES_STYLESHEET,
    ZEPHIUM_ADBLOCK_RES_IMAGE,
    ZEPHIUM_ADBLOCK_RES_FONT,
    ZEPHIUM_ADBLOCK_RES_MEDIA,
    ZEPHIUM_ADBLOCK_RES_XHR,
    ZEPHIUM_ADBLOCK_RES_PING,
    /* adblock-rust can filter WebSocket handshakes, but WebView2 never raises
     * WebResourceRequested for them; see windows/README.md. */
    ZEPHIUM_ADBLOCK_RES_WEBSOCKET,
    ZEPHIUM_ADBLOCK_RES_MANIFEST,
    ZEPHIUM_ADBLOCK_RES_OBJECT
} zephium_adblock_resource_type_t;

/* Opaque engine handle. */
typedef struct zephium_adblock_engine zephium_adblock_engine;

/* Flags for zephium_adblock_engine_new(). */
#define ZEPHIUM_ADBLOCK_FLAG_DEBUG_RULES 0x1u /* keep matched rule text (costs memory) */

/* Result of zephium_adblock_check(). Every pointer is owned by the engine handle
 * and stays valid until the next call on that handle (or until it is freed). */
typedef struct {
    zephium_adblock_verdict_t verdict;
    /* Non-zero when an $important rule matched. */
    uint32_t important;
    /* Decoded replacement body for ZEPHIUM_ADBLOCK_REDIRECT (may contain NULs). */
    const char *redirect_body;
    size_t redirect_len;
    /* MIME type of redirect_body, e.g. "image/gif" or "text/javascript". */
    const char *redirect_mime;
    /* Replacement URL for ZEPHIUM_ADBLOCK_REWRITE. */
    const char *rewritten_url;
    size_t rewritten_len;
    /* Matched rule text ("source:line: rule"), only with DEBUG_RULES. */
    const char *matched_rule;
    /* Wall time spent inside the engine for this check, in nanoseconds. */
    uint64_t elapsed_ns;
} zephium_adblock_result_t;

/* Counters for the transparency/log requirement. */
typedef struct {
    uint32_t abi_version;   /* filled by the library: ZEPHIUM_ADBLOCK_ABI_VERSION */
    uint32_t struct_size;   /* filled by the library: sizeof(zephium_adblock_stats_t) */
    uint64_t rule_count;    /* network rules loaded into the engine */
    uint64_t list_bytes;    /* raw filter list bytes that produced them */
    uint64_t checks;
    uint64_t blocked;
    uint64_t redirected;
    uint64_t rewritten;
    uint64_t panics;
    uint64_t last_check_ns;
    uint64_t total_check_ns;
} zephium_adblock_stats_t;

/* Engine ABI/version introspection. */
uint32_t zephium_adblock_abi_version(void);
/* Static string such as "zephium-adblock-ffi 1.0.0 (adblock-rust 0.13)". Never freed. */
const char *zephium_adblock_version(void);
/* Last error recorded on this thread, "" if none. Never freed. */
const char *zephium_adblock_last_error(void);

/*
 * Creates a thread-affine engine handle. Returns NULL only when allocation
 * fails; a handle without rules answers ALLOW.
 */
zephium_adblock_engine *zephium_adblock_engine_new(uint32_t flags);

/*
 * Builds the engine from raw ABP/uBO filter lists (EasyList, EasyPrivacy, uBlock
 * Origin assets, ...). list_texts[i] holds list_lens[i] bytes (need not be NUL
 * terminated); pass list_lens = NULL to treat the texts as NUL-terminated
 * strings. Lists are parsed in Standard format, malformed rules are ignored, and
 * the result replaces any previously loaded rule set. On failure the previous
 * rule set is kept, so a daily filter update that goes wrong cannot take a
 * working blocker offline.
 *
 * out_rule_count (optional) receives the number of network rules.
 */
zephium_adblock_status_t zephium_adblock_engine_load(zephium_adblock_engine *engine,
                                                     const char *const *list_texts,
                                                     const size_t *list_lens,
                                                     size_t list_count,
                                                     uint64_t *out_rule_count);

/*
 * Serialized-engine cache: a binary dump of the compiled engine that reloads in
 * a few milliseconds instead of re-parsing megabytes of filter lists.
 *
 * The path is UTF-8 (on Windows it is converted to the native wide path). Writes
 * are atomic: the file is written next to the target and renamed into place, so
 * a crash or a concurrent reader never sees a half-written cache. Cache files are
 * only valid for the exact library build that wrote them; deserialize failures
 * are reported as ZEPHIUM_ADBLOCK_ERR_CACHE and callers fall back to a rebuild.
 */
zephium_adblock_status_t zephium_adblock_engine_save_cache(zephium_adblock_engine *engine,
                                                           const char *utf8_path);
zephium_adblock_status_t zephium_adblock_engine_load_cache(zephium_adblock_engine *engine,
                                                           const char *utf8_path);

/* Copies counters into *out. */
zephium_adblock_status_t zephium_adblock_engine_stats(zephium_adblock_engine *engine,
                                                      zephium_adblock_stats_t *out);

/*
 * Asks the engine what to do with one request. url and source_url are absolute
 * URLs (source_url may be "" when the initiator is unknown; method defaults to
 * GET when NULL/empty). *out is always fully initialised, even on error, with
 * verdict ALLOW so a failed check never blocks a page.
 */
zephium_adblock_status_t zephium_adblock_check(zephium_adblock_engine *engine,
                                               const char *url,
                                               const char *source_url,
                                               zephium_adblock_resource_type_t type,
                                               const char *method,
                                               zephium_adblock_result_t *out);

/*
 * Cosmetic filtering (element hiding) for a document URL: writes a CSS rule
 * ("selector,selector{display:none!important}") into out. Returns the number of
 * bytes that would be written, excluding the NUL (0 when there is nothing to
 * hide, or on any error), snprintf-style. Unlike network blocking this has to be
 * injected into the page, so it is only used when a shell opts in; the WebKitGTK
 * shell gets its element hiding from the content filter JSON instead.
 */
size_t zephium_adblock_cosmetic_css(zephium_adblock_engine *engine,
                                    const char *url,
                                    char *out,
                                    size_t out_cap);

/* Frees the handle; safe to call with NULL. Must run on the owning thread. */
void zephium_adblock_engine_free(zephium_adblock_engine *engine);

/* ---- test-only hooks, used by blocker/rust unit tests and windows/adblock_selftest.cpp
 * to prove the panic containment keeps the browser alive. Not for production use. */
uint32_t zephium_adblock_selftest_panic(void);
uint32_t zephium_adblock_engine_selftest_panic(zephium_adblock_engine *engine);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_ADBLOCK_FFI_H */
