/* =============================================================================
 * Zephium ad blocking - console self test (no window, no WebView2, no COM)
 *
 * Exercises the pieces the Win32 shell relies on:
 *   - the pure request-classification helpers shared with the shell
 *     (url -> resource type, main frame vs iframe, source URL)
 *   - the frozen C ABI in blocker/adblock_ffi.h: block/allow/redirect/rewrite
 *     decisions against a synthetic filter list, engine cache save/load
 *     roundtrip and timing, thread-affinity rejection, panic containment
 *   - a benchmark with ~140k synthetic rules plus the real EasyList/EasyPrivacy
 *     downloads when the network is available, asserting p99 < 100 us/check
 *
 * Build/run: mingw32-make -C windows adblock-test
 * The test writes only inside ZEPHIUM_ADBLOCK_DIR when set (the Makefile points
 * it at windows\obj\adblock-selftest), otherwise into obj\adblock-selftest
 * relative to the current directory - never into the production data directory.
 * ========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "adblock_webview2.h"

/* --------------------------------------------------------------- reporting */
static int g_passed = 0;
static int g_failed = 0;

static void Report(bool condition, const char *name) {
    if (condition) {
        ++g_passed;
        std::printf("[PASS] %s\n", name);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", name);
    }
    std::fflush(stdout);
}

static void Section(const char *title) {
    std::printf("\n== %s ==\n", title);
    std::fflush(stdout);
}

/* ------------------------------------------------------------------- utils */
static double NowMs() {
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static std::wstring GetEnvW(const wchar_t *name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) return std::wstring();
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, &value[0], length);
    value.resize((size_t)written);
    return value;
}

