/* =============================================================================
 * Zephium ad blocking - request classification helpers + engine lifecycle
 *
 * Shared by the Win32 shell (zephium_win.cpp) and by the console self test
 * (adblock_selftest.cpp).  The classification helpers are deliberately free of
 * WebView2/COM types: they are pure string functions, so the self test can prove
 * the main-frame/iframe and source-URL rules without starting a browser.  The
 * shell maps COREWEBVIEW2_WEB_RESOURCE_CONTEXT_* onto ZEPHIUM_W2_CONTEXT_* and
 * static_asserts that the numeric values still line up with the SDK.
 *
 * The engine itself is the frozen C ABI in blocker/adblock_ffi.h (adblock-rust
 * staticlib).  Handles are thread affine and every check must run on the UI
 * thread; the functions below keep that contract and never let a failure turn
 * into "no browsing": no engine means ALLOW.
 * ========================================================================== */
#ifndef ZEPHIUM_ADBLOCK_WEBVIEW2_H
#define ZEPHIUM_ADBLOCK_WEBVIEW2_H

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../blocker/adblock_ffi.h"

/* Numeric mirror of COREWEBVIEW2_WEB_RESOURCE_CONTEXT_* (WebView2 SDK
 * 1.0.4370.26).  The values are part of the WebView2 ABI and change only with a
 * new SDK; zephium_win.cpp static_asserts them against the real enum. */
enum {
    ZEPHIUM_W2_CONTEXT_ALL = 0,
    ZEPHIUM_W2_CONTEXT_DOCUMENT = 1,
    ZEPHIUM_W2_CONTEXT_STYLESHEET = 2,
    ZEPHIUM_W2_CONTEXT_IMAGE = 3,
    ZEPHIUM_W2_CONTEXT_MEDIA = 4,
    ZEPHIUM_W2_CONTEXT_FONT = 5,
    ZEPHIUM_W2_CONTEXT_SCRIPT = 6,
    ZEPHIUM_W2_CONTEXT_XMLHTTPREQUEST = 7,
    ZEPHIUM_W2_CONTEXT_FETCH = 8,
    ZEPHIUM_W2_CONTEXT_TEXTTRACK = 9,
    ZEPHIUM_W2_CONTEXT_EVENTSOURCE = 10,
    ZEPHIUM_W2_CONTEXT_WEBSOCKET = 11,
    ZEPHIUM_W2_CONTEXT_MANIFEST = 12,
    ZEPHIUM_W2_CONTEXT_SIGNEDEXCHANGE = 13,
    ZEPHIUM_W2_CONTEXT_PING = 14,
    ZEPHIUM_W2_CONTEXT_CSP_VIOLATION_REPORT = 15,
    ZEPHIUM_W2_CONTEXT_OTHER = 16
};

/* Posted by the filter-list download thread when it has finished; wParam is the
 * number of list files that changed on disk (0 = nothing to rebuild). */
#define ZEPHIUM_ADBLOCK_WM_REFRESH (WM_APP + 0x51)

/* Downloads are capped per list so a hijacked/answered-too-much URL cannot eat
 * memory.  The default set is ~10 MB; 32 MB total leaves room for all optional
 * lists with a wide margin. */
#define ZEPHIUM_ADBLOCK_MAX_TOTAL_LIST_BYTES (32u * 1024u * 1024u)

/* Maps a WebView2 resource context to the engine's request type.  A DOCUMENT
 * context still maps to DOCUMENT here; zephium_adblock_classify() is the entry
 * point that can turn an iframe document into SUBDOCUMENT. */
zephium_adblock_resource_type_t zephium_adblock_map_context(int context);

/* Stable name for logs ("document", "script", ...); never NULL. */
const char *zephium_adblock_type_name(zephium_adblock_resource_type_t type);

/* Removes the fragment from a URL.  NavigationStarting reports the URL with its
 * fragment while the HTTP request that follows does not carry it, so iframe
 * tracking compares fragment-free URLs. */
std::string zephium_adblock_normalize_url(const std::string &url);

/*
 * Per-tab iframe tracker.  WebView2 reports navigations inside iframes as
 * COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT too, so the shell remembers the
 * URLs of iframe navigations: a DOCUMENT request whose URL is in this set is a
 * real subdocument, everything else DOCUMENT is the top-level document.
 *
 * Bounded on purpose: a hostile page can create iframes in a loop, so only the
 * most recent kMaxTrackedFrames entries are kept (FIFO).  A missed iframe then
 * degrades to "treated as a document", never to a crash or unbounded memory.
 */
class ZephiumAdblockFrameTracker {
public:
    static const size_t kMaxTrackedFrames = 64;

