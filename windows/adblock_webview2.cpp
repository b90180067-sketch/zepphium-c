/* =============================================================================
 * Zephium ad blocking - filter list management + engine lifecycle (Windows)
 *
 * The engine is adblock-rust behind blocker/adblock_ffi.h.  adblock-rust is
 * compiled in its fast single-thread mode and the FFI makes handles thread
 * affine, so this file splits the work along the same line:
 *
 *   UI thread (create/check/free the engine)
 *     - warm start from <root>\engine.bin (binary cache, milliseconds)
 *     - build from <root>\lists\*.txt when the cache is stale/missing
 *     - rebuild + swap after the refresh thread reports new list bytes
 *
 *   refresh thread (never touches the engine)
 *     - WinHTTP download of the filter lists into the lists directory
 *     - atomic tmp+rename writes, content-hash comparison against the old file
 *     - PostMessage(ZEPHIUM_ADBLOCK_WM_REFRESH) when anything changed
 *
 * A local manifest (<root>\lists\manifest.txt) records the bytes+FNV-1a hash of
 * every list file plus the last refresh time.  It decides whether engine.bin is
 * still valid, and keeps the 24h refresh from downloading on every start.
 * Every failure keeps the browser working: no engine simply means no filtering.
 * ========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adblock_webview2.h"

/* ------------------------------------------------------------------ logging */
/* The shell owns the log file; it installs this sink before zephium_adblock_start
 * (both the UI thread and the refresh thread call it). */
static void (*g_logger)(const wchar_t *line) = nullptr;

static void AdblockLog(const wchar_t *fmt, ...) {
    if (!g_logger) return;
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(line, (sizeof(line) / sizeof(line[0])) - 1, fmt, ap);
    va_end(ap);
    line[(sizeof(line) / sizeof(line[0])) - 1] = L'\0';
    g_logger(line);
}

static std::wstring LastErrorText() {
    const char *text = zephium_adblock_last_error();
    std::wstring wide = zephium_adblock_wide(text ? text : "");
    return wide.empty() ? std::wstring(L"(no detail)") : wide;
}

/* --------------------------------------------------------------- conversions */
std::string zephium_adblock_utf8(const std::wstring &wide) {
    if (wide.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &out[0], len, nullptr,
                        nullptr);
    return out;
}

std::wstring zephium_adblock_wide(const std::string &utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], len);
    return out;
}

/* ------------------------------------------- request classification helpers */
std::string zephium_adblock_normalize_url(const std::string &url) {
    const size_t fragment = url.find('#');
    return fragment == std::string::npos ? url : url.substr(0, fragment);
}