static std::wstring JoinPath(const std::wstring &dir, const wchar_t *name) {
    if (dir.empty()) return std::wstring(name);
    const wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

static bool MakeDirectoryTree(const std::wstring &path) {
    if (path.empty()) return false;
    std::wstring current;
    current.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        const bool separator = path[i] == L'\\' || path[i] == L'/';
        const bool drive_root = current.size() == 3 && current[1] == L':' && separator;
        if ((separator && !drive_root) || i + 1 == path.size()) {
            CreateDirectoryW(current.c_str(), nullptr);
        }
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool FileSizeW(const std::wstring &path, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    *size = ((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow;
    return true;
}

static bool WriteBytes(const std::wstring &path, const void *data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, (DWORD)size, &written, nullptr) != 0 &&
                    written == (DWORD)size;
    CloseHandle(file);
    return ok;
}

static std::string Narrow(const std::wstring &wide) { return zephium_adblock_utf8(wide); }

/* ------------------------------------------------------------ test helpers */
static zephium_adblock_engine *BuildEngine(const std::vector<std::string> &lists,
                                           uint64_t *rule_count, double *build_ms,
                                           zephium_adblock_status_t *status_out) {
    zephium_adblock_engine *engine = zephium_adblock_engine_new(0);
    if (!engine) return nullptr;
    std::vector<const char *> texts;
    std::vector<size_t> lengths;
    for (const std::string &list : lists) {
        texts.push_back(list.data());
        lengths.push_back(list.size());
    }
    const double started = NowMs();
    *status_out = zephium_adblock_engine_load(engine, texts.data(), lengths.data(),
                                              texts.size(), rule_count);
    *build_ms = NowMs() - started;
    if (*status_out != ZEPHIUM_ADBLOCK_OK) {
        zephium_adblock_engine_free(engine);
        return nullptr;
    }
    return engine;
}

static zephium_adblock_verdict_t Check(zephium_adblock_engine *engine, const char *url,
                                       const char *source, zephium_adblock_resource_type_t type,
                                       zephium_adblock_result_t *result,
                                       zephium_adblock_status_t *status_out) {
    zephium_adblock_result_t local = {};
    zephium_adblock_result_t *out = result ? result : &local;
    const zephium_adblock_status_t status =
        zephium_adblock_check(engine, url, source, type, "GET", out);
    if (status_out) *status_out = status;
    return status == ZEPHIUM_ADBLOCK_OK ? out->verdict : ZEPHIUM_ADBLOCK_ALLOW;
}

/* ------------------------------------------------------- classification tests */
static void TestClassification() {
    Section("classification helpers (shared with the shell)");

    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_DOCUMENT) ==
               ZEPHIUM_ADBLOCK_RES_DOCUMENT,
           "DOCUMENT context maps to document");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_SCRIPT) ==
               ZEPHIUM_ADBLOCK_RES_SCRIPT,
           "SCRIPT context maps to script");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_IMAGE) ==
               ZEPHIUM_ADBLOCK_RES_IMAGE,
           "IMAGE context maps to image");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_STYLESHEET) ==
               ZEPHIUM_ADBLOCK_RES_STYLESHEET,
           "STYLESHEET context maps to stylesheet");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_FONT) ==
               ZEPHIUM_ADBLOCK_RES_FONT,
           "FONT context maps to font");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_MEDIA) ==
               ZEPHIUM_ADBLOCK_RES_MEDIA,
           "MEDIA context maps to media");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_FETCH) ==
               ZEPHIUM_ADBLOCK_RES_XHR,
           "FETCH context maps to xhr");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_XMLHTTPREQUEST) ==
               ZEPHIUM_ADBLOCK_RES_XHR,
           "XMLHTTPREQUEST context maps to xhr");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_PING) ==
               ZEPHIUM_ADBLOCK_RES_PING,
           "PING context maps to ping");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_MANIFEST) ==
               ZEPHIUM_ADBLOCK_RES_MANIFEST,
           "MANIFEST context maps to manifest");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_WEBSOCKET) ==
               ZEPHIUM_ADBLOCK_RES_WEBSOCKET,
           "WEBSOCKET context maps to websocket (never raised by WebView2)");
    Report(zephium_adblock_map_context(ZEPHIUM_W2_CONTEXT_OTHER) ==
               ZEPHIUM_ADBLOCK_RES_OTHER,
           "unknown context maps to other");
    Report(zephium_adblock_map_context(-1) == ZEPHIUM_ADBLOCK_RES_OTHER,
           "out-of-range context maps to other");
    Report(zephium_adblock_type_name(ZEPHIUM_ADBLOCK_RES_SUBDOCUMENT) != nullptr &&
               std::strcmp(zephium_adblock_type_name(ZEPHIUM_ADBLOCK_RES_SUBDOCUMENT),
                           "subdocument") == 0,
           "type names are stable for logs");

    /* iframe tracking: WebView2 reports iframe navigations as DOCUMENT. */
    ZephiumAdblockFrameTracker tracker;
    tracker.BeginDocument("https://news.example/");
    Report(tracker.document_url() == "https://news.example/", "BeginDocument stores the URL");
    Report(zephium_adblock_classify(ZEPHIUM_W2_CONTEXT_DOCUMENT, "https://news.example/",
                                    tracker) == ZEPHIUM_ADBLOCK_RES_DOCUMENT,
           "top-level document stays DOCUMENT");
    tracker.FrameNavigating("https://ads.example/frame.html#section");
    Report(zephium_adblock_classify(ZEPHIUM_W2_CONTEXT_DOCUMENT,
                                    "https://ads.example/frame.html", tracker) ==
               ZEPHIUM_ADBLOCK_RES_SUBDOCUMENT,
           "iframe navigation becomes SUBDOCUMENT (fragment ignored)");
    Report(zephium_adblock_classify(ZEPHIUM_W2_CONTEXT_DOCUMENT,
                                    "https://news.example/other", tracker) ==
               ZEPHIUM_ADBLOCK_RES_DOCUMENT,
           "another top-level URL is not mistaken for an iframe");
    Report(zephium_adblock_classify(ZEPHIUM_W2_CONTEXT_SCRIPT, "https://ads.example/frame.html",
                                    tracker) == ZEPHIUM_ADBLOCK_RES_SCRIPT,
           "iframes only change the DOCUMENT classification, not resource types");

    const std::string first = "https://frame0.example/";
    for (unsigned i = 0; i < 200; ++i) {
        tracker.FrameNavigating("https://frame" + std::to_string(i) + ".example/");
    }
    Report(tracker.tracked_frames() == ZephiumAdblockFrameTracker::kMaxTrackedFrames,
           "iframe tracking is bounded (64 entries)");
    Report(tracker.IsIframeNavigation("https://frame199.example/"),
           "newest iframe navigation is still tracked");
    Report(!tracker.IsIframeNavigation(first), "oldest iframe navigation was evicted");

    tracker.BeginDocument("https://next.example/");
    Report(tracker.tracked_frames() == 0 && tracker.document_url() == "https://next.example/",
           "new top-level document clears stale iframes");
    tracker.SetDocumentUrl("https://spa.example/route#x");
    Report(tracker.document_url() == "https://spa.example/route",
           "SourceChanged URL is normalised");

    /* source URL rules */
    Report(zephium_adblock_source_url(ZEPHIUM_ADBLOCK_RES_DOCUMENT,
                                      "https://doc.example/", "https://parent.example/",
                                      "https://old.example/") == "https://doc.example/",
           "document requests use their own URL as source");
    Report(zephium_adblock_source_url(ZEPHIUM_ADBLOCK_RES_SCRIPT, "https://cdn.example/a.js",
                                      "https://parent.example/", "https://old.example/") ==
               "https://parent.example/",
           "subresources prefer the Referer header");
    Report(zephium_adblock_source_url(ZEPHIUM_ADBLOCK_RES_IMAGE, "https://cdn.example/a.png",
                                      "", "https://tab.example/page") ==
               "https://tab.example/page",
           "subresources fall back to the tab document URL");
    Report(zephium_adblock_source_url(ZEPHIUM_ADBLOCK_RES_IMAGE, "https://cdn.example/a.png",
                                      "about:blank", "file:///C:/page.html").empty(),
           "non-http sources are dropped instead of failing the check");
    Report(zephium_adblock_normalize_url("https://x.example/a?b=1#frag") ==
               "https://x.example/a?b=1",
           "normalize_url strips the fragment");
}