    /* Main-frame navigation started: the previous document's iframes are gone. */
    void BeginDocument(const std::string &url);
    /* SourceChanged: the committed top-level URL changed (history, SPA). */
    void SetDocumentUrl(const std::string &url);
    const std::string &document_url() const { return m_document_url; }

    /* An iframe started navigating to this URL. */
    void FrameNavigating(const std::string &url);
    bool IsIframeNavigation(const std::string &url) const;
    size_t tracked_frames() const { return m_frames.size(); }
    void Clear();

private:
    std::string m_document_url;
    std::vector<std::string> m_frames;
};

/* Context -> request type, including the document/subdocument decision. */
zephium_adblock_resource_type_t zephium_adblock_classify(
    int context, const std::string &url, const ZephiumAdblockFrameTracker &tracker);

/*
 * Source (initiator) URL for a request:
 *   - document requests use their own URL;
 *   - everything else uses the Referer request header when present;
 *   - otherwise the tab's current document URL;
 *   - otherwise "".
 * Candidates that are not absolute http(s) URLs are dropped because the engine
 * cannot parse them; a bad source must not make the whole check fail open/closed
 * unpredictably.
 */
std::string zephium_adblock_source_url(zephium_adblock_resource_type_t type,
                                        const std::string &request_url,
                                        const std::string &referer,
                                        const std::string &document_url);

/* UTF-8 <-> UTF-16 conversions (COM strings are wide, the engine is UTF-8). */
std::string zephium_adblock_utf8(const std::wstring &wide);
std::wstring zephium_adblock_wide(const std::string &utf8);

/*
 * Ad-block data directory: $ZEPHIUM_ADBLOCK_DIR when set, otherwise
 * %LOCALAPPDATA%\Zephium\adblock (the exe directory may be read-only, e.g. under
 * Program Files).  Filter lists live in <root>\lists\, the compiled engine cache
 * in <root>\engine.bin.
 */
std::wstring zephium_adblock_root_dir(const std::wstring &exe_dir);

/* One entry of the built-in filter-list catalog.  Raw ABP/uBO lists, not JSON. */
struct ZephiumAdblockList {
    const char *id;           /* stable id, used in logs and the local manifest */
    const wchar_t *filename;  /* file name inside <root>\lists\ */
    const char *url;          /* https source, see windows/README.md */
    size_t max_bytes;         /* per-list download cap */
    bool enabled_by_default;  /* the default set is EasyList + EasyPrivacy + uBO */
};

/* Returns the catalog (count written to *count when non-NULL). */
const ZephiumAdblockList *zephium_adblock_lists(size_t *count);

/* Downloads one filter list into memory; used by the refresh thread and by the
 * self test's "real lists" benchmark.  On failure returns false and fills
 * *error with a short reason. */
bool zephium_adblock_download_list(const ZephiumAdblockList &list,
                                   std::vector<unsigned char> *out,
                                   std::wstring *error);

/* Shell startup options. */
struct ZephiumAdblockConfig {
    HWND hwnd = nullptr;          /* receives ZEPHIUM_ADBLOCK_WM_REFRESH */
    std::wstring exe_dir;         /* directory of zephium_win.exe */
    bool enabled = true;          /* --no-adblock / ZEPHIUM_ADBLOCK=0 turn this off */
    void (*logger)(const wchar_t *line) = nullptr;  /* shell log sink, may be NULL */
};

/* Creates the thread-affine engine on the calling (UI) thread and tries the
 * binary cache first, then the cached lists, then a background download.
 * Returns false only when ad blocking is off or unusable; the shell keeps
 * browsing either way.  Every later call below must run on the same thread. */
bool zephium_adblock_start(const ZephiumAdblockConfig &config);

/* Called from the shell's WM_TIMER: kicks the 24h refresh and emits the periodic
 * stats line (only when the counters changed). */
void zephium_adblock_tick();

/* Called from the shell window procedure for ZEPHIUM_ADBLOCK_WM_REFRESH. */
void zephium_adblock_on_build_message(WPARAM changed_lists);

/* Stops the download thread, logs the final stats and frees the engine. */
void zephium_adblock_shutdown();

/* True while the feature is on (even when the engine is still being downloaded). */
bool zephium_adblock_is_enabled();
/* True when a loaded engine can answer checks. */
bool zephium_adblock_is_ready();

/*
 * Runs one request through the engine.  UI thread only, never throws, and always
 * initialises *out to an ALLOW verdict so a failed check lets the request
 * through.  Returns the FFI status (ZEPHIUM_ADBLOCK_OK when *out is meaningful).
 */
zephium_adblock_status_t zephium_adblock_check_url(
    const std::string &url, const std::string &source_url,
    zephium_adblock_resource_type_t type, const std::string &method,
    zephium_adblock_result_t *out);

#endif /* ZEPHIUM_ADBLOCK_WEBVIEW2_H */