static bool StartsWithNoCase(const std::string &text, const char *prefix) {
    const size_t n = strlen(prefix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = text[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool IsAbsoluteHttpUrl(const std::string &url) {
    return StartsWithNoCase(url, "http://") || StartsWithNoCase(url, "https://");
}

/* The WebView2 enum values are mirrored in the header so the self test does not
 * need WebView2.h; zephium_win.cpp static_asserts that they still match. */
zephium_adblock_resource_type_t zephium_adblock_map_context(int context) {
    switch (context) {
    case ZEPHIUM_W2_CONTEXT_STYLESHEET:
        return ZEPHIUM_ADBLOCK_RES_STYLESHEET;
    case ZEPHIUM_W2_CONTEXT_IMAGE:
        return ZEPHIUM_ADBLOCK_RES_IMAGE;
    case ZEPHIUM_W2_CONTEXT_MEDIA:
        return ZEPHIUM_ADBLOCK_RES_MEDIA;
    case ZEPHIUM_W2_CONTEXT_FONT:
        return ZEPHIUM_ADBLOCK_RES_FONT;
    case ZEPHIUM_W2_CONTEXT_SCRIPT:
        return ZEPHIUM_ADBLOCK_RES_SCRIPT;
    case ZEPHIUM_W2_CONTEXT_XMLHTTPREQUEST:
    case ZEPHIUM_W2_CONTEXT_FETCH:
    case ZEPHIUM_W2_CONTEXT_EVENTSOURCE:
        /* adblock-rust has no separate fetch/event-source type; uBO treats all
         * three as xmlhttprequest for $xhr filtering. */
        return ZEPHIUM_ADBLOCK_RES_XHR;
    case ZEPHIUM_W2_CONTEXT_TEXTTRACK:
        /* No text-track type in the ABI; media rules are the closest match. */
        return ZEPHIUM_ADBLOCK_RES_MEDIA;
    case ZEPHIUM_W2_CONTEXT_WEBSOCKET:
        /* Documented limitation: WebView2 never raises WebResourceRequested for
         * WebSockets, so this mapping is unused in practice. */
        return ZEPHIUM_ADBLOCK_RES_WEBSOCKET;
    case ZEPHIUM_W2_CONTEXT_MANIFEST:
        return ZEPHIUM_ADBLOCK_RES_MANIFEST;
    case ZEPHIUM_W2_CONTEXT_PING:
        return ZEPHIUM_ADBLOCK_RES_PING;
    case ZEPHIUM_W2_CONTEXT_SIGNEDEXCHANGE:
        /* A signed exchange replaces a top-level document. */
        return ZEPHIUM_ADBLOCK_RES_DOCUMENT;
    case ZEPHIUM_W2_CONTEXT_CSP_VIOLATION_REPORT:
        return ZEPHIUM_ADBLOCK_RES_OTHER;
    case ZEPHIUM_W2_CONTEXT_DOCUMENT:
        return ZEPHIUM_ADBLOCK_RES_DOCUMENT;
    case ZEPHIUM_W2_CONTEXT_ALL:
    case ZEPHIUM_W2_CONTEXT_OTHER:
    default:
        return ZEPHIUM_ADBLOCK_RES_OTHER;
    }
}

const char *zephium_adblock_type_name(zephium_adblock_resource_type_t type) {
    switch (type) {
    case ZEPHIUM_ADBLOCK_RES_DOCUMENT: return "document";
    case ZEPHIUM_ADBLOCK_RES_SUBDOCUMENT: return "subdocument";
    case ZEPHIUM_ADBLOCK_RES_SCRIPT: return "script";
    case ZEPHIUM_ADBLOCK_RES_STYLESHEET: return "stylesheet";
    case ZEPHIUM_ADBLOCK_RES_IMAGE: return "image";
    case ZEPHIUM_ADBLOCK_RES_FONT: return "font";
    case ZEPHIUM_ADBLOCK_RES_MEDIA: return "media";
    case ZEPHIUM_ADBLOCK_RES_XHR: return "xhr";
    case ZEPHIUM_ADBLOCK_RES_PING: return "ping";
    case ZEPHIUM_ADBLOCK_RES_WEBSOCKET: return "websocket";
    case ZEPHIUM_ADBLOCK_RES_MANIFEST: return "manifest";
    case ZEPHIUM_ADBLOCK_RES_OBJECT: return "object";
    case ZEPHIUM_ADBLOCK_RES_OTHER: return "other";
    default: return "other";
    }
}

void ZephiumAdblockFrameTracker::Clear() {
    m_document_url.clear();
    m_frames.clear();
}

void ZephiumAdblockFrameTracker::BeginDocument(const std::string &url) {
    m_frames.clear();
    m_document_url = zephium_adblock_normalize_url(url);
}

void ZephiumAdblockFrameTracker::SetDocumentUrl(const std::string &url) {
    m_document_url = zephium_adblock_normalize_url(url);
}

void ZephiumAdblockFrameTracker::FrameNavigating(const std::string &url) {
    std::string normalized = zephium_adblock_normalize_url(url);
    if (normalized.empty()) return;
    for (const std::string &existing : m_frames) {
        if (existing == normalized) return;  /* re-navigation of the same iframe */
    }
    if (m_frames.size() >= kMaxTrackedFrames) {
        m_frames.erase(m_frames.begin());  /* oldest first: bounded memory */
    }
    m_frames.push_back(normalized);
}

bool ZephiumAdblockFrameTracker::IsIframeNavigation(const std::string &url) const {
    const std::string normalized = zephium_adblock_normalize_url(url);
    if (normalized.empty()) return false;
    for (const std::string &tracked : m_frames) {
        if (tracked == normalized) return true;
    }
    return false;
}

zephium_adblock_resource_type_t zephium_adblock_classify(
    int context, const std::string &url, const ZephiumAdblockFrameTracker &tracker) {
    zephium_adblock_resource_type_t type = zephium_adblock_map_context(context);
    /* WebView2 reports iframe navigations as DOCUMENT; only the per-tab tracked
     * iframe URLs can tell a real subdocument from the main document. */
    if (type == ZEPHIUM_ADBLOCK_RES_DOCUMENT && tracker.IsIframeNavigation(url)) {
        return ZEPHIUM_ADBLOCK_RES_SUBDOCUMENT;
    }
    return type;
}

std::string zephium_adblock_source_url(zephium_adblock_resource_type_t type,
                                       const std::string &request_url,
                                       const std::string &referer,
                                       const std::string &document_url) {
    if (type == ZEPHIUM_ADBLOCK_RES_DOCUMENT) return request_url;
    if (IsAbsoluteHttpUrl(referer)) return referer;
    if (IsAbsoluteHttpUrl(document_url)) return document_url;
    /* No usable initiator: adblock-rust treats this as first-party, which is the
     * conservative choice for third-party rules. */
    return std::string();
}

/* ------------------------------------------------------- filter list catalog */
/* Default set: EasyList + EasyPrivacy + uBlock Origin filters/badware/quick-fixes.
 * The remaining uBO assets and Peter Lowe are shipped in the catalog but off by
 * default (they overlap the defaults and cost parse time/rule memory). */
static const ZephiumAdblockList kFilterLists[] = {
    {"easylist", L"easylist.txt", "https://easylist.to/easylist/easylist.txt",
     8u * 1024u * 1024u, true},
    {"easyprivacy", L"easyprivacy.txt", "https://easylist.to/easylist/easyprivacy.txt",
     8u * 1024u * 1024u, true},
    {"ublock-filters", L"ublock-filters.txt",
     "https://ublockorigin.github.io/uAssets/filters/filters.txt", 8u * 1024u * 1024u, true},
    {"ublock-badware", L"ublock-badware.txt",
     "https://ublockorigin.github.io/uAssets/filters/badware.txt", 4u * 1024u * 1024u, true},
    {"ublock-quick-fixes", L"ublock-quick-fixes.txt",
     "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt", 2u * 1024u * 1024u,
     true},
    /* Optional lists, not downloaded by default. */
    {"ublock-privacy", L"ublock-privacy.txt",
     "https://ublockorigin.github.io/uAssets/filters/privacy.txt", 4u * 1024u * 1024u, false},
    {"ublock-unbreak", L"ublock-unbreak.txt",
     "https://ublockorigin.github.io/uAssets/filters/unbreak.txt", 2u * 1024u * 1024u, false},
    {"peter-lowe", L"peter-lowe.txt",
     "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&mimetype=plaintext",
     2u * 1024u * 1024u, false},
};

const ZephiumAdblockList *zephium_adblock_lists(size_t *count) {
    if (count) *count = sizeof(kFilterLists) / sizeof(kFilterLists[0]);
    return kFilterLists;
}

static size_t EnabledListCount() {
    size_t total = 0;
    const size_t count = sizeof(kFilterLists) / sizeof(kFilterLists[0]);
    for (size_t i = 0; i < count; ++i) {
        if (kFilterLists[i].enabled_by_default) ++total;
    }
    return total;
}

/* ------------------------------------------------------------- file helpers */
static std::wstring JoinPath(const std::wstring &dir, const wchar_t *name) {
    if (dir.empty()) return std::wstring(name);
    const wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

static bool FileExistsW(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool FileSizeW(const std::wstring &path, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    *size = ((uint64_t)data.nFileSizeHigh << 32) | (uint64_t)data.nFileSizeLow;
    return true;
}

static bool EnsureDirectory(const std::wstring &path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Creates every missing component; %LOCALAPPDATA%\Zephium\adblock needs two new
 * levels on a fresh profile.  Reports the Win32 error of the first component that
 * could not be created so a failure can be logged with a reason instead of a
 * bare "could not create". */
static bool EnsureDirectoryTreeEx(const std::wstring &path, DWORD *error_out) {
    if (error_out) *error_out = ERROR_SUCCESS;
    if (path.empty()) {
        if (error_out) *error_out = ERROR_INVALID_PARAMETER;
        return false;
    }
    DWORD first_error = ERROR_SUCCESS;
    std::wstring current;
    current.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        const bool separator = path[i] == L'\\' || path[i] == L'/';
        const bool drive_root =
            current.size() == 3 && current[1] == L':' && separator;
        if ((separator && !drive_root) || i + 1 == path.size()) {
            if (!CreateDirectoryW(current.c_str(), nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS && first_error == ERROR_SUCCESS) {
                    first_error = error;
                }
            }
        }
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool ok = attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!ok && error_out) {
        *error_out = first_error != ERROR_SUCCESS ? first_error : ERROR_PATH_NOT_FOUND;
    }
    return ok;
}

static bool ReadFileBytes(const std::wstring &path, size_t max_bytes,
                          std::vector<unsigned char> *out) {
    out->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (uint64_t)size.QuadPart > (uint64_t)max_bytes) {
        CloseHandle(file);
        return false;
    }
    out->resize((size_t)size.QuadPart);
    size_t offset = 0;
    while (offset < out->size()) {
        DWORD chunk = (DWORD)((out->size() - offset) > (1u << 20) ? (1u << 20)
                                                                 : (out->size() - offset));
        DWORD read = 0;
        if (!ReadFile(file, out->data() + offset, chunk, &read, nullptr) || read == 0) {
            CloseHandle(file);
            out->clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

/* tmp+rename so a crash never leaves a half-written list (and a concurrent
 * reader never sees one): the rename is atomic on NTFS. */
static bool WriteFileAtomic(const std::wstring &path, const void *data, size_t size) {
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        AdblockLog(L"adblock: cannot create %ls (error %lu)", temp.c_str(),
                   (unsigned long)GetLastError());
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    size_t offset = 0;
    bool ok = true;
    DWORD failure = ERROR_SUCCESS;
    while (offset < size) {
        DWORD chunk = (DWORD)((size - offset) > (1u << 20) ? (1u << 20) : (size - offset));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, chunk, &written, nullptr) || written == 0) {
            failure = GetLastError();
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) failure = GetLastError();
    if (!failure && !ok && failure == ERROR_SUCCESS) failure = ERROR_WRITE_FAULT;
    CloseHandle(file);
    if (!ok || failure != ERROR_SUCCESS) {
        DeleteFileW(temp.c_str());
        AdblockLog(L"adblock: cannot write %ls (error %lu)", temp.c_str(),
                   (unsigned long)failure);
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        AdblockLog(L"adblock: cannot replace %ls (error %lu)", path.c_str(),
                   (unsigned long)error);
        return false;
    }
    return true;
}

/* 64-bit FNV-1a: only used to detect list changes and validate the cache, not
 * for security. */
static uint64_t Fnv1a(const unsigned char *data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool HashFile(const std::wstring &path, uint64_t *size_out, uint64_t *hash_out) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, ZEPHIUM_ADBLOCK_MAX_TOTAL_LIST_BYTES, &bytes) || bytes.empty()) {
        return false;
    }
    *size_out = (uint64_t)bytes.size();
    *hash_out = Fnv1a(bytes.data(), bytes.size());
    return true;
}

static uint64_t NowUnixSeconds() {
    FILETIME file_time = {};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    const uint64_t hundred_ns = value.QuadPart;
    return hundred_ns / 10000000ULL - 11644473600ULL;  /* 1601 -> 1970 */
}

/* --------------------------------------------------------------- paths/manifest */
static bool g_started = false;
static bool g_enabled = false;
static bool g_engine_ready = false;
static HWND g_hwnd = nullptr;
static std::wstring g_exe_dir;
static std::wstring g_root;
static zephium_adblock_engine *g_engine = nullptr;
static uint64_t g_last_refresh_unix = 0;
static ULONGLONG g_last_stats_ms = 0;
static ULONGLONG g_last_due_check_ms = 0;

static std::wstring ListsDirectory() { return JoinPath(g_root, L"lists"); }
static std::wstring CachePath() { return JoinPath(g_root, L"engine.bin"); }
static std::wstring ManifestPath() { return JoinPath(ListsDirectory(), L"manifest.txt"); }

struct ManifestEntry {
    std::string id;
    uint64_t bytes = 0;
    uint64_t hash = 0;
};

struct Manifest {
    bool valid = false;
    uint64_t refreshed_unix = 0;
    std::vector<ManifestEntry> entries;
};

static Manifest ReadManifest() {
    Manifest manifest;
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(ManifestPath(), 1024u * 1024u, &bytes)) return manifest;
    const std::string text(bytes.begin(), bytes.end());
    size_t position = 0;
    while (position < text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        position = end + 1;
        if (line.rfind("# zephium adblock manifest", 0) == 0) {
            manifest.valid = true;
        } else if (line.rfind("refreshed ", 0) == 0) {
            manifest.refreshed_unix = _strtoui64(line.c_str() + 10, nullptr, 10);
        } else if (line.rfind("list ", 0) == 0) {
            char id[64] = {0};
            unsigned long long list_bytes = 0;
            unsigned long long hash = 0;
            if (sscanf(line.c_str() + 5, "%63s %llu %llx", id, &list_bytes, &hash) == 3) {
                ManifestEntry entry;
                entry.id = id;
                entry.bytes = (uint64_t)list_bytes;
                entry.hash = (uint64_t)hash;
                manifest.entries.push_back(entry);
            }
        }
    }
    return manifest;
}

static bool WriteManifest(const Manifest &manifest) {
    std::string text = "# zephium adblock manifest v1\n";
    char line[256];
    snprintf(line, sizeof(line), "refreshed %llu\n",
             (unsigned long long)manifest.refreshed_unix);
    text += line;
    for (const ManifestEntry &entry : manifest.entries) {
        snprintf(line, sizeof(line), "list %s %llu %016llx\n", entry.id.c_str(),
                 (unsigned long long)entry.bytes, (unsigned long long)entry.hash);
        text += line;
    }
    return WriteFileAtomic(ManifestPath(), text.data(), text.size());
}

/* Fast cache-validity check: the manifest is written after the list files, so a
 * size match means "these are the bytes the cache was built from".  The refresh
 * thread still compares full hashes when it downloads new lists. */
static bool ManifestMatchesDisk(const Manifest &manifest) {
    if (!manifest.valid) return false;
    size_t checked = 0;
    const size_t count = sizeof(kFilterLists) / sizeof(kFilterLists[0]);
    for (size_t i = 0; i < count; ++i) {
        if (!kFilterLists[i].enabled_by_default) continue;
        uint64_t size = 0;
        if (!FileSizeW(JoinPath(ListsDirectory(), kFilterLists[i].filename), &size) || size == 0) {
            return false;
        }
        bool found = false;
        for (const ManifestEntry &entry : manifest.entries) {
            if (entry.id != kFilterLists[i].id) continue;
            if (entry.bytes != size) return false;
            found = true;
            break;
        }
        if (!found) return false;
        ++checked;
    }
    return checked > 0;
}

static void WriteManifestFromDisk(uint64_t refreshed_unix) {
    Manifest manifest;
    manifest.valid = true;
    manifest.refreshed_unix = refreshed_unix;
    const size_t count = sizeof(kFilterLists) / sizeof(kFilterLists[0]);
    for (size_t i = 0; i < count; ++i) {
        if (!kFilterLists[i].enabled_by_default) continue;
        uint64_t size = 0;
        uint64_t hash = 0;
        if (!HashFile(JoinPath(ListsDirectory(), kFilterLists[i].filename), &size, &hash)) continue;
        ManifestEntry entry;
        entry.id = kFilterLists[i].id;
        entry.bytes = size;
        entry.hash = hash;
        manifest.entries.push_back(entry);
    }
    if (manifest.entries.empty()) return;
    if (!WriteManifest(manifest)) {
        AdblockLog(L"adblock: could not write %ls; the engine will be rebuilt next start",
                   ManifestPath().c_str());
    }
}

/* --------------------------------------------------------- WinHTTP download */
static bool HttpGet(const wchar_t *url, size_t max_bytes, std::vector<unsigned char> *out,
                    std::wstring *error) {
    out->clear();
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    bool ok = false;

    session = WinHttpOpen(L"Zephium/1.0 (adblock filter lists)",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *error = L"WinHttpOpen failed";
        goto done;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);

    {
        URL_COMPONENTS parts = {};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {0};
        wchar_t path[2048] = {0};
        parts.lpszHostName = host;
        parts.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0])) - 1;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0])) - 1;
        if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
            *error = L"invalid list URL";
            goto done;
        }
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        connection = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connection) {
            *error = L"WinHttpConnect failed";
            goto done;
        }
        request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            *error = L"WinHttpOpenRequest failed";
            goto done;
        }
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                         sizeof(redirect_policy));
        WinHttpAddRequestHeaders(request, L"Accept: text/plain,*/*;q=0.5", (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD);
        /* WinHTTP does not decompress for us; ask for the raw bytes. */
        WinHttpAddRequestHeaders(request, L"Accept-Encoding: identity", (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            *error = L"WinHttpSendRequest failed";
            goto done;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            *error = L"WinHttpReceiveResponse failed";
            goto done;
        }
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            *error = L"could not read the HTTP status";
            goto done;
        }
        if (status != 200) {
            wchar_t text[64];
            swprintf(text, 64, L"HTTP %lu", (unsigned long)status);
            *error = text;
            goto done;
        }
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                *error = L"WinHttpQueryDataAvailable failed";
                goto done;
            }
            if (available == 0) break;
            if (out->size() + available > max_bytes) {
                *error = L"list exceeds its size cap";
                goto done;
            }
            const size_t offset = out->size();
            out->resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out->data() + offset, available, &read) || read == 0) {
                *error = L"WinHttpReadData failed";
                goto done;
            }
            out->resize(offset + read);
        }
    }
    if (out->empty()) {
        *error = L"empty response body";
        goto done;
    }
    ok = true;