/* --------------------------------------------------------------- FFI tests */
static const char *kSyntheticList =
    "! Title: zephium selftest synthetic list\n"
    "[Adblock Plus 2.0]\n"
    "||ads.example^\n"
    "||tracker.example^$image\n"
    "@@||good.ads.example^\n"
    "||redirect.example/ads.js$script,redirect=1x1-transparent.gif\n"
    "||example.test^$removeparam=utm_source\n";

static void TestDecisions() {
    Section("engine decisions (blocker/adblock_ffi.h)");

    std::vector<std::string> lists = {kSyntheticList};
    uint64_t rules = 0;
    double build_ms = 0;
    zephium_adblock_status_t status = ZEPHIUM_ADBLOCK_OK;
    zephium_adblock_engine *engine = BuildEngine(lists, &rules, &build_ms, &status);
    Report(engine != nullptr, "synthetic filter list builds an engine");
    if (!engine) {
        std::printf("       last error: %s\n", zephium_adblock_last_error());
        return;
    }
    std::printf("       synthetic engine: %llu rules, built in %.2f ms\n",
                (unsigned long long)rules, build_ms);

    zephium_adblock_result_t result = {};
    Report(Check(engine, "https://ads.example/banner.png", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_IMAGE, &result, &status) == ZEPHIUM_ADBLOCK_BLOCK,
           "domain anchor rule blocks an image");
    Report(status == ZEPHIUM_ADBLOCK_OK, "check reports OK for a blocked request");
    Report(Check(engine, "https://news.example/app.js", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_SCRIPT, &result, &status) == ZEPHIUM_ADBLOCK_ALLOW,
           "unrelated script is allowed");
    Report(Check(engine, "https://good.ads.example/ad.js", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_SCRIPT, &result, &status) == ZEPHIUM_ADBLOCK_ALLOW,
           "exception rule overrides a broader block");
    Report(Check(engine, "https://tracker.example/px.png", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_IMAGE, &result, &status) == ZEPHIUM_ADBLOCK_BLOCK,
           "resource-type rule blocks the matching image");
    Report(Check(engine, "https://tracker.example/px.js", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_SCRIPT, &result, &status) == ZEPHIUM_ADBLOCK_ALLOW,
           "resource-type rule does not block other types");
    Report(Check(engine, "https://doubleclick.net/ad.js", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_SCRIPT, &result, &status) == ZEPHIUM_ADBLOCK_ALLOW,
           "unlisted domain is allowed (no false positive)");

    zephium_adblock_result_t redirect = {};
    Report(Check(engine, "https://redirect.example/ads.js", "https://news.example/",
                 ZEPHIUM_ADBLOCK_RES_SCRIPT, &redirect, &status) == ZEPHIUM_ADBLOCK_REDIRECT,
           "redirect rule produces a REDIRECT verdict");
    Report(redirect.redirect_len > 0 && redirect.redirect_body != nullptr,
           "redirect verdict carries a decoded body");
    Report(redirect.redirect_mime != nullptr &&
               std::strcmp(redirect.redirect_mime, "image/gif") == 0,
           "redirect verdict carries the MIME type");
    Report(redirect.redirect_len >= 6 &&
               std::memcmp(redirect.redirect_body, "GIF89a", 6) == 0,
           "redirect body is decoded pixel data, not a data: URI");

    zephium_adblock_result_t rewrite = {};
    Report(Check(engine, "https://example.test/page?utm_source=x&keep=1",
                 "https://news.example/", ZEPHIUM_ADBLOCK_RES_XHR, &rewrite, &status) ==
               ZEPHIUM_ADBLOCK_REWRITE,
           "removeparam rule produces a REWRITE verdict");
    Report(rewrite.rewritten_url != nullptr &&
               std::strstr(rewrite.rewritten_url, "utm_source") == nullptr &&
               std::strstr(rewrite.rewritten_url, "keep=1") != nullptr,
           "rewritten URL dropped the tracking parameter");

    zephium_adblock_stats_t stats = {};
    Report(zephium_adblock_engine_stats(engine, &stats) == ZEPHIUM_ADBLOCK_OK,
           "engine stats are readable");
    Report(stats.rule_count == rules && stats.checks > 0 && stats.blocked > 0 &&
               stats.redirected > 0 && stats.rewritten > 0,
           "engine stats count checks/blocked/redirected/rewritten");

    /* Engine without filters: fail-open semantics. */
    zephium_adblock_engine *empty = zephium_adblock_engine_new(0);
    Report(empty != nullptr && Check(empty, "https://ads.example/x", "",
                                     ZEPHIUM_ADBLOCK_RES_SCRIPT, &result, &status) ==
                                  ZEPHIUM_ADBLOCK_ALLOW,
           "an engine without rules allows everything");
    zephium_adblock_engine_free(empty);

    zephium_adblock_engine_free(engine);
}