done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

bool zephium_adblock_download_list(const ZephiumAdblockList &list,
                                   std::vector<unsigned char> *out, std::wstring *error) {
    if (!out) return false;
    std::wstring local_error;
    std::wstring *target = error ? error : &local_error;
    target->clear();
    if (!list.url || !*list.url) {
        *target = L"no URL configured";
        return false;
    }
    const std::wstring url = zephium_adblock_wide(list.url);
    return HttpGet(url.c_str(), list.max_bytes, out, target);
}

/* -------------------------------------------------------------- engine build */
struct LoadedList {
    const ZephiumAdblockList *info = nullptr;
    std::vector<unsigned char> bytes;
};

static std::wstring ListPath(const ZephiumAdblockList &list) {
    return JoinPath(ListsDirectory(), list.filename);
}

/* Reads every enabled list that exists on disk.  Returns false when none is
 * usable (first run, or every download failed). */
static bool ReadEnabledLists(std::vector<LoadedList> *lists, uint64_t *total_bytes) {
    lists->clear();
    *total_bytes = 0;
    const size_t count = sizeof(kFilterLists) / sizeof(kFilterLists[0]);
    for (size_t i = 0; i < count; ++i) {
        if (!kFilterLists[i].enabled_by_default) continue;
        LoadedList loaded;
        loaded.info = &kFilterLists[i];
        if (!ReadFileBytes(ListPath(kFilterLists[i]), kFilterLists[i].max_bytes, &loaded.bytes) ||
            loaded.bytes.empty()) {
            continue;
        }
        *total_bytes += (uint64_t)loaded.bytes.size();
        if (*total_bytes > (uint64_t)ZEPHIUM_ADBLOCK_MAX_TOTAL_LIST_BYTES) {
            AdblockLog(L"adblock: filter lists exceed the total size cap, ignoring the rest");
            break;
        }
        lists->push_back(std::move(loaded));
    }
    return !lists->empty();
}

/* Builds a brand new engine handle from stored lists.  The caller swaps it in on
 * success, so a failed rebuild never takes the working engine offline. */
static zephium_adblock_engine *BuildEngine(const std::vector<LoadedList> &lists,
                                           uint64_t *rules_out, ULONGLONG *build_ms_out,
                                           uint64_t *list_bytes_out) {
    zephium_adblock_engine *engine = zephium_adblock_engine_new(0);
    if (!engine) {
        AdblockLog(L"adblock: engine allocation failed");
        return nullptr;
    }
    std::vector<const char *> texts;
    std::vector<size_t> lengths;
    texts.reserve(lists.size());
    lengths.reserve(lists.size());
    for (const LoadedList &list : lists) {
        texts.push_back((const char *)list.bytes.data());
        lengths.push_back(list.bytes.size());
    }

    const ULONGLONG started = GetTickCount64();
    uint64_t rules = 0;
    const zephium_adblock_status_t status = zephium_adblock_engine_load(
        engine, texts.data(), lengths.data(), texts.size(), &rules);
    const ULONGLONG build_ms = GetTickCount64() - started;
    if (status != ZEPHIUM_ADBLOCK_OK) {
        AdblockLog(L"adblock: engine build failed: %ls", LastErrorText().c_str());
        zephium_adblock_engine_free(engine);
        return nullptr;
    }

    /* Persist the compiled engine so the next start is a warm one.  A failure
     * here only costs a rebuild on the next start. */
    const std::string cache_path = zephium_adblock_utf8(CachePath());
    const ULONGLONG save_started = GetTickCount64();
    const zephium_adblock_status_t saved =
        zephium_adblock_engine_save_cache(engine, cache_path.c_str());
    const ULONGLONG save_ms = GetTickCount64() - save_started;
    if (saved == ZEPHIUM_ADBLOCK_OK) {
        uint64_t cache_bytes = 0;
        FileSizeW(CachePath(), &cache_bytes);
        AdblockLog(L"adblock: engine cache written in %llu ms (%llu KB)", save_ms,
                   cache_bytes / 1024u);
    } else {
        AdblockLog(L"adblock: engine cache could not be written: %ls",
                   LastErrorText().c_str());
    }

    *rules_out = rules;
    *build_ms_out = build_ms;
    *list_bytes_out = 0;
    for (const LoadedList &list : lists) *list_bytes_out += (uint64_t)list.bytes.size();
    return engine;
}