static void TestThreadAffinity() {
    Section("thread affinity (engine handle is UI-thread only)");

    std::vector<std::string> lists = {kSyntheticList};
    uint64_t rules = 0;
    double build_ms = 0;
    zephium_adblock_status_t status = ZEPHIUM_ADBLOCK_OK;
    zephium_adblock_engine *engine = BuildEngine(lists, &rules, &build_ms, &status);
    Report(engine != nullptr, "engine created on the main thread");
    if (!engine) return;

    std::atomic<int> foreign_status{12345};
    std::thread worker([&]() {
        zephium_adblock_result_t result = {};
        foreign_status.store((int)zephium_adblock_check(engine, "https://ads.example/x", "",
                                                        ZEPHIUM_ADBLOCK_RES_SCRIPT, "GET",
                                                        &result));
    });
    worker.join();
    Report(foreign_status.load() == (int)ZEPHIUM_ADBLOCK_ERR_THREAD,
           "check from a foreign thread returns ERR_THREAD");
    Report(Check(engine, "https://ads.example/x", "", ZEPHIUM_ADBLOCK_RES_SCRIPT, nullptr,
                 nullptr) == ZEPHIUM_ADBLOCK_BLOCK,
           "engine keeps working after a foreign-thread rejection");
    zephium_adblock_engine_free(engine);
}

static void TestCache(const std::wstring &work_dir) {
    Section("binary engine cache");

    const std::wstring cache_path = JoinPath(work_dir, L"selftest-engine.bin");
    const std::wstring bad_path = JoinPath(work_dir, L"selftest-bad-cache.bin");
    DeleteFileW(cache_path.c_str());

    std::vector<std::string> lists = {kSyntheticList};
    uint64_t rules = 0;
    double build_ms = 0;
    zephium_adblock_status_t status = ZEPHIUM_ADBLOCK_OK;
    zephium_adblock_engine *engine = BuildEngine(lists, &rules, &build_ms, &status);
    Report(engine != nullptr, "engine built for the cache roundtrip");
    if (!engine) return;

    const double save_started = NowMs();
    const zephium_adblock_status_t saved =
        zephium_adblock_engine_save_cache(engine, Narrow(cache_path).c_str());
    const double save_ms = NowMs() - save_started;
    uint64_t cache_bytes = 0;
    Report(saved == ZEPHIUM_ADBLOCK_OK, "save_cache succeeds");
    Report(FileSizeW(cache_path, &cache_bytes) && cache_bytes > 0,
           "cache file exists and is not empty");
    std::printf("       cache size: %llu KB, save time: %.2f ms\n",
                (unsigned long long)(cache_bytes / 1024), save_ms);
    zephium_adblock_engine_free(engine);

    engine = zephium_adblock_engine_new(0);
    Report(engine != nullptr, "fresh handle for the warm start");
    const double load_started = NowMs();
    const zephium_adblock_status_t loaded =
        zephium_adblock_engine_load_cache(engine, Narrow(cache_path).c_str());
    const double load_ms = NowMs() - load_started;
    Report(loaded == ZEPHIUM_ADBLOCK_OK, "load_cache restores the engine");
    Report(load_ms < 500.0, "warm start is fast (< 500 ms)");
    std::printf("       cache load time: %.2f ms\n", load_ms);
    Report(Check(engine, "https://ads.example/x", "", ZEPHIUM_ADBLOCK_RES_SCRIPT, nullptr,
                 nullptr) == ZEPHIUM_ADBLOCK_BLOCK,
           "restored engine still blocks");
    zephium_adblock_stats_t stats = {};
    zephium_adblock_engine_stats(engine, &stats);
    Report(stats.rule_count == rules, "restored engine reports the original rule count");

    /* A broken cache must be rejected without disturbing the loaded rules. */
    Report(zephium_adblock_engine_load_cache(engine, Narrow(bad_path).c_str()) ==
               ZEPHIUM_ADBLOCK_ERR_CACHE,
           "missing cache is rejected with ERR_CACHE");
    const char garbage[] = "this is not a zephium adblock engine cache";
    Report(WriteBytes(bad_path, garbage, sizeof(garbage)) != 0,
           "corrupt cache fixture written");
    Report(zephium_adblock_engine_load_cache(engine, Narrow(bad_path).c_str()) ==
               ZEPHIUM_ADBLOCK_ERR_CACHE,
           "corrupt cache is rejected with ERR_CACHE");
    Report(Check(engine, "https://ads.example/x", "", ZEPHIUM_ADBLOCK_RES_SCRIPT, nullptr,
                 nullptr) == ZEPHIUM_ADBLOCK_BLOCK,
           "rules survive a failed cache load");
    zephium_adblock_engine_free(engine);
    DeleteFileW(cache_path.c_str());
    DeleteFileW(bad_path.c_str());
}