struct RetiredStats {
    uint64_t checks = 0;
    uint64_t blocked = 0;
    uint64_t redirected = 0;
    uint64_t rewritten = 0;
    uint64_t total_check_ns = 0;
};
static RetiredStats g_retired;

/* Frees the current engine (UI thread: the handle is thread affine). */
static void RetireEngine() {
    if (!g_engine) return;
    zephium_adblock_stats_t stats = {};
    if (zephium_adblock_engine_stats(g_engine, &stats) == ZEPHIUM_ADBLOCK_OK) {
        g_retired.checks += stats.checks;
        g_retired.blocked += stats.blocked;
        g_retired.redirected += stats.redirected;
        g_retired.rewritten += stats.rewritten;
        g_retired.total_check_ns += stats.total_check_ns;
    }
    zephium_adblock_engine_free(g_engine);
    g_engine = nullptr;
    g_engine_ready = false;
}

/* Swaps in a freshly built engine and frees the old one after reading its
 * counters, so the cumulative stats survive a daily refresh. */
static void InstallEngine(zephium_adblock_engine *engine) {
    RetireEngine();
    g_engine = engine;
    g_engine_ready = engine != nullptr;
}

static void LogEngineSummary(const wchar_t *what) {
    if (!g_engine) return;
    zephium_adblock_stats_t stats = {};
    if (zephium_adblock_engine_stats(g_engine, &stats) != ZEPHIUM_ADBLOCK_OK) return;
    AdblockLog(L"adblock: %ls: %llu rules, %llu bytes of filter lists, ABI %u", what,
               stats.rule_count, stats.list_bytes, zephium_adblock_abi_version());
}

/* --------------------------------------------------------- refresh thread */
static volatile LONG g_worker_running = 0;
static HANDLE g_worker_thread = nullptr;
static HANDLE g_stop_event = nullptr;
/* Set when a refresh could not store every list (read-only directory, network
 * error).  Without it the 60 s due-check would re-download megabytes over and
 * over while the failure persists. */
static volatile ULONGLONG g_refresh_retry_after_ms = 0;

struct WorkerContext {
    HWND hwnd = nullptr;
    HANDLE stop = nullptr;
};

static DWORD WINAPI RefreshThreadProc(void *parameter) {
    WorkerContext *context = (WorkerContext *)parameter;
    const ULONGLONG started = GetTickCount64();

    Manifest manifest;
    manifest.valid = true;
    manifest.refreshed_unix = NowUnixSeconds();

    size_t downloaded = 0;
    size_t changed = 0;
    size_t failed = 0;
    uint64_t downloaded_bytes = 0;

    const size_t count = sizeof(kFilterLists) / sizeof(kFilterLists[0]);
    for (size_t i = 0; i < count; ++i) {
        const ZephiumAdblockList &list = kFilterLists[i];
        if (!list.enabled_by_default) continue;
        if (WaitForSingleObject(context->stop, 0) == WAIT_OBJECT_0) break;

        const std::wstring path = ListPath(list);
        uint64_t old_size = 0;
        uint64_t old_hash = 0;
        const bool have_old = HashFile(path, &old_size, &old_hash);

        std::vector<unsigned char> bytes;
        std::wstring error;
        if (!zephium_adblock_download_list(list, &bytes, &error)) {
            ++failed;
            AdblockLog(L"adblock: download failed for %ls: %ls", list.filename,
                       error.empty() ? L"(no detail)" : error.c_str());
            if (have_old) {
                ManifestEntry entry;
                entry.id = list.id;
                entry.bytes = old_size;
                entry.hash = old_hash;
                manifest.entries.push_back(entry);
            }
            continue;
        }
        ++downloaded;
        downloaded_bytes += (uint64_t)bytes.size();
        const uint64_t hash = Fnv1a(bytes.data(), bytes.size());
        if (!have_old || old_size != (uint64_t)bytes.size() || old_hash != hash) {
            if (!WriteFileAtomic(path, bytes.data(), bytes.size())) {
                ++failed;
                AdblockLog(L"adblock: could not write %ls", list.filename);
                if (have_old) {
                    ManifestEntry entry;
                    entry.id = list.id;
                    entry.bytes = old_size;
                    entry.hash = old_hash;
                    manifest.entries.push_back(entry);
                }
                continue;
            }
            ++changed;
        }
        ManifestEntry entry;
        entry.id = list.id;
        entry.bytes = (uint64_t)bytes.size();
        entry.hash = hash;
        manifest.entries.push_back(entry);
    }

    const bool manifest_written = WriteManifest(manifest);
    if (!manifest_written) {
        AdblockLog(L"adblock: could not write the list manifest");
    }
    if (failed > 0 || !manifest_written) {
        /* Back off for 30 minutes instead of retrying every due-check. */
        g_refresh_retry_after_ms = GetTickCount64() + 30u * 60u * 1000u;
    } else {
        g_refresh_retry_after_ms = 0;
    }
    AdblockLog(L"adblock: refresh finished in %llu ms: %zu/%zu lists downloaded (%llu KB), "
               L"%zu changed, %zu failed%s",
               GetTickCount64() - started, downloaded, EnabledListCount(),
               downloaded_bytes / 1024u, changed, failed,
               (failed > 0 || !manifest_written) ? L" (retrying in 30 min)" : L"");

    /* Always tell the UI thread: it updates the "last refresh" clock even when
     * nothing changed, and only rebuilds the engine when list files did. */
    if (context->hwnd) {
        PostMessageW(context->hwnd, ZEPHIUM_ADBLOCK_WM_REFRESH, (WPARAM)changed, 0);
    }
    InterlockedExchange(&g_worker_running, 0);
    delete context;
    return 0;
}

static void StartRefreshWorker() {
    if (!g_enabled || !g_hwnd) return;
    if (InterlockedCompareExchange(&g_worker_running, 1, 0) != 0) return;  /* already running */
    if (!g_stop_event) g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        InterlockedExchange(&g_worker_running, 0);
        AdblockLog(L"adblock: could not create the refresh stop event");
        return;
    }
    ResetEvent(g_stop_event);
    WorkerContext *context = new WorkerContext();
    context->hwnd = g_hwnd;
    context->stop = g_stop_event;
    HANDLE thread = CreateThread(nullptr, 0, RefreshThreadProc, context, 0, nullptr);
    if (!thread) {
        delete context;
        InterlockedExchange(&g_worker_running, 0);
        AdblockLog(L"adblock: could not start the list refresh thread");
        return;
    }
    if (g_worker_thread) CloseHandle(g_worker_thread);  /* previous thread has finished */
    g_worker_thread = thread;
    AdblockLog(L"adblock: downloading %zu filter list(s) in the background", EnabledListCount());
}

/* Starts a download unless the lists were refreshed less than 24h ago. */
static void MaybeStartRefresh(bool force) {
    if (!g_enabled || !g_hwnd) return;
    if (InterlockedCompareExchange(&g_worker_running, 0, 0) != 0) return;
    if (!force && GetTickCount64() < g_refresh_retry_after_ms) return;
    if (!force && g_last_refresh_unix != 0) {
        const uint64_t now = NowUnixSeconds();
        if (now >= g_last_refresh_unix && now - g_last_refresh_unix < 24u * 3600u) return;
    }
    StartRefreshWorker();
}