static void TestPanicContainment() {
    Section("panic containment (fail open, never crash)");
    std::printf("       (the Rust panic messages below are injected on purpose)\n");
    std::fflush(stdout);

    Report(zephium_adblock_selftest_panic() == ZEPHIUM_ADBLOCK_ERR_PANIC,
           "global panic hook is contained and reports ERR_PANIC");

    std::vector<std::string> lists = {kSyntheticList};
    uint64_t rules = 0;
    double build_ms = 0;
    zephium_adblock_status_t status = ZEPHIUM_ADBLOCK_OK;
    zephium_adblock_engine *engine = BuildEngine(lists, &rules, &build_ms, &status);
    Report(engine != nullptr, "engine built for the panic test");
    if (!engine) return;

    Report(Check(engine, "https://ads.example/x", "", ZEPHIUM_ADBLOCK_RES_SCRIPT, nullptr,
                 nullptr) == ZEPHIUM_ADBLOCK_BLOCK,
           "engine blocks before the injected panic");
    Report(zephium_adblock_engine_selftest_panic(engine) == ZEPHIUM_ADBLOCK_ERR_PANIC,
           "engine panic is contained and poisons the handle");
    zephium_adblock_result_t result = {};
    zephium_adblock_status_t after_status = ZEPHIUM_ADBLOCK_OK;
    const zephium_adblock_verdict_t verdict =
        Check(engine, "https://ads.example/x", "", ZEPHIUM_ADBLOCK_RES_SCRIPT, &result,
              &after_status);
    Report(after_status == ZEPHIUM_ADBLOCK_OK && verdict == ZEPHIUM_ADBLOCK_ALLOW,
           "checks on a poisoned handle fail open (ALLOW, no crash)");
    Report(zephium_adblock_last_error() != nullptr && zephium_adblock_last_error()[0] != '\0',
           "last_error explains the poisoned handle");
    zephium_adblock_engine_free(engine);
}

/* ------------------------------------------------------- engine lifecycle */
/* Exercises the real shell lifecycle (adblock_webview2.cpp): build from list
 * files, manifest + binary cache write, then a second start that warm-loads the
 * cache.  No window is created, so the (background) download path is not
 * involved - this is the code path zephium_win.exe uses on every start when the
 * lists are already on disk. */
static void TestEngineManager(const std::wstring &work_dir) {
    Section("engine lifecycle (build from lists -> cache -> warm start)");

    const std::wstring lists_dir = JoinPath(work_dir, L"lists");
    Report(MakeDirectoryTree(lists_dir), "lists directory created for the lifecycle test");
    if (!MakeDirectoryTree(lists_dir)) return;
    /* Every enabled default list must exist for the cache to be considered
     * valid, so write the same tiny synthetic list under each file name. */
    size_t catalog_size = 0;
    const ZephiumAdblockList *catalog = zephium_adblock_lists(&catalog_size);
    for (size_t i = 0; i < catalog_size; ++i) {
        if (!catalog[i].enabled_by_default) continue;
        const std::wstring path = JoinPath(lists_dir, catalog[i].filename);
        Report(WriteBytes(path, kSyntheticList, std::strlen(kSyntheticList)) != 0,
               "default list fixture written");
    }

    const std::wstring previous = GetEnvW(L"ZEPHIUM_ADBLOCK_DIR");
    SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", work_dir.c_str());

    ZephiumAdblockConfig config;
    config.hwnd = nullptr;   /* no window: the test never needs the refresh thread */
    config.exe_dir = L"C:\\fake\\zephium";
    config.enabled = true;
    config.logger = nullptr;

    const double first_started = NowMs();
    const bool first_ok = zephium_adblock_start(config);
    const double first_ms = NowMs() - first_started;
    Report(first_ok && zephium_adblock_is_enabled(), "ad blocking starts with cached lists");
    Report(zephium_adblock_is_ready(), "engine is ready after the synchronous build");
    std::printf("       first start (build + cache write): %.1f ms\n", first_ms);

    /* One request through the public check wrapper used by the shell. */
    zephium_adblock_result_t result = {};
    const zephium_adblock_status_t wrapper_status = zephium_adblock_check_url(
        "https://ads.example/banner.js", "https://news.example/", ZEPHIUM_ADBLOCK_RES_SCRIPT,
        "GET", &result);
    Report(wrapper_status == ZEPHIUM_ADBLOCK_OK && result.verdict == ZEPHIUM_ADBLOCK_BLOCK,
           "shell check wrapper blocks a known ad URL");
    zephium_adblock_shutdown();
    Report(!zephium_adblock_is_enabled(), "shutdown disables ad blocking");

    const double warm_started = NowMs();
    const bool second_ok = zephium_adblock_start(config);
    const double warm_ms = NowMs() - warm_started;
    Report(second_ok && zephium_adblock_is_ready(), "second start is warm-ready");
    Report(warm_ms <= first_ms + 250.0, "warm start is not slower than a cold rebuild");
    std::printf("       second start (cache load): %.1f ms\n", warm_ms);
    result = {};
    const zephium_adblock_status_t warm_status = zephium_adblock_check_url(
        "https://ads.example/banner.js", "https://news.example/", ZEPHIUM_ADBLOCK_RES_SCRIPT,
        "GET", &result);
    Report(warm_status == ZEPHIUM_ADBLOCK_OK && result.verdict == ZEPHIUM_ADBLOCK_BLOCK,
           "warm engine still filters");
    zephium_adblock_shutdown();

    /* Clean up the fixtures so a rerun starts from scratch. */
    for (size_t i = 0; i < catalog_size; ++i) {
        if (!catalog[i].enabled_by_default) continue;
        DeleteFileW(JoinPath(lists_dir, catalog[i].filename).c_str());
    }
    DeleteFileW(JoinPath(lists_dir, L"manifest.txt").c_str());
    DeleteFileW(JoinPath(work_dir, L"engine.bin").c_str());
    if (previous.empty()) {
        SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", nullptr);
    } else {
        SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", previous.c_str());
    }
}

/* --------------------------------------------------------------- benchmark */
static std::string MakeSyntheticRules(unsigned count) {
    std::string text;
    text.reserve((size_t)count * 48u + 64u);
    text += "! Title: zephium selftest benchmark (synthetic)\n";
    text += "[Adblock Plus 2.0]\n";
    char line[96];
    for (unsigned i = 0; i < count; ++i) {
        std::snprintf(line, sizeof(line), "||ads%u.example^$script,third-party\n", i);
        text += line;
    }
    return text;
}

/* Downloads a couple of real lists into memory, if the network allows it. */
static std::vector<std::string> DownloadRealLists(size_t max_lists, uint64_t *bytes_out) {
    std::vector<std::string> lists;
    *bytes_out = 0;
    size_t catalog_size = 0;
    const ZephiumAdblockList *catalog = zephium_adblock_lists(&catalog_size);

    static const char *wanted[] = {"easylist", "easyprivacy", "ublock-filters"};
    for (const char *id : wanted) {
        if (lists.size() >= max_lists) break;
        const ZephiumAdblockList *found = nullptr;
        for (size_t i = 0; i < catalog_size; ++i) {
            if (std::strcmp(catalog[i].id, id) == 0) {
                found = &catalog[i];
                break;
            }
        }
        if (!found) continue;
        std::vector<unsigned char> bytes;
        std::wstring error;
        const double started = NowMs();
        const bool ok = zephium_adblock_download_list(*found, &bytes, &error);
        if (!ok) {
            std::printf("       network: %s not available (%ls)\n", id, error.c_str());
            std::fflush(stdout);
            continue;
        }
        const double elapsed_ms = NowMs() - started;
        *bytes_out += (uint64_t)bytes.size();
        std::printf("       network: %s %llu KB in %.0f ms\n", id,
                    (unsigned long long)(bytes.size() / 1024), elapsed_ms);
        std::fflush(stdout);
        lists.push_back(std::string(bytes.begin(), bytes.end()));
    }
    return lists;
}