/* ---------------------------------------------------------------- stats log */
static void LogStats(bool force) {
    if (!g_engine) {
        if (force) AdblockLog(L"adblock final stats: no engine was loaded");
        return;
    }
    zephium_adblock_stats_t current = {};
    if (zephium_adblock_engine_stats(g_engine, &current) != ZEPHIUM_ADBLOCK_OK) return;
    const uint64_t checks = g_retired.checks + current.checks;
    const uint64_t blocked = g_retired.blocked + current.blocked;
    const uint64_t redirected = g_retired.redirected + current.redirected;
    const uint64_t rewritten = g_retired.rewritten + current.rewritten;
    const uint64_t total_ns = g_retired.total_check_ns + current.total_check_ns;
    if (!force && !g_engine_ready) return;
    if (!force && checks == 0 && blocked == 0 && redirected == 0 && rewritten == 0) return;
    /* Only log when the numbers actually moved. */
    static uint64_t last_checks = 0;
    static uint64_t last_blocked = 0;
    static uint64_t last_redirected = 0;
    static uint64_t last_rewritten = 0;
    if (!force && checks == last_checks && blocked == last_blocked &&
        redirected == last_redirected && rewritten == last_rewritten) {
        return;
    }
    last_checks = checks;
    last_blocked = blocked;
    last_redirected = redirected;
    last_rewritten = rewritten;
    const uint64_t average_ns = checks ? total_ns / checks : 0;
    AdblockLog(L"adblock stats: checks=%llu blocked=%llu redirected=%llu rewritten=%llu "
               L"panics=%llu avg=%llu ns",
               (unsigned long long)checks, (unsigned long long)blocked,
               (unsigned long long)redirected, (unsigned long long)rewritten,
               (unsigned long long)current.panics, (unsigned long long)average_ns);
}

/* --------------------------------------------------------------- public API */
std::wstring zephium_adblock_root_dir(const std::wstring &exe_dir) {
    wchar_t buffer[1024] = {0};
    const DWORD capacity = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
    DWORD length = GetEnvironmentVariableW(L"ZEPHIUM_ADBLOCK_DIR", buffer, capacity);
    if (length > 0 && length < capacity) {
        std::wstring result(buffer);
        while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) {
            result.pop_back();
        }
        if (!result.empty()) return result;
    }
    /* Production location: %LOCALAPPDATA%\Zephium\adblock.  The exe directory may
     * be read-only (Program Files) while the lists and the ~7 MB engine cache must
     * be writable.  Only when LOCALAPPDATA is missing (never on a normal Windows
     * login) fall back to a directory next to the exe. */
    length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, capacity);
    if (length > 0 && length < capacity) {
        std::wstring result(buffer);
        while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) {
            result.pop_back();
        }
        if (!result.empty()) return result + L"\\Zephium\\adblock";
    }
    if (exe_dir.empty()) return std::wstring(L"zephium_adblock");
    return JoinPath(exe_dir, L"zephium_adblock");
}

/* Creating a directory is not proof that files can be written inside it: a
 * profile can exist yet be denied to the current token (sandboxed processes,
 * mandatory integrity labels).  Probe with a temporary file that deletes itself
 * when closed. */
static bool DirectoryWritable(const std::wstring &path, DWORD *error_out) {
    if (error_out) *error_out = ERROR_SUCCESS;
    if (path.empty()) {
        if (error_out) *error_out = ERROR_INVALID_PARAMETER;
        return false;
    }
    const std::wstring probe = JoinPath(path, L".zephium-write-test");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error_out) *error_out = GetLastError();
        return false;
    }
    CloseHandle(file);
    return true;
}

/* Picks a writable data directory: the configured/preferred one first, then one
 * next to the exe.  A locked-down profile (or a process running under a sandbox
 * that only allows writes inside its workspace) used to make ad blocking
 * silently impossible - every list write failed while the feature still claimed
 * to be enabled.  The fallback keeps it working, and the log names both the
 * failure and the directory actually used. */
static bool PrepareRootDir(const std::wstring &exe_dir, const std::wstring &preferred,
                           std::wstring *chosen, std::wstring *detail) {
    std::vector<std::wstring> candidates;
    candidates.push_back(preferred);
    if (!exe_dir.empty()) {
        const std::wstring beside_exe = JoinPath(exe_dir, L"zephium_adblock");
        if (beside_exe != preferred) candidates.push_back(beside_exe);
    }
    for (const std::wstring &candidate : candidates) {
        DWORD error = ERROR_SUCCESS;
        if (EnsureDirectoryTreeEx(candidate, &error) && DirectoryWritable(candidate, &error)) {
            *chosen = candidate;
            return true;
        }
        if (!detail->empty()) *detail += L"; ";
        *detail += candidate + L" (error " + std::to_wstring((unsigned long)error) + L")";
    }
    return false;
}