static void TestBenchmark(bool allow_network) {
    const std::string benchmark_title =
        std::string("benchmark: ~140k synthetic rules") +
        (allow_network ? " + real lists" : " (network disabled)");
    Section(benchmark_title.c_str());

    const unsigned kSyntheticRules = 140000;
    const unsigned kChecks = 100000;
    std::vector<std::string> lists;
    lists.push_back(MakeSyntheticRules(kSyntheticRules));
    std::printf("       synthetic list: %llu KB, %u rules requested\n",
                (unsigned long long)(lists[0].size() / 1024), kSyntheticRules);

    uint64_t real_bytes = 0;
    if (allow_network) {
        const std::vector<std::string> real = DownloadRealLists(3, &real_bytes);
        for (const std::string &list : real) lists.push_back(list);
        if (real.empty()) std::printf("       network unavailable: using synthetic rules only\n");
    }

    uint64_t rule_count = 0;
    double build_ms = 0;
    zephium_adblock_status_t status = ZEPHIUM_ADBLOCK_OK;
    const double build_started = NowMs();
    zephium_adblock_engine *engine = BuildEngine(lists, &rule_count, &build_ms, &status);
    Report(engine != nullptr, "benchmark engine builds");
    if (!engine) {
        std::printf("       last error: %s\n", zephium_adblock_last_error());
        return;
    }
    std::printf("       engine: %llu rules, %llu KB of lists, build time %.1f ms\n",
                (unsigned long long)rule_count,
                (unsigned long long)((lists[0].size() + real_bytes) / 1024), build_ms);
    (void)build_started;
    Report(rule_count >= kSyntheticRules, "engine loaded at least the synthetic rule count");

    const std::wstring cache_path = JoinPath(GetEnvW(L"ZEPHIUM_ADBLOCK_DIR"),
                                             L"benchmark-engine.bin");
    const double save_started = NowMs();
    if (zephium_adblock_engine_save_cache(engine, Narrow(cache_path).c_str()) ==
        ZEPHIUM_ADBLOCK_OK) {
        uint64_t cache_bytes = 0;
        FileSizeW(cache_path, &cache_bytes);
        std::printf("       cache: %llu KB, save time %.1f ms\n",
                    (unsigned long long)(cache_bytes / 1024), NowMs() - save_started);
        DeleteFileW(cache_path.c_str());
    }

    std::vector<int64_t> samples;
    samples.reserve(kChecks);
    uint64_t blocked = 0;
    uint64_t allow = 0;
    const zephium_adblock_resource_type_t types[] = {
        ZEPHIUM_ADBLOCK_RES_SCRIPT, ZEPHIUM_ADBLOCK_RES_IMAGE, ZEPHIUM_ADBLOCK_RES_XHR,
        ZEPHIUM_ADBLOCK_RES_DOCUMENT};
    char url[160];
    char source[160];
    for (unsigned i = 0; i < kChecks; ++i) {
        if (i % 10u < 3u) {
            std::snprintf(url, sizeof(url), "https://ads%u.example/asset%u.js",
                          i % kSyntheticRules, i);
        } else {
            std::snprintf(url, sizeof(url), "https://site%u.example/asset%u.js", i % 1000u, i);
        }
        std::snprintf(source, sizeof(source), "https://news%u.example/", i % 100u);
        const zephium_adblock_resource_type_t type = types[i % 4u];

        zephium_adblock_result_t result = {};
        const auto started = std::chrono::steady_clock::now();
        const zephium_adblock_status_t check_status =
            zephium_adblock_check(engine, url, source, type, "GET", &result);
        const auto finished = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
        if (check_status == ZEPHIUM_ADBLOCK_OK && result.verdict == ZEPHIUM_ADBLOCK_BLOCK) {
            ++blocked;
        } else {
            ++allow;
        }
    }

    std::vector<int64_t> sorted(samples);
    std::sort(sorted.begin(), sorted.end());
    long double total = 0;
    for (int64_t value : samples) total += (long double)value;
    const int64_t average = (int64_t)(total / (long double)samples.size());
    const int64_t median = sorted[sorted.size() / 2];
    const int64_t p99 = sorted[(size_t)((double)sorted.size() * 0.99)];
    const int64_t maximum = sorted.back();
    std::printf("       checks: %u (%llu blocked, %llu allowed)\n", kChecks,
                (unsigned long long)blocked, (unsigned long long)allow);
    std::printf("       per-check time: avg %lld ns, median %lld ns, p99 %lld ns, max %lld ns\n",
                (long long)average, (long long)median, (long long)p99, (long long)maximum);
    Report(p99 < 100000, "p99 per-check time below 100 us");
    Report(blocked > 0 && allow > 0, "benchmark exercised both block and allow paths");
    zephium_adblock_engine_free(engine);
}

static void TestCatalogAndDirectories(const std::wstring &work_dir) {
    Section("filter list catalog and data directory");

    size_t count = 0;
    const ZephiumAdblockList *lists = zephium_adblock_lists(&count);
    Report(lists != nullptr && count >= 5, "filter list catalog is present");
    size_t defaults = 0;
    size_t default_cap = 0;
    bool ids_unique = true;
    for (size_t i = 0; i < count; ++i) {
        if (lists[i].enabled_by_default) {
            ++defaults;
            /* Only the default set is read into the engine; optional lists are
             * downloaded only if a future build enables them. */
            default_cap += lists[i].max_bytes;
        }
        for (size_t j = i + 1; j < count; ++j) {
            if (std::strcmp(lists[i].id, lists[j].id) == 0) ids_unique = false;
        }
        if (lists[i].max_bytes == 0 || lists[i].url == nullptr ||
            std::strncmp(lists[i].url, "https://", 8) != 0) {
            Report(false, "catalog entries have an https URL and a size cap");
            return;
        }
    }
    Report(ids_unique, "catalog list ids are unique");
    Report(defaults == 5, "default set is EasyList + EasyPrivacy + 3 uBO lists");
    Report(default_cap <= ZEPHIUM_ADBLOCK_MAX_TOTAL_LIST_BYTES,
           "the default set's per-list caps fit inside the total size cap");

    /* ZEPHIUM_ADBLOCK_DIR override and the production default. */
    const std::wstring previous = GetEnvW(L"ZEPHIUM_ADBLOCK_DIR");
    SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", work_dir.c_str());
    Report(zephium_adblock_root_dir(L"C:\\fake\\exe") == work_dir,
           "ZEPHIUM_ADBLOCK_DIR overrides the data directory");
    SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", nullptr);
    const std::wstring local_app_data = GetEnvW(L"LOCALAPPDATA");
    if (!local_app_data.empty()) {
        Report(zephium_adblock_root_dir(L"C:\\fake\\exe") ==
                   local_app_data + L"\\Zephium\\adblock",
               "production default is %LOCALAPPDATA%\\Zephium\\adblock");
    } else {
        Report(zephium_adblock_root_dir(L"C:\\fake\\exe") ==
                   std::wstring(L"C:\\fake\\exe\\zephium_adblock"),
               "without LOCALAPPDATA the directory falls back next to the exe");
    }
    if (previous.empty()) {
        SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", nullptr);
    } else {
        SetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", previous.c_str());
    }
}

/* -------------------------------------------------------------------- main */
int main(int argc, char **argv) {
    bool allow_network = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-network") == 0) allow_network = false;
    }

    std::printf("Zephium ad-block self test\n");
    std::printf("engine: %s\n", zephium_adblock_version());
    std::printf("ABI: header %u, library %u\n", ZEPHIUM_ADBLOCK_ABI_VERSION,
                zephium_adblock_abi_version());
    Report(zephium_adblock_abi_version() == ZEPHIUM_ADBLOCK_ABI_VERSION,
           "header and staticlib ABI versions match");

    /* Test data lives inside the workspace (or wherever ZEPHIUM_ADBLOCK_DIR
     * points); %TEMP% may be read-only for processes spawned from a build. */
    std::wstring work_dir = GetEnvW(L"ZEPHIUM_ADBLOCK_DIR");
    if (work_dir.empty()) {
        wchar_t current[1024] = {0};
        GetCurrentDirectoryW(1024, current);
        work_dir = JoinPath(current, L"obj\\adblock-selftest");
    }
    if (!MakeDirectoryTree(work_dir)) {
        std::printf("[FAIL] could not create the test data directory: %ls\n", work_dir.c_str());
        ++g_failed;
    } else {
        std::printf("test data directory: %ls\n", work_dir.c_str());
    }
    std::fflush(stdout);

    TestClassification();
    TestCatalogAndDirectories(work_dir);
    TestDecisions();
    TestThreadAffinity();
    TestCache(work_dir);
    TestPanicContainment();
    TestEngineManager(work_dir);
    TestBenchmark(allow_network);

    std::printf("\n===== SUMMARY: %d passed, %d failed =====\n", g_passed, g_failed);
    if (g_failed != 0) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