bool zephium_adblock_start(const ZephiumAdblockConfig &config) {
    if (g_started) return g_enabled;
    g_started = true;
    g_logger = config.logger;
    g_hwnd = config.hwnd;
    g_exe_dir = config.exe_dir;
    if (!config.enabled) {
        /* The shell already logged "ad blocking: disabled (...)" with its reason. */
        return false;
    }
    if (zephium_adblock_abi_version() != ZEPHIUM_ADBLOCK_ABI_VERSION) {
        AdblockLog(L"adblock: engine ABI %u does not match the shell (header %u); disabled",
                   zephium_adblock_abi_version(), ZEPHIUM_ADBLOCK_ABI_VERSION);
        return false;
    }

    const std::wstring preferred_root = zephium_adblock_root_dir(config.exe_dir);
    g_root = preferred_root;
    {
        std::wstring chosen;
        std::wstring detail;
        if (PrepareRootDir(config.exe_dir, preferred_root, &chosen, &detail)) {
            g_root = chosen;
            if (chosen != preferred_root) {
                AdblockLog(L"adblock: %ls is not writable (%ls); storing filter lists in %ls "
                           L"instead",
                           preferred_root.c_str(), detail.c_str(), chosen.c_str());
            }
        } else {
            AdblockLog(L"adblock: no writable data directory (%ls); filter lists cannot be "
                       L"stored, only an existing engine cache can be loaded",
                       detail.c_str());
        }
    }
    EnsureDirectory(ListsDirectory());

    g_engine = zephium_adblock_engine_new(0);
    if (!g_engine) {
        AdblockLog(L"adblock: engine allocation failed; browsing continues unfiltered");
        return false;
    }
    g_enabled = true;
    AdblockLog(L"adblock: %ls (ABI %u), data dir %ls",
               zephium_adblock_wide(zephium_adblock_version()).c_str(),
               zephium_adblock_abi_version(), g_root.c_str());
    AdblockLog(L"adblock: %zu list(s) in the default set, cache %ls", EnabledListCount(),
               CachePath().c_str());

    const Manifest manifest = ReadManifest();
    if (FileExistsW(CachePath()) && ManifestMatchesDisk(manifest)) {
        const ULONGLONG started = GetTickCount64();
        const zephium_adblock_status_t status =
            zephium_adblock_engine_load_cache(g_engine, zephium_adblock_utf8(CachePath()).c_str());
        const ULONGLONG elapsed_ms = GetTickCount64() - started;
        if (status == ZEPHIUM_ADBLOCK_OK) {
            g_engine_ready = true;
            g_last_refresh_unix = manifest.refreshed_unix;
            LogEngineSummary(L"warm start from engine.bin");
            AdblockLog(L"adblock: engine.bin loaded in %llu ms", elapsed_ms);
            MaybeStartRefresh(false);
            return true;
        }
        AdblockLog(L"adblock: engine.bin rejected (%ls); rebuilding from filter lists",
                   LastErrorText().c_str());
    }

    std::vector<LoadedList> lists;
    uint64_t total_bytes = 0;
    if (ReadEnabledLists(&lists, &total_bytes)) {
        uint64_t rules = 0;
        uint64_t list_bytes = 0;
        ULONGLONG build_ms = 0;
        zephium_adblock_engine *built = BuildEngine(lists, &rules, &build_ms, &list_bytes);
        if (built) {
            InstallEngine(built);
            g_last_refresh_unix = manifest.refreshed_unix;
            LogEngineSummary(L"engine built from the cached filter lists");
            AdblockLog(L"adblock: engine built from %zu list(s) in %llu ms", lists.size(),
                       build_ms);
            WriteManifestFromDisk(manifest.refreshed_unix);
            MaybeStartRefresh(manifest.refreshed_unix == 0);
            return true;
        }
        AdblockLog(L"adblock: could not build an engine from the cached lists");
    } else {
        AdblockLog(L"adblock: no cached filter lists in %ls", ListsDirectory().c_str());
    }
    /* Browsing starts now; the engine appears when the download finishes. */
    MaybeStartRefresh(true);
    return true;
}

void zephium_adblock_on_build_message(WPARAM changed_lists) {
    if (!g_enabled) return;
    const Manifest manifest = ReadManifest();
    if (manifest.refreshed_unix != 0) g_last_refresh_unix = manifest.refreshed_unix;
    if (changed_lists == 0) return;

    std::vector<LoadedList> lists;
    uint64_t total_bytes = 0;
    if (!ReadEnabledLists(&lists, &total_bytes)) {
        AdblockLog(L"adblock: refreshed lists are unreadable; keeping the current engine");
        return;
    }
    uint64_t rules = 0;
    uint64_t list_bytes = 0;
    ULONGLONG build_ms = 0;
    zephium_adblock_engine *built = BuildEngine(lists, &rules, &build_ms, &list_bytes);
    if (!built) {
        AdblockLog(L"adblock: rebuild after a list refresh failed; keeping the previous engine");
        return;
    }
    InstallEngine(built);
    LogEngineSummary(L"engine rebuilt from the refreshed lists");
    AdblockLog(L"adblock: engine rebuilt in %llu ms from %zu list(s)", build_ms, lists.size());
}

void zephium_adblock_tick() {
    if (!g_enabled) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_due_check_ms >= 60000) {
        g_last_due_check_ms = now;
        MaybeStartRefresh(false);
    }
    if (now - g_last_stats_ms >= 10000) {
        g_last_stats_ms = now;
        LogStats(false);
    }
}

void zephium_adblock_shutdown() {
    if (!g_started) return;
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_worker_thread) {
        const DWORD wait = WaitForSingleObject(g_worker_thread, 15000);
        if (wait == WAIT_TIMEOUT) {
            AdblockLog(L"adblock: download thread still running at exit; detaching");
        } else {
            CloseHandle(g_worker_thread);
        }
        g_worker_thread = nullptr;
    }
    LogStats(true);
    if (g_engine) {
        RetireEngine();
        AdblockLog(L"adblock: engine freed");
    }
    g_enabled = false;
    g_started = false;
}

bool zephium_adblock_is_enabled() { return g_enabled; }
bool zephium_adblock_is_ready() { return g_enabled && g_engine_ready && g_engine != nullptr; }

zephium_adblock_status_t zephium_adblock_check_url(const std::string &url,
                                                   const std::string &source_url,
                                                   zephium_adblock_resource_type_t type,
                                                   const std::string &method,
                                                   zephium_adblock_result_t *out) {
    if (out) {
        zephium_adblock_result_t allow = {};
        allow.verdict = ZEPHIUM_ADBLOCK_ALLOW;
        *out = allow;
    }
    if (!g_engine || !g_engine_ready) return ZEPHIUM_ADBLOCK_ERR_STATE;
    const char *method_text = method.empty() ? nullptr : method.c_str();
    return zephium_adblock_check(g_engine, url.c_str(), source_url.c_str(), type, method_text, out);
}
