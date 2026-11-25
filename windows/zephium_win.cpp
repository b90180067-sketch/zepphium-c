/* =============================================================================
 * Zephium Browser - native Windows shell (Win32 + WebView2 / Edge Chromium)
 *
 * Multi-tab shell on top of the portable C17 core. Every tab owns one
 * WebView2 controller; tabs that stay inactive are hibernated (their webview is
 * destroyed and only URL/title/zoom survive) so idle tabs stop costing memory,
 * the way the original Rust build's sleeping tabs do.
 * ========================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sddl.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

#include "WebView2.h"

#include "adblock_webview2.h"

extern "C" {
#include "../browser_core/port.h"
}

/* The ad-block helpers mirror the WebView2 resource-context enum by number so
 * the console self test can include adblock_webview2.h without WebView2/COM.
 * If a future SDK renumbers the enum this fails at compile time instead of
 * silently mis-classifying requests. */
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL == ZEPHIUM_W2_CONTEXT_ALL,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT == ZEPHIUM_W2_CONTEXT_DOCUMENT,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET == ZEPHIUM_W2_CONTEXT_STYLESHEET,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE == ZEPHIUM_W2_CONTEXT_IMAGE,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MEDIA == ZEPHIUM_W2_CONTEXT_MEDIA,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT == ZEPHIUM_W2_CONTEXT_FONT,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT == ZEPHIUM_W2_CONTEXT_SCRIPT,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST ==
                  ZEPHIUM_W2_CONTEXT_XMLHTTPREQUEST,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH == ZEPHIUM_W2_CONTEXT_FETCH,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_TEXT_TRACK == ZEPHIUM_W2_CONTEXT_TEXTTRACK,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_EVENT_SOURCE ==
                  ZEPHIUM_W2_CONTEXT_EVENTSOURCE,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_WEBSOCKET == ZEPHIUM_W2_CONTEXT_WEBSOCKET,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MANIFEST == ZEPHIUM_W2_CONTEXT_MANIFEST,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SIGNED_EXCHANGE ==
                  ZEPHIUM_W2_CONTEXT_SIGNEDEXCHANGE,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_PING == ZEPHIUM_W2_CONTEXT_PING,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_CSP_VIOLATION_REPORT ==
                  ZEPHIUM_W2_CONTEXT_CSP_VIOLATION_REPORT,
              "WebView2 resource context enum drifted");
static_assert((int)COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER == ZEPHIUM_W2_CONTEXT_OTHER,
              "WebView2 resource context enum drifted");

/* ---------------------------------------------------------------- constants */
#define ZEPHIUM_WINDOW_CLASS L"ZephiumWin32TabbedWindow"
#define ZEPHIUM_TABBAR_HEIGHT 32
#define ZEPHIUM_TOOLBAR_HEIGHT 42
#define ZEPHIUM_NEWTAB_HOST L"newtab.zephium"
#define ZEPHIUM_NEWTAB_URL L"https://newtab.zephium/index.html"
#define ZEPHIUM_TIMER_ID 1
#define ZEPHIUM_TIMER_MS 400

#define ID_BACK 1001
#define ID_FORWARD 1002
#define ID_RELOAD 1003
#define ID_URL 1004
#define ID_ZOOM_OUT 1005
#define ID_ZOOM_IN 1006
#define ID_ZOOM_LABEL 1007
#define ID_THEME 1008
#define ID_NEW_TAB 1010
#define ID_CLOSE_TAB 1011
#define ID_NEXT_TAB 1012
#define ID_PREV_TAB 1013

/* Chromium/Edge flags, modelled on the original Rust Zephium (Tauri/wry). */
#define ZEPHIUM_WEBVIEW2_ARGS \
    L"--autoplay-policy=no-user-gesture-required " \
    L"--disable-features=msWebOOUI,msPdfOOUI,SpareRendererForSitePerProcess," \
    L"msEdgeSidebarV2,msEdgeAutofill,msEdgeDiscover,msEdgeShoppingAssistant," \
    L"msEdgeWebCapture,msEdgeCollections " \
    L"--disable-background-networking --disable-component-update " \
    L"--no-first-run --no-default-browser-check"

/* ------------------------------------------------------------------- types */
struct ZephiumTab {
    int id = 0;
    ICoreWebView2Controller *ctrl = nullptr;
    ICoreWebView2 *view = nullptr;
    std::wstring url = L"about:blank";
    std::wstring title = L"New Tab";
    bool sleeping = false;
    bool creating = false;
    ULONGLONG last_active = 0;
    /* Ad-block state for this tab: the current document URL and the bounded set
     * of iframe URLs (WebView2 reports iframe navigations as DOCUMENT). */
    ZephiumAdblockFrameTracker adblock;
    /* Set when the shell itself changes the tab URL (start page); the
     * SourceChanged handler must not overwrite the pending navigation. */
    ULONGLONG suppress_source_until = 0;
};

struct TabGeometry {
    std::vector<RECT> tabs;
    std::vector<RECT> closes;
    RECT plus = {0, 0, 0, 0};
};

/* ------------------------------------------------------------------ globals */
static HWND g_hwnd = nullptr;
static HWND g_url = nullptr;
static HWND g_back = nullptr;
static HWND g_forward = nullptr;
static HWND g_reload = nullptr;
static HWND g_zoom_out = nullptr;
static HWND g_zoom_in = nullptr;
static HWND g_zoom_label = nullptr;
static HWND g_theme_btn = nullptr;

static HFONT g_tab_font = nullptr;
static HFONT g_url_font = nullptr;
static HFONT g_icon_font = nullptr;
static HBRUSH g_edit_brush = nullptr;
static HBRUSH g_chrome_brush = nullptr;

/* One palette holds every colour the chrome paint path reads.  Dark is the
 * default; the Frutiger Aero palette is the optional second instance. */
struct ChromePalette {
    COLORREF bg;
    COLORREF bg_bottom;
    COLORREF toolbar;
    COLORREF toolbar_bottom;
    COLORREF field;
    COLORREF line;
    COLORREF ink;
    COLORREF muted;
    COLORREF accent;
    COLORREF tab_active;
    COLORREF tab_hover;
    COLORREF tab_idle;
    COLORREF button;
    COLORREF button_pressed;
};

static const ChromePalette kDarkPalette = {
    RGB(10, 14, 19),   /* bg */
    RGB(10, 14, 19),   /* bg_bottom */
    RGB(11, 16, 22),   /* toolbar */
    RGB(11, 16, 22),   /* toolbar_bottom */
    RGB(13, 20, 28),   /* field */
    RGB(31, 43, 55),   /* line */
    RGB(232, 238, 245),/* ink */
    RGB(139, 152, 166),/* muted */
    RGB(124, 232, 196),/* accent */
    RGB(24, 33, 44),   /* tab active */
    RGB(18, 25, 34),   /* tab hover */
    RGB(13, 19, 26),   /* tab idle */
    RGB(15, 22, 30),   /* button */
    RGB(28, 39, 51)    /* button pressed */
};

static const ChromePalette kAeroPalette = {
    RGB(46, 158, 224), /* bg */
    RGB(125, 206, 245),/* bg_bottom */
    RGB(186, 230, 252),/* toolbar */
    RGB(92, 176, 230), /* toolbar_bottom */
    RGB(255, 255, 255),/* field */
    RGB(255, 255, 255),/* line */
    RGB(14, 58, 102),  /* ink */
    RGB(255, 255, 255),/* muted */
    RGB(255, 255, 255),/* accent */
    RGB(236, 248, 255),/* tab active */
    RGB(186, 224, 244),/* tab hover */
    RGB(120, 190, 230),/* tab idle */
    RGB(230, 246, 255),/* button */
    RGB(176, 220, 242) /* button pressed */
};

enum ChromeTheme { kThemeDark = 0, kThemeAero = 1 };

static ChromeTheme g_theme = kThemeDark;

static const ChromePalette &Chrome() {
    return g_theme == kThemeAero ? kAeroPalette : kDarkPalette;
}

static bool AeroTheme() {
    return g_theme == kThemeAero;
}

static ICoreWebView2Environment *g_env = nullptr;
static zephium_browser_context_t *g_core = nullptr;

static std::vector<ZephiumTab *> g_tabs;
static int g_active = -1;
static int g_next_tab_id = 0;
static TabGeometry g_geo;
static POINT g_mouse = {-1, -1};

static int g_zoom_percent = 100;
static int g_sleep_seconds = 600;          /* 0 disables hibernation */
static bool g_adblock_enabled = true;      /* --no-adblock / ZEPHIUM_ADBLOCK=0 */
static std::wstring g_initial_url;
static std::wstring g_exe_dir;
static std::wstring g_log_path;
static std::wstring g_newtab_dir;          /* folder mapped to newtab.zephium */
static std::wstring g_newtab_url;
static bool g_newtab_mode = false;         /* the page is the full-bleed start page */

/* -------------------------------------------------------------------- utils */
static void Log(const wchar_t *fmt, ...) {
    if (g_log_path.empty()) return;
    FILE *f = _wfopen(g_log_path.c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

/* The ad-block subsystem formats its own lines (it also logs from the download
 * thread); route them through the shell log so every line is timestamped and
 * lands in the same file. */
static void AdblockLogLine(const wchar_t *line) {
    Log(L"%ls", line);
}

/* --------------------------------------------------------------- theme file */
/* The persisted theme is one UTF-8 line in
 * %LOCALAPPDATA%\Zephium\theme.txt.  Every failure path falls back to dark so a
 * missing, empty, unknown or unreadable file can never stop the shell starting. */
static bool ThemeDirectory(std::wstring *dir) {
    wchar_t local[32768] = {0};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (len == 0 || len >= 32768 || local[0] == L'\0') return false;
    *dir = std::wstring(local) + L"\\Zephium";
    return true;
}

static ChromeTheme LoadTheme() {
    std::wstring dir;
    if (!ThemeDirectory(&dir)) {
        Log(L"theme: LOCALAPPDATA unavailable; starting dark");
        return kThemeDark;
    }
    const std::wstring path = dir + L"\\theme.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            Log(L"theme: no theme.txt; using dark");
        } else {
            Log(L"theme: could not read %ls (error %lu); using dark", path.c_str(), error);
        }
        return kThemeDark;
    }

    char buffer[64] = {0};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (!ok) {
        Log(L"theme: read failed for %ls (error %lu); using dark", path.c_str(), error);
        return kThemeDark;
    }

    std::string value(buffer, (size_t)read);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (value == "aero") {
        Log(L"theme: theme.txt selects aero");
        return kThemeAero;
    }
    if (value == "dark") {
        Log(L"theme: theme.txt selects dark");
        return kThemeDark;
    }
    Log(L"theme: theme.txt is empty or unknown; using dark");
    return kThemeDark;
}

/* Write through a sibling temporary file and replace atomically: a failed write
 * must leave the previous theme.txt exactly as it was. */
static bool SaveTheme(ChromeTheme theme) {
    std::wstring dir;
    if (!ThemeDirectory(&dir)) {
        Log(L"theme: LOCALAPPDATA unavailable; theme not persisted");
        return false;
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            Log(L"theme: could not create %ls (error %lu); theme not persisted", dir.c_str(),
                error);
            return false;
        }
    }

    const std::wstring path = dir + L"\\theme.txt";
    const std::wstring temporary = path + L".tmp";
    const std::string text = theme == kThemeAero ? "aero" : "dark";

    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log(L"theme: could not open %ls for writing (error %lu); theme not persisted",
            temporary.c_str(), GetLastError());
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
    DWORD error = ok ? 0 : GetLastError();
    if (ok) {
        ok = FlushFileBuffers(file);
        if (!ok) error = GetLastError();
    }
    CloseHandle(file);

    if (!ok || written != (DWORD)text.size()) {
        Log(L"theme: write failed for %ls (error %lu); previous theme.txt kept", path.c_str(),
            error);
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log(L"theme: replace failed for %ls (error %lu); previous theme.txt kept", path.c_str(),
            GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    Log(L"theme: saved %ls", theme == kThemeAero ? L"aero" : L"dark");
    return true;
}

static std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const char *s) {
    if (!s || !*s) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], len);
    return out;
}

/* The log records every page the user visited, so URLs are written without their
 * query string and fragment: those carry session tokens, signed CDN parameters and
 * search terms. The portable C helper decides what "redacted" means. */
static std::wstring RedactUrl(const std::wstring &url) {
    std::string utf8 = WideToUtf8(url);
    char safe[ZEPHIUM_MAX_URL_BYTES];
    if (!zephium_nav_redact_url(utf8.c_str(), utf8.size(), safe, sizeof(safe))) {
        return std::wstring(L"(unprintable)");
    }
    return Utf8ToWide(safe);
}

/* Windows has no POSIX modes, so the equivalent of "mode 0600" is an explicit DACL:
 * SYSTEM and the file owner, nobody else. Best effort - failing to tighten the ACL
 * must never stop the browser from starting. */
static void RestrictFileToOwner(const std::wstring &path) {
    if (path.empty()) return;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) {
        return;
    }
    (void)SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
}

static std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

static bool FileExistsW(const std::wstring &path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring FullPathW(const std::wstring &path) {
    wchar_t buffer[MAX_PATH] = {0};
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buffer, n) : path;
}

static bool IsNewTabUrl(const std::wstring &url) {
    if (g_newtab_url.empty()) return false;
    if (url == g_newtab_url) return true;
    return url.rfind(g_newtab_url + L"?", 0) == 0 ||
           url.rfind(g_newtab_url + L"#", 0) == 0;
}

/* A tab can be showing the start page before the 400 ms timer has copied the
 * committed URL into tab->url, so consult the live view as well. */
static bool TabShowsNewTabPage(ZephiumTab *tab) {
    if (!tab) return false;
    if (IsNewTabUrl(tab->url)) return true;
    if (!tab->view) return false;
    LPWSTR source = nullptr;
    if (FAILED(tab->view->get_Source(&source)) || !source) return false;
    const bool matches = IsNewTabUrl(source);
    CoTaskMemFree(source);
    return matches;
}

/* The page's zephiumSetTheme() only flips data-theme and pauses/resumes the
 * ASCII loop; it never navigates or calls back into the shell. */
static void SendThemeToView(ZephiumTab *tab) {
    if (!tab || !tab->view) return;
    const wchar_t *script = AeroTheme() ? L"window.zephiumSetTheme(\"aero\");"
                                        : L"window.zephiumSetTheme(\"dark\");";
    const HRESULT hr = tab->view->ExecuteScript(script, nullptr);
    if (FAILED(hr)) {
        Log(L"theme: ExecuteScript failed for tab id=%d (0x%08lx)", tab->id,
            (unsigned long)hr);
    }
}

/* Every open start-page view follows a theme click immediately, including
 * views in inactive tabs; a view that is still navigating gets it again from
 * its NavigationCompleted handler. */
static void PushThemeToNewTabViews() {
    for (ZephiumTab *tab : g_tabs) {
        if (tab && tab->view && TabShowsNewTabPage(tab)) SendThemeToView(tab);
    }
}

static std::wstring NewTabUrl() {
    return g_newtab_url.empty() ? std::wstring(L"about:blank") : g_newtab_url;
}

/* The DWM caption and border follow the active palette's bg. dwmapi is loaded
 * on demand so the shell still runs on systems without it. */
static void SetTitleBarColors() {
    if (!g_hwnd) return;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI *DwmSetAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    FARPROC proc = GetProcAddress(dwm, "DwmSetWindowAttribute");
    DwmSetAttributeFn set_attribute = nullptr;
    memcpy(&set_attribute, &proc, sizeof(set_attribute));
    if (set_attribute) {
        BOOL enabled = g_theme == kThemeDark ? TRUE : FALSE;
        set_attribute(g_hwnd, 20, &enabled, sizeof(enabled)); /* Win11 */
        set_attribute(g_hwnd, 19, &enabled, sizeof(enabled)); /* Win10 20H1+ */
        COLORREF caption = Chrome().bg;
        set_attribute(g_hwnd, 35, &caption, sizeof(caption)); /* caption colour */
        COLORREF border = Chrome().bg;
        set_attribute(g_hwnd, 34, &border, sizeof(border));   /* border colour */
    }
    FreeLibrary(dwm);
}

/* Re-derive the brushes the controls pull their background from, then repaint
 * the whole chrome (owner-draw buttons and the address field included). */
static void ApplyThemeToChrome() {
    if (g_edit_brush) {
        DeleteObject(g_edit_brush);
        g_edit_brush = nullptr;
    }
    if (g_chrome_brush) {
        DeleteObject(g_chrome_brush);
        g_chrome_brush = nullptr;
    }
    g_edit_brush = CreateSolidBrush(Chrome().field);
    g_chrome_brush = CreateSolidBrush(Chrome().toolbar);

    SetTitleBarColors();
    if (g_hwnd) {
        RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
        HWND controls[] = {g_back,      g_forward,   g_reload,     g_url,
                           g_theme_btn, g_zoom_out,  g_zoom_label, g_zoom_in};
        for (HWND control : controls) {
            if (control && IsWindowVisible(control)) {
                RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
            }
        }
    }
}

/* One click swaps the palettes, persists the choice and lets every open start
 * page know. URLs, zoom, hibernation and ad blocking are untouched. */
static void ToggleTheme() {
    g_theme = AeroTheme() ? kThemeDark : kThemeAero;
    ApplyThemeToChrome();
    SaveTheme(g_theme);
    PushThemeToNewTabViews();
    Log(L"theme: switched to %ls", AeroTheme() ? L"aero" : L"dark");
}

static RECT ContentRect() {
    RECT rc = {0, 0, 0, 0};
    if (!g_hwnd) return rc;
    GetClientRect(g_hwnd, &rc);
    rc.top = ZEPHIUM_TABBAR_HEIGHT + (g_newtab_mode ? 0 : ZEPHIUM_TOOLBAR_HEIGHT);
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}

/* ------------------------------------------------------------- COM helpers */
static const GUID kIID_IUnknown_ = {0x00000000, 0x0000, 0x0000,
                                    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_EnvOptions = {0x2fde08a8, 0x1e9a, 0x4766,
                                     {0x8c, 0x05, 0x95, 0xa9, 0xce, 0xb9, 0xd1, 0xc5}};
static const GUID kIID_EnvCompleted = {0x4e8a3389, 0xc9d8, 0x4bd2,
                                       {0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d}};
static const GUID kIID_CtrlCompleted = {0x6c4819f3, 0xc9b7, 0x4260,
                                        {0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c}};
static const GUID kIID_AccelHandler = {0xb29c7e28, 0xfa79, 0x41a8,
                                       {0x8e, 0x44, 0x65, 0x81, 0x1c, 0x76, 0xdc, 0xb2}};
static const GUID kIID_WebView2_8 = {0xe9632730, 0x6e1e, 0x43ab,
                                     {0xb7, 0xb8, 0x7b, 0x2c, 0x9e, 0x62, 0xe0, 0x94}};
/* Start page virtual host (ICoreWebView2_3) and the page -> shell bridge. */
static const GUID kIID_WebView2_3 = {0xA0D6DF20, 0x3B92, 0x416D,
                                     {0xAA, 0x0C, 0x43, 0x7A, 0x9C, 0x72, 0x78, 0x57}};
static const GUID kIID_WebMessage = {0x57213f19, 0x00e6, 0x49fa,
                                     {0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2}};

static bool GuidEq(REFIID a, const GUID &b) {
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

template <typename Iface, const GUID *Iid>
class ZephiumCom : public Iface {
public:
    virtual ~ZephiumCom() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIID_IUnknown_) || GuidEq(riid, *Iid)) {
            *ppv = static_cast<Iface *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

private:
    volatile LONG m_ref = 1;
};

static HRESULT CoStrDup(const std::wstring &src, LPWSTR *out) {
    if (!out) return E_POINTER;
    size_t bytes = (src.size() + 1) * sizeof(wchar_t);
    LPWSTR copy = (LPWSTR)CoTaskMemAlloc(bytes);
    if (!copy) return E_OUTOFMEMORY;
    memcpy(copy, src.c_str(), bytes);
    *out = copy;
    return S_OK;
}

/* Environment options: ICoreWebView2EnvironmentOptions carries the Chromium
 * switches.  It is implemented by hand because the SDK helper class needs WRL,
 * which MinGW does not ship. */
class ZephiumEnvOptions : public ICoreWebView2EnvironmentOptions {
public:
    /* Must match the SDK: an empty TargetCompatibleBrowserVersion makes
     * CreateCoreWebView2EnvironmentWithOptions fail with E_INVALIDARG. */
    static constexpr wchar_t kTargetVersion[] = L"158.0.4370.26";

    explicit ZephiumEnvOptions(const std::wstring &args)
        : m_args(args), m_version(kTargetVersion) {}
    virtual ~ZephiumEnvOptions() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIID_IUnknown_) || GuidEq(riid, kIID_EnvOptions)) {
            *ppv = static_cast<ICoreWebView2EnvironmentOptions *>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR *value) override {
        return CoStrDup(m_args, value);
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
        m_args = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *value) override { return CoStrDup(m_lang, value); }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        m_lang = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR *value) override {
        return CoStrDup(m_version, value);
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
        m_version = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *allow) override {
        if (!allow) return E_POINTER;
        *allow = m_sso;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) override {
        m_sso = allow;
        return S_OK;
    }

private:
    std::wstring m_args;
    std::wstring m_lang;
    std::wstring m_version;
    BOOL m_sso = FALSE;
    volatile LONG m_ref = 1;
};

/* ------------------------------------------------------------- tab helpers */
static ZephiumTab *ActiveTab() {
    return (g_active >= 0 && g_active < (int)g_tabs.size()) ? g_tabs[(size_t)g_active] : nullptr;
}

static ZephiumTab *FindTabById(int id) {
    for (ZephiumTab *t : g_tabs) {
        if (t->id == id) return t;
    }
    return nullptr;
}

static void UpdateTabStrip() {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    RECT strip = {0, 0, rc.right, ZEPHIUM_TABBAR_HEIGHT};
    InvalidateRect(g_hwnd, &strip, TRUE);
}

static void ComputeTabGeometry(int client_width) {
    g_geo.tabs.clear();
    g_geo.closes.clear();

    const int count = (int)g_tabs.size();
    const int left = 6;
    const int plus_w = 30;
    int available = client_width - left * 2 - plus_w;
    int tabw = count > 0 ? available / count : 0;
    tabw = std::max(90, std::min(220, tabw));

    int x = left;
    for (int i = 0; i < count; ++i) {
        RECT r = {x, 4, x + tabw - 2, ZEPHIUM_TABBAR_HEIGHT - 1};
        RECT c = {r.right - 20, r.top + 4, r.right - 4, r.bottom - 4};
        g_geo.tabs.push_back(r);
        g_geo.closes.push_back(c);
        x += tabw;
    }
    g_geo.plus = {x + 4, 5, x + 4 + 24, ZEPHIUM_TABBAR_HEIGHT - 6};
}

static void FillVerticalGradient(HDC dc, const RECT &r, COLORREF top, COLORREF bottom) {
    TRIVERTEX vertex[2];
    vertex[0].x = r.left;
    vertex[0].y = r.top;
    vertex[0].Red = (COLOR16)(GetRValue(top) << 8);
    vertex[0].Green = (COLOR16)(GetGValue(top) << 8);
    vertex[0].Blue = (COLOR16)(GetBValue(top) << 8);
    vertex[0].Alpha = 0;
    vertex[1].x = r.right;
    vertex[1].y = r.bottom;
    vertex[1].Red = (COLOR16)(GetRValue(bottom) << 8);
    vertex[1].Green = (COLOR16)(GetGValue(bottom) << 8);
    vertex[1].Blue = (COLOR16)(GetBValue(bottom) << 8);
    vertex[1].Alpha = 0;
    GRADIENT_RECT mesh = {0, 1};
    if (!GradientFill(dc, vertex, 2, &mesh, 1, GRADIENT_FILL_RECT_V)) {
        HBRUSH fallback = CreateSolidBrush(top);
        FillRect(dc, &r, fallback);
        DeleteObject(fallback);
    }
}

static void FillRoundRect(HDC dc, const RECT &r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void DrawTabStrip(HDC dc, int client_width) {
    ComputeTabGeometry(client_width);
    const ChromePalette &palette = Chrome();
    const bool aero = AeroTheme();

    RECT strip = {0, 0, client_width, ZEPHIUM_TABBAR_HEIGHT};
    if (aero) {
        FillVerticalGradient(dc, strip, palette.bg, palette.bg_bottom);
    } else {
        HBRUSH background = CreateSolidBrush(palette.bg);
        FillRect(dc, &strip, background);
        DeleteObject(background);
    }

    RECT bottom_line = {0, ZEPHIUM_TABBAR_HEIGHT - 1, client_width, ZEPHIUM_TABBAR_HEIGHT};
    HBRUSH line_brush = CreateSolidBrush(palette.line);
    FillRect(dc, &bottom_line, line_brush);
    DeleteObject(line_brush);

    HBRUSH border_brush = CreateSolidBrush(palette.line);

    SetBkMode(dc, TRANSPARENT);
    HFONT font = g_tab_font ? g_tab_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old_font = SelectObject(dc, font);

    for (size_t i = 0; i < g_tabs.size(); ++i) {
        const RECT &r = g_geo.tabs[i];
        const bool active = ((int)i == g_active);
        const bool hovered = PtInRect(&r, g_mouse) != 0;

        COLORREF face = active ? palette.tab_active
                               : (hovered ? palette.tab_hover : palette.tab_idle);
        if (aero) {
            FillRoundRect(dc, r, face, palette.line, r.bottom - r.top);
        } else {
            HBRUSH brush = CreateSolidBrush(face);
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            FrameRect(dc, &r, border_brush);
        }

        /* The dark theme keeps its mint underline; aero signals the active tab
         * with the lightest pill instead. */
        if (active && !aero) {
            RECT accent = {r.left + 9, r.bottom - 2, r.right - 9, r.bottom};
            HBRUSH accent_brush = CreateSolidBrush(palette.accent);
            FillRect(dc, &accent, accent_brush);
            DeleteObject(accent_brush);
        }

        std::wstring label = g_tabs[i]->sleeping ? (L"\u23FE  " + g_tabs[i]->title) : g_tabs[i]->title;
        if (IsNewTabUrl(g_tabs[i]->url)) label = L"Tab mới";
        if (label.empty()) label = L"New Tab";
        RECT text = {r.left + 9, r.top, r.right - 23, r.bottom};
        SetTextColor(dc, active ? palette.ink : palette.muted);
        DrawTextW(dc, label.c_str(), -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        const RECT &close = g_geo.closes[i];
        const bool close_hovered = PtInRect(&close, g_mouse) != 0;
        if (close_hovered) {
            if (aero) {
                const int radius = std::min(close.right - close.left, close.bottom - close.top);
                FillRoundRect(dc, close, palette.accent, palette.line, radius);
            } else {
                HBRUSH hot = CreateSolidBrush(palette.accent);
                FillRect(dc, &close, hot);
                DeleteObject(hot);
            }
        }
        SetTextColor(dc, close_hovered ? palette.bg
                                       : (aero ? palette.ink : palette.muted));
        DrawTextW(dc, L"\u00D7", -1, (RECT *)&close,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    const bool plus_hovered = PtInRect(&g_geo.plus, g_mouse) != 0;
    const COLORREF plus_face = plus_hovered ? palette.tab_active : palette.tab_idle;
    if (aero) {
        FillRoundRect(dc, g_geo.plus, plus_face, palette.line,
                      g_geo.plus.bottom - g_geo.plus.top);
    } else {
        HBRUSH plus_brush = CreateSolidBrush(plus_face);
        FillRect(dc, &g_geo.plus, plus_brush);
        DeleteObject(plus_brush);
        FrameRect(dc, &g_geo.plus, border_brush);
    }
    SetTextColor(dc, palette.ink);
    DrawTextW(dc, L"+", -1, &g_geo.plus,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    DeleteObject(border_brush);
    SelectObject(dc, old_font);
}

/* --------------------------------------------------------------- functions */
static void LayoutChildren() {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    const int width = rc.right - rc.left;
    const int top = ZEPHIUM_TABBAR_HEIGHT + 6;
    const int height = 30;
    int x = 8;

    HWND buttons[3] = {g_back, g_forward, g_reload};
    for (int i = 0; i < 3; ++i) {
        if (buttons[i]) MoveWindow(buttons[i], x, top, 30, height, TRUE);
        x += 34;
    }

    const int zoom_in_x = width - 8 - 30;
    const int label_x = zoom_in_x - 6 - 52;
    const int zoom_out_x = label_x - 6 - 30;
    const int theme_x = zoom_out_x - 6 - 30;
    const int url_x = x + 2;
    int url_w = theme_x - 8 - (url_x - 2);
    /* The edit itself is url_w - 4 wide (the 2 px field frame is drawn around
     * it on each side), so reserve 124 px to keep the address field >= 120 px. */
    if (url_w < 124) url_w = 124;

    if (g_url) MoveWindow(g_url, url_x, top + 2, url_w - 4, height - 4, TRUE);
    if (g_theme_btn) MoveWindow(g_theme_btn, theme_x, top, 30, height, TRUE);
    if (g_zoom_out) MoveWindow(g_zoom_out, zoom_out_x, top, 30, height, TRUE);
    if (g_zoom_label) MoveWindow(g_zoom_label, label_x, top, 52, height, TRUE);
    if (g_zoom_in) MoveWindow(g_zoom_in, zoom_in_x, top, 30, height, TRUE);

    RECT content = ContentRect();
    for (ZephiumTab *t : g_tabs) {
        if (t->ctrl) t->ctrl->put_Bounds(content);
    }
}

static void UpdateZoomLabel() {
    if (!g_zoom_label) return;
    wchar_t buf[32];
    swprintf(buf, 32, L"%d%%", g_zoom_percent);
    SetWindowTextW(g_zoom_label, buf);
    if (g_hwnd && !g_newtab_mode) {
        /* The percentage is painted by DrawToolbar; a hidden STATIC does not
         * repaint its parent when its text changes. */
        RECT bar = {0, ZEPHIUM_TABBAR_HEIGHT, 0, ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT};
        GetClientRect(g_hwnd, &bar);
        bar.top = ZEPHIUM_TABBAR_HEIGHT;
        bar.bottom = ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT;
        InvalidateRect(g_hwnd, &bar, FALSE);
    }
}

/* The start page is a full-bleed surface: the tab strip stays, the URL row
 * hides, and the web view grows into the space. */
static void ApplyNewTabMode(bool on) {
    if (g_newtab_mode == on) return;
    g_newtab_mode = on;

    HWND controls[] = {g_back, g_forward, g_reload, g_url, g_theme_btn, g_zoom_out,
                       g_zoom_in};
    for (HWND control : controls) {
        if (control) ShowWindow(control, on ? SW_HIDE : SW_SHOW);
    }
    LayoutChildren();
    SetTitleBarColors();
    if (g_hwnd) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        InvalidateRect(g_hwnd, &rc, TRUE);
    }
    Log(L"chrome mode: %ls", on ? L"new tab" : L"browsing");
}

static void SyncActiveView() {
    ZephiumTab *tab = ActiveTab();
    if (!tab) return;

    zephium_tab_t *core_tab = zephium_browser_get_tab(g_core, (size_t)g_active);
    if (core_tab && core_tab->zoom_percent && (int)core_tab->zoom_percent != g_zoom_percent) {
        g_zoom_percent = (int)core_tab->zoom_percent;
        UpdateZoomLabel();
    }

    if (tab->view) {
        LPWSTR src = nullptr;
        if (SUCCEEDED(tab->view->get_Source(&src)) && src) {
            if (GetFocus() != g_url) SetWindowTextW(g_url, src);
            std::wstring observed(src);
            CoTaskMemFree(src);
            /* WebView2 reports about:blank before the first navigation starts.
             * Do not let that clobber the URL the tab was created with, and do
             * not chase the old Source while a deliberate navigation is in
             * flight. */
            const bool settling = GetTickCount64() < tab->suppress_source_until;
            const bool placeholder = observed == L"about:blank" && tab->url != L"about:blank";
            if (!settling && !placeholder && observed != tab->url) {
                tab->url = observed;
                if (core_tab && observed != Utf8ToWide(core_tab->current_url)) {
                    zephium_browser_navigate(g_core, WideToUtf8(observed).c_str());
                }
                UpdateTabStrip();
            }
        }

        LPWSTR title = nullptr;
        if (SUCCEEDED(tab->view->get_DocumentTitle(&title)) && title) {
            std::wstring observed(title);
            CoTaskMemFree(title);
            if (!observed.empty() && observed != tab->title) {
                tab->title = observed;
                if (core_tab) {
                    zephium_browser_set_tab_title(g_core, (size_t)g_active, WideToUtf8(observed).c_str());
                }
                UpdateTabStrip();
            }
            std::wstring full = tab->title + L" - Zephium Browser";
            wchar_t current[512] = {0};
            GetWindowTextW(g_hwnd, current, 512);
            if (wcscmp(current, full.c_str()) != 0) SetWindowTextW(g_hwnd, full.c_str());
        }

        BOOL can = FALSE;
        if (SUCCEEDED(tab->view->get_CanGoBack(&can))) EnableWindow(g_back, can);
        if (SUCCEEDED(tab->view->get_CanGoForward(&can))) EnableWindow(g_forward, can);
    } else {
        if (GetFocus() != g_url) SetWindowTextW(g_url, tab->url.c_str());
        EnableWindow(g_back, FALSE);
        EnableWindow(g_forward, FALSE);
    }
    ApplyNewTabMode(IsNewTabUrl(tab->url));
}

static bool TabPlayingAudio(ZephiumTab *tab) {
    if (!tab || !tab->view) return false;
    ICoreWebView2_8 *view8 = nullptr;
    if (FAILED(tab->view->QueryInterface(kIID_WebView2_8, (void **)&view8)) || !view8) return false;
    BOOL playing = FALSE;
    HRESULT hr = view8->get_IsDocumentPlayingAudio(&playing);
    view8->Release();
    return SUCCEEDED(hr) && playing;
}

static void GiveViewFocus(ZephiumTab *tab) {
    if (tab && tab->ctrl) tab->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

/* --------------------------------------------------------- COM tab handlers */
class ZephiumControllerHandler
    : public ZephiumCom<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, &kIID_CtrlCompleted> {
public:
    explicit ZephiumControllerHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override;

private:
    int m_tab_id;
};

class ZephiumAccelHandler
    : public ZephiumCom<ICoreWebView2AcceleratorKeyPressedEventHandler, &kIID_AccelHandler> {
public:
    explicit ZephiumAccelHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *args) override;

private:
    int m_tab_id;
};

/* ------------------------------------------------------- downloads + permissions */

static unsigned g_download_seq = 0;

static const wchar_t *PermissionKindName(COREWEBVIEW2_PERMISSION_KIND kind) {
    switch (kind) {
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE: return L"microphone";
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA: return L"camera";
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION: return L"geolocation";
    case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS: return L"notifications";
    case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS: return L"sensors";
    case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ: return L"clipboard-read";
    case COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS: return L"multiple-downloads";
    case COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE: return L"file-read-write";
    case COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY: return L"autoplay";
    case COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS: return L"local-fonts";
    case COREWEBVIEW2_PERMISSION_KIND_MIDI_SYSTEM_EXCLUSIVE_MESSAGES: return L"midi-exclusive";
    case COREWEBVIEW2_PERMISSION_KIND_WINDOW_MANAGEMENT: return L"window-management";
    case COREWEBVIEW2_PERMISSION_KIND_PERSISTENT_STORAGE: return L"persistent-storage";
    default: return L"unknown";
    }
}

/* The user's Downloads known folder, created when it does not exist yet. */
static std::wstring DownloadsDirectory() {
    std::wstring dir;
#ifdef FOLDERID_Downloads
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &known)) && known) {
        dir.assign(known);
        CoTaskMemFree(known);
    }
#endif
    if (dir.empty()) {
        wchar_t profile[MAX_PATH] = {0};
        DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (len > 0 && len < MAX_PATH) dir = std::wstring(profile) + L"\\Downloads";
    }
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

/* Keeps only the last path component of the server-provided name, replaces every
 * separator and control character, drops leading dots and never returns "". */
static std::wstring SanitizeDownloadName(const std::wstring &suggested) {
    size_t slash = suggested.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? suggested : suggested.substr(slash + 1);

    std::wstring out;
    out.reserve(base.size());
    for (wchar_t c : base) {
        if (c == L'\\' || c == L'/' || c == L':' || c < 0x20) out.push_back(L'_');
        else out.push_back(c);
    }

    size_t first = out.find_first_not_of(L'.');
    out = first == std::wstring::npos ? std::wstring() : out.substr(first);
    if (out.empty()) out = L"download";
    return out;
}

static std::wstring UniqueDownloadPath(const std::wstring &dir, const std::wstring &name) {
    std::wstring path = dir + L"\\" + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    for (unsigned n = 1; n < 1000; ++n) {
        wchar_t suffix[32];
        swprintf(suffix, 32, L" (%u)", n);
        std::wstring candidate = dir + L"\\" + name + suffix;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return path;
}

class ZephiumNewWindowHandler
    : public ZephiumCom<ICoreWebView2NewWindowRequestedEventHandler,
                        &IID_ICoreWebView2NewWindowRequestedEventHandler> {
public:
    explicit ZephiumNewWindowHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) override;

private:
    int m_tab_id;
};


/* Default-deny permission handler: WebView2 asks before a page may use a device,
 * the location, notifications, the clipboard or the filesystem. Only autoplay
 * (media playback) is granted - the same line the GTK shell draws with EME. */
class ZephiumPermissionHandler
    : public ZephiumCom<ICoreWebView2PermissionRequestedEventHandler,
                        &IID_ICoreWebView2PermissionRequestedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) override;
};

class ZephiumDownloadHandler
    : public ZephiumCom<ICoreWebView2DownloadStartingEventHandler,
                        &IID_ICoreWebView2DownloadStartingEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) override;
};

class ZephiumDownloadProgressHandler
    : public ZephiumCom<ICoreWebView2BytesReceivedChangedEventHandler,
                        &IID_ICoreWebView2BytesReceivedChangedEventHandler> {
public:
    explicit ZephiumDownloadProgressHandler(std::string id) : m_id(std::move(id)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2DownloadOperation *sender, IUnknown *args) override;

private:
    std::string m_id;
    ULONGLONG m_last_update_ms = 0;
};

class ZephiumDownloadStateHandler
    : public ZephiumCom<ICoreWebView2StateChangedEventHandler,
                        &IID_ICoreWebView2StateChangedEventHandler> {
public:
    explicit ZephiumDownloadStateHandler(std::string id) : m_id(std::move(id)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2DownloadOperation *sender, IUnknown *args) override;

private:
    std::string m_id;
};

/* ------------------------------------------------------------ ad blocking */
/* Native network-level filtering: every request WebView2 is about to send is
 * checked against the adblock-rust engine (blocker/adblock_ffi.h) before
 * Chromium sends it. The handlers below only ever touch the event args for an
 * explicit BLOCK/REDIRECT verdict; everything else - including any error - lets
 * the request through unchanged, because a broken blocker must never break
 * browsing.
 *
 * The engine handle is thread affine, so it is created, used and freed on this
 * UI thread only (WebView2 raises these events on the UI thread). The filter
 * list downloader runs on its own thread and never touches the engine; it asks
 * the UI thread to rebuild through ZEPHIUM_ADBLOCK_WM_REFRESH. */

/* Same IIDs as the SDK constants of the same names; copied as plain GUIDs so the
 * ZephiumCom template can take a compile-time pointer (same pattern as the other
 * handlers above). */
static const GUID kIID_WebResourceRequested = {0xab00b74c, 0x15f1, 0x4646,
                                               {0x80, 0xe8, 0xe7, 0x63, 0x41, 0xd2, 0x5d, 0x71}};
static const GUID kIID_FrameCreated = {0x38059770, 0x9baa, 0x11eb,
                                       {0xa8, 0xb3, 0x02, 0x42, 0xac, 0x13, 0x00, 0x03}};
static const GUID kIID_FrameNavigationStarting = {0xe79908bf, 0x2d5d, 0x4968,
                                                  {0x83, 0xdb, 0x26, 0x3f, 0xea, 0x2c, 0x1d, 0xa3}};
static const GUID kIID_NavigationStarting = {0x9adbe429, 0xf36d, 0x432b,
                                              {0x9d, 0xdc, 0xf8, 0x88, 0x1f, 0xbd, 0x76, 0xe3}};
static const GUID kIID_SourceChanged = {0x3c067f9f, 0x5388, 0x4772,
                                        {0x8b, 0x48, 0x79, 0xf7, 0xef, 0x1a, 0xb3, 0x7c}};

/* Request events can be extremely frequent (hundreds per page); logging every
 * decision would both slow pages down and make the log useless. The first few
 * of each kind are logged in full, the rest only appear in the stats line. */
static unsigned g_adblock_block_logs = 0;
static unsigned g_adblock_redirect_logs = 0;
static unsigned g_adblock_rewrite_logs = 0;
static const unsigned kAdblockDecisionLogCap = 20;

static std::string TakeComString(LPWSTR value) {
    if (!value) return std::string();
    std::string out = WideToUtf8(value);
    CoTaskMemFree(value);
    return out;
}

/* Builds a synthetic response for the request.  The stream is created from a
 * copy of the body, so the engine result buffer (valid only until the next
 * check on the same handle) may be reused immediately. */
static void AdblockSetResponse(ICoreWebView2WebResourceRequestedEventArgs *args, int status,
                               const wchar_t *reason, const wchar_t *headers,
                               const unsigned char *body, size_t body_len) {
    if (!args || !g_env) return;
    IStream *stream = SHCreateMemStream(body_len ? body : nullptr, (UINT)body_len);
    if (!stream) {
        Log(L"adblock: SHCreateMemStream failed; request allowed");
        return;
    }
    ICoreWebView2WebResourceResponse *response = nullptr;
    const HRESULT hr =
        g_env->CreateWebResourceResponse(stream, status, reason, headers, &response);
    stream->Release();
    if (FAILED(hr) || !response) {
        Log(L"adblock: CreateWebResourceResponse failed (0x%08lx); request allowed",
            (unsigned long)hr);
        return;
    }
    const HRESULT put = args->put_Response(response);
    response->Release();
    if (FAILED(put)) {
        Log(L"adblock: put_Response failed (0x%08lx); request allowed", (unsigned long)put);
    }
}

/* Header values come from the engine, so keep them free of CR/LF: a response we
 * fabricate must not be a header-injection primitive. */
static bool HeaderValueSafe(const std::wstring &value) {
    if (value.empty()) return false;
    for (wchar_t c : value) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static std::wstring AdblockResponseHeaders(const zephium_adblock_result_t &result,
                                           zephium_adblock_resource_type_t type) {
    std::wstring mime = result.redirect_mime ? Utf8ToWide(result.redirect_mime) : std::wstring();
    if (!HeaderValueSafe(mime)) mime = L"text/plain";
    std::wstring headers = L"Content-Type: " + mime + L"\r\n";
    /* A script/stylesheet body served from a memory stream has no network origin;
     * pages with strict CORS would otherwise refuse to execute/apply it. */
    if (type == ZEPHIUM_ADBLOCK_RES_SCRIPT || type == ZEPHIUM_ADBLOCK_RES_STYLESHEET) {
        headers += L"Access-Control-Allow-Origin: *\r\n";
    }
    return headers;
}

static void LogAdblockBlocked(const ZephiumTab *tab, zephium_adblock_resource_type_t type,
                              const std::string &url) {
    if (g_adblock_block_logs >= kAdblockDecisionLogCap) return;
    ++g_adblock_block_logs;
    Log(L"adblock: blocked %ls request in tab %d: %ls",
        zephium_adblock_wide(zephium_adblock_type_name(type)).c_str(), tab->id,
        RedactUrl(zephium_adblock_wide(url)).c_str());
    if (g_adblock_block_logs == kAdblockDecisionLogCap) {
        Log(L"adblock: further blocked requests are counted in the stats line only");
    }
}

static void LogAdblockRedirect(const ZephiumTab *tab, zephium_adblock_resource_type_t type,
                               const std::string &url, const zephium_adblock_result_t &result) {
    if (g_adblock_redirect_logs >= kAdblockDecisionLogCap) return;
    ++g_adblock_redirect_logs;
    const std::wstring mime = result.redirect_mime
                                  ? zephium_adblock_wide(result.redirect_mime)
                                  : std::wstring(L"?");
    Log(L"adblock: served %ls placeholder (%ls, %zu bytes) in tab %d for %ls",
        zephium_adblock_wide(zephium_adblock_type_name(type)).c_str(), mime.c_str(),
        result.redirect_len, tab->id, RedactUrl(zephium_adblock_wide(url)).c_str());
}

static void LogAdblockRewrite(const ZephiumTab *tab, zephium_adblock_resource_type_t type,
                              const std::string &url, const zephium_adblock_result_t &result) {
    if (g_adblock_rewrite_logs >= kAdblockDecisionLogCap) return;
    ++g_adblock_rewrite_logs;
    const std::wstring rewritten =
        result.rewritten_url
            ? zephium_adblock_wide(std::string(result.rewritten_url, result.rewritten_len))
            : std::wstring();
    Log(L"adblock: $removeparam rewrite (not supported by WebView2) for %ls in tab %d: %ls "
        L"-> %ls",
        zephium_adblock_wide(zephium_adblock_type_name(type)).c_str(), tab->id,
        RedactUrl(zephium_adblock_wide(url)).c_str(), RedactUrl(rewritten).c_str());
}

class ZephiumWebResourceHandler
    : public ZephiumCom<ICoreWebView2WebResourceRequestedEventHandler, &kIID_WebResourceRequested> {
public:
    explicit ZephiumWebResourceHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2WebResourceRequestedEventArgs *args) override;

private:
    int m_tab_id;
};

/* FrameCreated -> the frame's own navigation events.  WebView2 reports iframe
 * document requests as COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT, so the shell
 * remembers frame navigation URLs and re-labels those requests SUBDOCUMENT. */
class ZephiumFrameCreatedHandler
    : public ZephiumCom<ICoreWebView2FrameCreatedEventHandler, &kIID_FrameCreated> {
public:
    explicit ZephiumFrameCreatedHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2FrameCreatedEventArgs *args) override;

private:
    int m_tab_id;
};

class ZephiumFrameNavigationHandler
    : public ZephiumCom<ICoreWebView2FrameNavigationStartingEventHandler,
                        &kIID_FrameNavigationStarting> {
public:
    explicit ZephiumFrameNavigationHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Frame *sender,
                                     ICoreWebView2NavigationStartingEventArgs *args) override;

private:
    int m_tab_id;
};

/* Main-frame navigation: a new top-level document means the previous document's
 * iframe set is stale, and the new URL becomes the fallback source URL. */
class ZephiumDocumentNavigationHandler
    : public ZephiumCom<ICoreWebView2NavigationStartingEventHandler, &kIID_NavigationStarting> {
public:
    explicit ZephiumDocumentNavigationHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2NavigationStartingEventArgs *args) override;

private:
    int m_tab_id;
};

class ZephiumSourceChangedHandler
    : public ZephiumCom<ICoreWebView2SourceChangedEventHandler, &kIID_SourceChanged> {
public:
    explicit ZephiumSourceChangedHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2SourceChangedEventArgs *args) override;

private:
    int m_tab_id;
};

/* Navigation completion for a start-page view is the reliable moment to hand it
 * the theme: the page's own script has run by then, and a view that was still
 * loading when the theme was clicked gets a second chance. */
class ZephiumNavigationCompletedHandler
    : public ZephiumCom<ICoreWebView2NavigationCompletedEventHandler,
                        &IID_ICoreWebView2NavigationCompletedEventHandler> {
public:
    explicit ZephiumNavigationCompletedHandler(int tab_id) : m_tab_id(tab_id) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2NavigationCompletedEventArgs *args) override;

private:
    int m_tab_id;
};

HRESULT STDMETHODCALLTYPE ZephiumWebResourceHandler::Invoke(
    ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) {
    /* No exception may escape into WebView2, and every early return leaves the
     * args untouched, i.e. the request is sent as-is. */
    try {
        if (!args || !zephium_adblock_is_ready()) return S_OK;
        ZephiumTab *tab = FindTabById(m_tab_id);
        if (!tab) return S_OK;

        ICoreWebView2WebResourceRequest *request = nullptr;
        if (FAILED(args->get_Request(&request)) || !request) return S_OK;

        std::string url;
        std::string method;
        LPWSTR value = nullptr;
        if (SUCCEEDED(request->get_Uri(&value)) && value) url = TakeComString(value);
        value = nullptr;
        if (SUCCEEDED(request->get_Method(&value)) && value) method = TakeComString(value);

        std::string referer;
        ICoreWebView2HttpRequestHeaders *headers = nullptr;
        if (SUCCEEDED(request->get_Headers(&headers)) && headers) {
            value = nullptr;
            if (SUCCEEDED(headers->GetHeader(L"Referer", &value)) && value) {
                referer = TakeComString(value);
            }
            headers->Release();
        }
        request->Release();
        if (url.empty()) return S_OK;

        COREWEBVIEW2_WEB_RESOURCE_CONTEXT context = COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER;
        if (FAILED(args->get_ResourceContext(&context))) {
            context = COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER;
        }

        const zephium_adblock_resource_type_t type =
            zephium_adblock_classify((int)context, url, tab->adblock);
        const std::string source =
            zephium_adblock_source_url(type, url, referer, tab->adblock.document_url());

        zephium_adblock_result_t result = {};
        if (zephium_adblock_check_url(url, source, type, method, &result) !=
            ZEPHIUM_ADBLOCK_OK) {
            return S_OK;  /* fail open */
        }
        switch (result.verdict) {
        case ZEPHIUM_ADBLOCK_BLOCK:
            /* 204 No Content: Chromium sends no network traffic and the page gets
             * an empty resource. */
            AdblockSetResponse(args, 204, L"No Content", L"", nullptr, 0);
            LogAdblockBlocked(tab, type, url);
            break;
        case ZEPHIUM_ADBLOCK_REDIRECT: {
            const std::wstring response_headers = AdblockResponseHeaders(result, type);
            AdblockSetResponse(args, 200, L"OK", response_headers.c_str(),
                               (const unsigned char *)result.redirect_body, result.redirect_len);
            LogAdblockRedirect(tab, type, url, result);
            break;
        }
        case ZEPHIUM_ADBLOCK_REWRITE:
            /* WebView2 cannot replace a request URL from this event; count it
             * (engine stats) and let the request through. See windows/README.md
             * for the CDP Network.setBlockedURLs alternative we did not take. */
            LogAdblockRewrite(tab, type, url, result);
            break;
        case ZEPHIUM_ADBLOCK_ALLOW:
        default:
            break;
        }
    } catch (...) {
        Log(L"adblock: exception while filtering a request; request left untouched");
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumFrameCreatedHandler::Invoke(
    ICoreWebView2 *, ICoreWebView2FrameCreatedEventArgs *args) {
    try {
        if (!args) return S_OK;
        ICoreWebView2Frame *frame = nullptr;
        if (FAILED(args->get_Frame(&frame)) || !frame) return S_OK;
        /* NavigationStarting lives on ICoreWebView2Frame2 (FrameCreated exists
         * since runtime 1.0.1082; the QI is cheap and fails gracefully). */
        ICoreWebView2Frame2 *frame2 = nullptr;
        if (SUCCEEDED(frame->QueryInterface(IID_ICoreWebView2Frame2, (void **)&frame2)) &&
            frame2) {
            EventRegistrationToken token = {};
            ZephiumFrameNavigationHandler *handler = new ZephiumFrameNavigationHandler(m_tab_id);
            frame2->add_NavigationStarting(handler, &token);  /* takes its own reference */
            handler->Release();                               /* drop our constructor ref */
            frame2->Release();
        }
        /* The handler is owned by the frame (which WebView2 owns) and holds only
         * the tab id, so a destroyed iframe cannot leak a COM reference here. */
        frame->Release();
    } catch (...) {
        Log(L"adblock: exception while installing iframe tracking; iframes stay documents");
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumFrameNavigationHandler::Invoke(
    ICoreWebView2Frame *, ICoreWebView2NavigationStartingEventArgs *args) {
    try {
        if (!args) return S_OK;
        ZephiumTab *tab = FindTabById(m_tab_id);
        if (!tab) return S_OK;
        LPWSTR value = nullptr;
        if (SUCCEEDED(args->get_Uri(&value)) && value) {
            const std::string uri = TakeComString(value);
            if (!uri.empty()) tab->adblock.FrameNavigating(uri);
        }
    } catch (...) {
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumDocumentNavigationHandler::Invoke(
    ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
    try {
        if (!args) return S_OK;
        ZephiumTab *tab = FindTabById(m_tab_id);
        if (!tab) return S_OK;
        LPWSTR value = nullptr;
        if (SUCCEEDED(args->get_Uri(&value)) && value) {
            const std::string uri = TakeComString(value);
            if (!uri.empty()) tab->adblock.BeginDocument(uri);
        }
    } catch (...) {
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumSourceChangedHandler::Invoke(
    ICoreWebView2 *sender, ICoreWebView2SourceChangedEventArgs *) {
    try {
        if (!sender) return S_OK;
        ZephiumTab *tab = FindTabById(m_tab_id);
        if (!tab) return S_OK;
        /* The event args only carry IsNewDocument; the committed URL comes from
         * the sender (a history navigation or an in-page document change). */
        LPWSTR value = nullptr;
        if (SUCCEEDED(sender->get_Source(&value)) && value) {
            const std::string url = TakeComString(value);
            if (!url.empty()) tab->adblock.SetDocumentUrl(url);
        }
    } catch (...) {
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumNavigationCompletedHandler::Invoke(
    ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *) {
    try {
        if (!sender) return S_OK;
        ZephiumTab *tab = FindTabById(m_tab_id);
        if (!tab || !tab->view) return S_OK;
        /* Ask the sender for the committed URL, so "?query" and "#hash" forms
         * are recognised by the same IsNewTabUrl predicate as everywhere else. */
        LPWSTR value = nullptr;
        if (SUCCEEDED(sender->get_Source(&value)) && value) {
            std::wstring source(value);
            CoTaskMemFree(value);
            if (IsNewTabUrl(source)) SendThemeToView(tab);
        }
    } catch (...) {
        Log(L"theme: exception while pushing the theme after navigation");
    }
    return S_OK;
}

/* Installs the request filter and the document/iframe bookkeeping for one tab.
 * Called for every CoreWebView2 the shell creates (new tab and wake-up), and the
 * filter must be added before the first navigation so the first page is covered. */
static void AttachAdblockToTab(ZephiumTab *tab) {
    if (!tab || !tab->view || !zephium_adblock_is_enabled()) return;
    tab->adblock.Clear();
    if (!tab->url.empty()) tab->adblock.SetDocumentUrl(WideToUtf8(tab->url));

    HRESULT hr = tab->view->AddWebResourceRequestedFilter(
        L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    if (FAILED(hr)) {
        Log(L"adblock: AddWebResourceRequestedFilter failed for tab id=%d (0x%08lx)",
            tab->id, (unsigned long)hr);
        return;
    }
    /* add_* takes its own reference to each handler; the constructor reference is
     * dropped right away so no handler outlives the webview that owns it. */
    EventRegistrationToken token = {};
    ZephiumWebResourceHandler *resource_handler = new ZephiumWebResourceHandler(tab->id);
    tab->view->add_WebResourceRequested(resource_handler, &token);
    resource_handler->Release();
    ZephiumDocumentNavigationHandler *navigation_handler =
        new ZephiumDocumentNavigationHandler(tab->id);
    tab->view->add_NavigationStarting(navigation_handler, &token);
    navigation_handler->Release();
    ZephiumSourceChangedHandler *source_handler = new ZephiumSourceChangedHandler(tab->id);
    tab->view->add_SourceChanged(source_handler, &token);
    source_handler->Release();

    ICoreWebView2_4 *view4 = nullptr;
    if (SUCCEEDED(tab->view->QueryInterface(IID_ICoreWebView2_4, (void **)&view4)) && view4) {
        EventRegistrationToken frame_token = {};
        ZephiumFrameCreatedHandler *frame_handler = new ZephiumFrameCreatedHandler(tab->id);
        view4->add_FrameCreated(frame_handler, &frame_token);
        frame_handler->Release();
        view4->Release();
    } else {
        Log(L"adblock: ICoreWebView2_4 missing; iframe documents are filtered as documents");
    }
    Log(L"adblock: request filtering installed for tab id=%d", tab->id);
}

/* ------------------------------------------------------------- tab control */
static void CreateTabController(int index);

static void ZoomStep(bool zoom_in) {
    if (!g_core) return;
    int percent = (int)zephium_browser_step_zoom(g_core, zoom_in);
    if (ZephiumTab *tab = ActiveTab()) {
        if (tab->ctrl) tab->ctrl->put_ZoomFactor((double)percent / 100.0);
    }
    g_zoom_percent = percent;
    UpdateZoomLabel();
    Log(L"action: zoom %s -> %d%%", zoom_in ? L"in" : L"out", percent);
}

static void ActivateTab(int index);

static int AddTab(const std::wstring &raw_url, bool activate, bool reuse_initial_tab) {
    if (!g_core) return -1;

    std::string url8 = WideToUtf8(raw_url);
    int core_index = -1;
    if (reuse_initial_tab && g_core->tab_count == 1) {
        core_index = 0;
        zephium_browser_navigate(g_core, url8.c_str());
    } else {
        if (!zephium_browser_new_tab(g_core, url8.c_str())) return -1;
        core_index = (int)g_core->tab_count - 1;
    }
    if (activate) zephium_browser_switch_tab(g_core, (size_t)core_index);

    zephium_tab_t *core_tab = zephium_browser_get_tab(g_core, (size_t)core_index);
    ZephiumTab *tab = new ZephiumTab();
    tab->id = ++g_next_tab_id;
    tab->url = core_tab ? Utf8ToWide(core_tab->current_url) : raw_url;
    tab->title = L"Tab mới";
    tab->last_active = GetTickCount64();

    g_tabs.push_back(tab);
    ApplyNewTabMode(IsNewTabUrl(tab->url));
    const int index = (int)g_tabs.size() - 1;
    Log(L"new tab %d id=%d url=%ls (core tabs=%zu)", index, tab->id, RedactUrl(tab->url).c_str(), g_core->tab_count);

    if (g_env) CreateTabController(index);
    if (activate || g_active < 0) ActivateTab(index);
    UpdateTabStrip();
    return index;
}

static void StartTabControllerAsync(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    ZephiumTab *tab = g_tabs[(size_t)index];
    tab->creating = true;
    HRESULT hr = g_env->CreateCoreWebView2Controller(g_hwnd, new ZephiumControllerHandler(tab->id));
    if (FAILED(hr)) {
        tab->creating = false;
        Log(L"CreateCoreWebView2Controller failed for tab %d: 0x%08lx", index, (unsigned long)hr);
    }
}

static void CreateTabController(int index) {
    if (!g_env) return;
    StartTabControllerAsync(index);
}

static void SleepTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    if (index == g_active) return;
    ZephiumTab *tab = g_tabs[(size_t)index];
    if (!tab || tab->sleeping || tab->creating || !tab->ctrl) return;
    if (TabPlayingAudio(tab)) {
        tab->last_active = GetTickCount64();
        return;
    }

    if (tab->view) {
        LPWSTR src = nullptr;
        if (SUCCEEDED(tab->view->get_Source(&src)) && src) {
            tab->url = src;
            CoTaskMemFree(src);
        }
    }
    tab->sleeping = true;
    /* The webview is gone; drop the iframe/document bookkeeping with it (it is
     * rebuilt from the restored URL when the tab wakes). */
    tab->adblock.Clear();
    tab->ctrl->Close();
    tab->ctrl->Release();
    tab->ctrl = nullptr;
    if (tab->view) {
        tab->view->Release();
        tab->view = nullptr;
    }
    Log(L"tab %d hibernated, url=%ls", index, RedactUrl(tab->url).c_str());
    UpdateTabStrip();
}

static void WakeTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    ZephiumTab *tab = g_tabs[(size_t)index];
    if (!tab || !tab->sleeping) return;
    tab->sleeping = false;
    tab->creating = true;
    Log(L"tab %d waking, url=%ls", index, RedactUrl(tab->url).c_str());
    CreateTabController(index);
    UpdateTabStrip();
}

static void ActivateTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size() || !g_core) return;

    if (g_active >= 0 && g_active < (int)g_tabs.size() && g_active != index) {
        ZephiumTab *previous = g_tabs[(size_t)g_active];
        previous->last_active = GetTickCount64();
        if (previous->ctrl) previous->ctrl->put_IsVisible(FALSE);
    }

    const bool changed = (g_active != index);
    g_active = index;
    zephium_browser_switch_tab(g_core, (size_t)index);

    ZephiumTab *tab = g_tabs[(size_t)index];
    tab->last_active = GetTickCount64();
    if (tab->sleeping) WakeTab(index);
    if (tab->ctrl) {
        tab->ctrl->put_Bounds(ContentRect());
        tab->ctrl->put_IsVisible(TRUE);
        GiveViewFocus(tab);
    }
    if (changed) Log(L"activate tab %d (%ls)", index, RedactUrl(tab->url).c_str());
    SyncActiveView();
    UpdateTabStrip();
}

static void CloseTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    ZephiumTab *tab = g_tabs[(size_t)index];
    Log(L"close tab %d id=%d", index, tab->id);

    if (tab->ctrl) {
        tab->ctrl->Close();
        tab->ctrl->Release();
        tab->ctrl = nullptr;
    }
    if (tab->view) {
        tab->view->Release();
        tab->view = nullptr;
    }
    delete tab;
    g_tabs.erase(g_tabs.begin() + index);
    if (g_active > index) g_active--;
    else if (g_active == index) g_active = -1;

    if (g_core && g_core->tab_count > 1) zephium_browser_close_tab(g_core, (size_t)index);

    if (g_tabs.empty()) {
        g_active = -1;
        AddTab(NewTabUrl(), true, false);
        return;
    }
    if (g_active < 0 || g_active >= (int)g_tabs.size()) g_active = std::min(index, (int)g_tabs.size() - 1);
    const int next = g_active;
    g_active = -1;
    ActivateTab(next);
    UpdateTabStrip();
}

static void NextTab(int delta) {
    const int count = (int)g_tabs.size();
    if (count < 2) return;
    int next = (g_active + delta) % count;
    if (next < 0) next += count;
    ActivateTab(next);
}

static void CheckHibernation() {
    if (g_sleep_seconds <= 0) return;
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (i == g_active) continue;
        ZephiumTab *tab = g_tabs[(size_t)i];
        if (!tab || tab->sleeping || tab->creating || !tab->ctrl) continue;
        if (now - tab->last_active >= (ULONGLONG)g_sleep_seconds * 1000ULL) SleepTab(i);
    }
}

static void NavigateToResolved(const std::wstring &raw) {
    if (raw.empty() || !g_core) return;
    ZephiumTab *tab = ActiveTab();
    if (!tab) return;

    std::string input = WideToUtf8(raw);
    if (!zephium_browser_navigate(g_core, input.c_str())) {
        Log(L"navigation refused by core: %ls", RedactUrl(raw).c_str());
        return;
    }
    zephium_tab_t *core_tab = zephium_browser_get_tab(g_core, (size_t)g_active);
    std::wstring resolved = core_tab ? Utf8ToWide(core_tab->current_url) : raw;
    tab->url = resolved;
    tab->suppress_source_until = GetTickCount64() + 1500;
    ApplyNewTabMode(IsNewTabUrl(tab->url));
    Log(L"navigate tab %d: %ls -> %ls", g_active, RedactUrl(raw).c_str(), RedactUrl(resolved).c_str());

    if (tab->sleeping) {
        WakeTab(g_active);
    } else if (tab->view) {
        tab->view->Navigate(resolved.c_str());
        GiveViewFocus(tab);
    }
    UpdateTabStrip();
}

static void NavigateFromEdit() {
    wchar_t buf[4096] = {0};
    GetWindowTextW(g_url, buf, 4096);
    NavigateToResolved(buf);
}

/* The start page asks the shell to resolve a typed address and navigate the
 * tab, so URL classification / search fallback stay in the C core rather than
 * being re-implemented in JavaScript. Messages from any other origin are
 * ignored. */
class ZephiumWebMessageHandler
    : public ZephiumCom<ICoreWebView2WebMessageReceivedEventHandler, &kIID_WebMessage> {
public:
    explicit ZephiumWebMessageHandler(int tab_id) : m_tab_id(tab_id) {}

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                     ICoreWebView2WebMessageReceivedEventArgs *args) override {
        if (!args) return S_OK;

        LPWSTR source = nullptr;
        if (FAILED(args->get_Source(&source)) || !source) return S_OK;
        std::wstring source_url(source);
        CoTaskMemFree(source);
        if (source_url.rfind(L"https://" ZEPHIUM_NEWTAB_HOST L"/", 0) != 0) {
            Log(L"ignored web message from %ls", RedactUrl(source_url).c_str());
            return S_OK;
        }

        LPWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
        std::wstring message(raw);
        CoTaskMemFree(raw);

        const std::wstring prefix = L"navigate|";
        if (message.rfind(prefix, 0) != 0) return S_OK;
        const std::wstring target = message.substr(prefix.size());
        if (target.empty()) return S_OK;

        int index = -1;
        for (int i = 0; i < (int)g_tabs.size(); ++i) {
            if (g_tabs[(size_t)i]->id == m_tab_id) {
                index = i;
                break;
            }
        }
        if (index < 0) return S_OK;
        if (index != g_active) ActivateTab(index);

        Log(L"newtab navigate: %ls", RedactUrl(target).c_str());
        NavigateToResolved(target);
        return S_OK;
    }

private:
    int m_tab_id;
};

/* --------------------------------------------------------- COM callbacks */
HRESULT STDMETHODCALLTYPE ZephiumControllerHandler::Invoke(HRESULT result, ICoreWebView2Controller *controller) {
    ZephiumTab *tab = FindTabById(m_tab_id);
    if (!tab) {
        if (controller) {
            controller->Close();
            controller->Release();
        }
        return S_OK;
    }
    tab->creating = false;

    if (FAILED(result) || !controller) {
        Log(L"controller creation failed for tab id=%d: 0x%08lx", m_tab_id, (unsigned long)result);
        return S_OK;
    }

    tab->ctrl = controller;
    controller->AddRef();
    if (FAILED(controller->get_CoreWebView2(&tab->view)) || !tab->view) {
        Log(L"get_CoreWebView2 failed for tab id=%d", m_tab_id);
        return S_OK;
    }

    ICoreWebView2Settings *settings = nullptr;
    if (SUCCEEDED(tab->view->get_Settings(&settings)) && settings) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_AreDefaultContextMenusEnabled(TRUE);
        settings->put_IsBuiltInErrorPageEnabled(TRUE);
        settings->Release();
    }

    EventRegistrationToken token = {};
    controller->add_AcceleratorKeyPressed(new ZephiumAccelHandler(tab->id), &token);

    EventRegistrationToken message_token = {};
    tab->view->add_WebMessageReceived(new ZephiumWebMessageHandler(tab->id), &message_token);

    EventRegistrationToken newwindow_token = {};
    tab->view->add_NewWindowRequested(new ZephiumNewWindowHandler(tab->id), &newwindow_token);

    EventRegistrationToken navigation_token = {};
    ZephiumNavigationCompletedHandler *navigation_completed =
        new ZephiumNavigationCompletedHandler(tab->id);
    tab->view->add_NavigationCompleted(navigation_completed, &navigation_token);
    navigation_completed->Release();

    EventRegistrationToken permission_token = {};
    tab->view->add_PermissionRequested(new ZephiumPermissionHandler(), &permission_token);

    /* Downloads live on ICoreWebView2_4 (runtime 1.0.864+); without it WebView2
     * falls back to its own save dialog and nothing is recorded. */
    ICoreWebView2_4 *view4 = nullptr;
    if (SUCCEEDED(tab->view->QueryInterface(IID_ICoreWebView2_4, (void **)&view4)) && view4) {
        EventRegistrationToken download_token = {};
        view4->add_DownloadStarting(new ZephiumDownloadHandler(), &download_token);
        view4->Release();
    } else {
        Log(L"ICoreWebView2_4 unavailable; downloads are not intercepted");
    }

    /* Install the request filter before the first Navigate() so the very first
     * page (and its subresources) is already covered. */
    AttachAdblockToTab(tab);

    if (!g_newtab_dir.empty()) {
        ICoreWebView2_3 *view3 = nullptr;
        if (SUCCEEDED(tab->view->QueryInterface(kIID_WebView2_3, (void **)&view3)) && view3) {
            HRESULT mapped = view3->SetVirtualHostNameToFolderMapping(
                ZEPHIUM_NEWTAB_HOST, g_newtab_dir.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            Log(L"map %ls -> %ls: 0x%08lx", ZEPHIUM_NEWTAB_HOST, g_newtab_dir.c_str(),
                (unsigned long)mapped);
            view3->Release();
        } else {
            Log(L"ICoreWebView2_3 unavailable; new tab host not mapped");
        }
    }

    const int index = (int)(std::find(g_tabs.begin(), g_tabs.end(), tab) - g_tabs.begin());
    zephium_tab_t *core_tab = g_core ? zephium_browser_get_tab(g_core, (size_t)index) : nullptr;
    const int zoom = (core_tab && core_tab->zoom_percent) ? (int)core_tab->zoom_percent : 100;
    controller->put_ZoomFactor((double)zoom / 100.0);
    controller->put_Bounds(ContentRect());
    controller->put_IsVisible(index == g_active ? TRUE : FALSE);

    if (!tab->url.empty() && tab->url != L"about:blank") {
        tab->view->Navigate(tab->url.c_str());
    }
    Log(L"tab %d ready (id=%d, url=%ls)", index, tab->id, RedactUrl(tab->url).c_str());

    if (index == g_active) {
        g_zoom_percent = zoom;
        UpdateZoomLabel();
        GiveViewFocus(tab);
        SyncActiveView();
    }
    UpdateTabStrip();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumAccelHandler::Invoke(ICoreWebView2Controller *,
                                                      ICoreWebView2AcceleratorKeyPressedEventArgs *args) {
    if (!args) return S_OK;
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    if (FAILED(args->get_KeyEventKind(&kind))) return S_OK;
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
        kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
        return S_OK;
    }

    UINT key = 0;
    if (FAILED(args->get_VirtualKey(&key))) return S_OK;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (ctrl && key == 'T') {
        AddTab(NewTabUrl(), true, false);
        args->put_Handled(TRUE);
    } else if (ctrl && key == 'W') {
        CloseTab(g_active);
        args->put_Handled(TRUE);
    } else if (ctrl && key == VK_TAB) {
        NextTab(shift ? -1 : 1);
        args->put_Handled(TRUE);
    } else if (ctrl && key == 'L') {
        if (g_newtab_mode) {
            if (ZephiumTab *tab = ActiveTab()) {
                if (tab->view) {
                    tab->view->ExecuteScript(
                        L"(function(){var i=document.getElementById('omnibox');"
                        L"if(i){i.focus();i.select();}})();",
                        nullptr);
                }
            }
        } else {
            SetFocus(g_url);
            SendMessageW(g_url, EM_SETSEL, 0, -1);
        }
        args->put_Handled(TRUE);
    } else if (ctrl && (key == VK_OEM_PLUS || key == VK_ADD)) {
        ZoomStep(true);
        args->put_Handled(TRUE);
    } else if (ctrl && (key == VK_OEM_MINUS || key == VK_SUBTRACT)) {
        ZoomStep(false);
        args->put_Handled(TRUE);
    } else if (key == VK_F5) {
        if (ZephiumTab *tab = ActiveTab()) {
            if (tab->view) tab->view->Reload();
        }
        args->put_Handled(TRUE);
    }
    (void)m_tab_id;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumNewWindowHandler::Invoke(ICoreWebView2 *,
                                                          ICoreWebView2NewWindowRequestedEventArgs *args) {
    if (!args) return S_OK;

    LPWSTR uri = nullptr;
    if (FAILED(args->get_Uri(&uri)) || !uri) {
        args->put_Handled(TRUE);
        return S_OK;
    }
    std::wstring target(uri);
    CoTaskMemFree(uri);

    /* Always handled: WebView2 would otherwise open a second top-level window with
     * its own chrome. The request becomes a tab of this shell instead. */
    args->put_Handled(TRUE);

    std::string target8 = WideToUtf8(target);
    if (!zephium_nav_is_allowed(target8.c_str(), target8.size())) {
        Log(L"popup blocked: %ls", RedactUrl(target).c_str());
        return S_OK;
    }

    Log(L"popup opened in a new tab: %ls", RedactUrl(target).c_str());
    if (AddTab(target, true, false) < 0) Log(L"popup: could not create a tab");
    (void)m_tab_id;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumPermissionHandler::Invoke(ICoreWebView2 *,
                                                           ICoreWebView2PermissionRequestedEventArgs *args) {
    if (!args) return S_OK;

    COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
    args->get_PermissionKind(&kind);

    std::wstring origin;
    LPWSTR uri = nullptr;
    if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
        origin.assign(uri);
        CoTaskMemFree(uri);
    }

    if (kind == COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY) {
        args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
        Log(L"permission: autoplay allowed (%ls)", RedactUrl(origin).c_str());
    } else {
        args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
        Log(L"permission: %ls denied (%ls)", PermissionKindName(kind), RedactUrl(origin).c_str());
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumDownloadHandler::Invoke(ICoreWebView2 *,
                                                         ICoreWebView2DownloadStartingEventArgs *args) {
    if (!args) return S_OK;

    ICoreWebView2DownloadOperation *op = nullptr;
    if (FAILED(args->get_DownloadOperation(&op)) || !op) return S_OK;

    std::wstring url;
    LPWSTR uri = nullptr;
    if (SUCCEEDED(op->get_Uri(&uri)) && uri) {
        url.assign(uri);
        CoTaskMemFree(uri);
    }

    std::wstring suggested;
    LPWSTR result_path = nullptr;
    if (SUCCEEDED(args->get_ResultFilePath(&result_path)) && result_path) {
        suggested.assign(result_path);
        CoTaskMemFree(result_path);
    }

    std::wstring name = SanitizeDownloadName(suggested);
    std::wstring dir = DownloadsDirectory();
    if (dir.empty()) {
        Log(L"download cancelled: no Downloads directory");
        args->put_Cancel(TRUE);
        op->Release();
        return S_OK;
    }

    std::wstring path = UniqueDownloadPath(dir, name);
    HRESULT put = args->put_ResultFilePath(path.c_str());
    if (FAILED(put)) {
        Log(L"download cancelled: destination rejected (0x%08lx)", (unsigned long)put);
        args->put_Cancel(TRUE);
        op->Release();
        return S_OK;
    }
    args->put_Handled(TRUE);   /* no "Save as" dialog */

    INT64 total = 0;
    op->get_TotalBytesToReceive(&total);

    char id[32] = {0};
    snprintf(id, sizeof(id), "dl-%u", ++g_download_seq);

    if (g_core && g_core->store) {
        std::string url8 = WideToUtf8(url);
        std::string path8 = WideToUtf8(path);
        if (!zephium_store_record_download(g_core->store, id, url8.c_str(), path8.c_str(),
                                           (int64_t)total)) {
            Log(L"download: could not record it in the store");
        }
    }

    EventRegistrationToken token = {};
    op->add_BytesReceivedChanged(new ZephiumDownloadProgressHandler(id), &token);
    op->add_StateChanged(new ZephiumDownloadStateHandler(id), &token);

    Log(L"download started: %ls -> %ls", RedactUrl(url).c_str(), name.c_str());
    op->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumDownloadProgressHandler::Invoke(ICoreWebView2DownloadOperation *sender,
                                                                 IUnknown *) {
    if (!sender || !g_core || !g_core->store) return S_OK;

    /* Raised per chunk; the store commits synchronously, so write at most twice a
     * second. */
    ULONGLONG now = GetTickCount64();
    if (m_last_update_ms != 0 && now - m_last_update_ms < 500) return S_OK;
    m_last_update_ms = now;

    INT64 received = 0, total = 0;
    sender->get_BytesReceived(&received);
    sender->get_TotalBytesToReceive(&total);
    zephium_store_update_download_progress(g_core->store, m_id.c_str(), (int64_t)received,
                                           (int64_t)total);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ZephiumDownloadStateHandler::Invoke(ICoreWebView2DownloadOperation *sender,
                                                              IUnknown *) {
    if (!sender || !g_core || !g_core->store) return S_OK;

    INT64 received = 0, total = 0;
    sender->get_BytesReceived(&received);
    sender->get_TotalBytesToReceive(&total);

    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    sender->get_State(&state);

    if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
        zephium_store_update_download_progress(g_core->store, m_id.c_str(), (int64_t)received,
                                               total > 0 ? (int64_t)total : (int64_t)received);
        zephium_store_update_download(g_core->store, m_id.c_str(), (int64_t)received,
                                      ZEPHIUM_DOWNLOAD_COMPLETED);
        Log(L"download finished: %lld bytes", (long long)received);
    } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        zephium_store_update_download(g_core->store, m_id.c_str(), (int64_t)received,
                                      ZEPHIUM_DOWNLOAD_FAILED);
        Log(L"download interrupted");
    }
    return S_OK;
}

/* ------------------------------------------------------------ dark chrome */

static void DrawChromeButton(const DRAWITEMSTRUCT *item) {
    if (!item) return;
    const ChromePalette &palette = Chrome();
    const bool aero = AeroTheme();
    const bool theme_button = item->CtlID == ID_THEME;

    const wchar_t *glyph = nullptr;
    if (!theme_button) {
        switch (item->CtlID) {
        case ID_BACK: glyph = L"\u2190"; break;
        case ID_FORWARD: glyph = L"\u2192"; break;
        case ID_RELOAD: glyph = L"\u21BB"; break;
        case ID_ZOOM_OUT: glyph = L"\u2212"; break;
        case ID_ZOOM_IN: glyph = L"+"; break;
        default: return;
        }
    }

    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;

    RECT r = item->rcItem;
    r.right -= 1;
    r.bottom -= 1;

    const COLORREF fill = pressed ? palette.button_pressed : palette.button;
    const COLORREF border =
        focused ? (aero ? palette.ink : palette.accent) : palette.line;
    const COLORREF ink = disabled ? palette.muted : palette.ink;

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);

    SetBkMode(item->hDC, TRANSPARENT);
    if (aero) {
        Ellipse(item->hDC, r.left, r.top, r.right, r.bottom);
    } else {
        RoundRect(item->hDC, r.left, r.top, r.right, r.bottom, 9, 9);
    }

    if (theme_button) {
        /* A plain filled dot: the control never depends on a symbol font. */
        const int cx = (r.left + r.right) / 2;
        const int cy = (r.top + r.bottom) / 2;
        const int radius = 5;
        const COLORREF dot_color = aero ? palette.ink : palette.accent;
        HBRUSH dot_brush = CreateSolidBrush(dot_color);
        HPEN dot_pen = CreatePen(PS_SOLID, 1, dot_color);
        HGDIOBJ old_dot_brush = SelectObject(item->hDC, dot_brush);
        HGDIOBJ old_dot_pen = SelectObject(item->hDC, dot_pen);
        Ellipse(item->hDC, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
        SelectObject(item->hDC, old_dot_pen);
        SelectObject(item->hDC, old_dot_brush);
        DeleteObject(dot_pen);
        DeleteObject(dot_brush);
    } else {
        HFONT font = g_icon_font ? g_icon_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HGDIOBJ old_font = SelectObject(item->hDC, font);
        SetTextColor(item->hDC, ink);
        RECT text = item->rcItem;
        DrawTextW(item->hDC, glyph, -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        SelectObject(item->hDC, old_font);
    }

    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void DrawToolbar(HDC dc, int width) {
    const ChromePalette &palette = Chrome();
    const bool aero = AeroTheme();

    RECT bar = {0, ZEPHIUM_TABBAR_HEIGHT, width, ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT};
    if (aero) {
        FillVerticalGradient(dc, bar, palette.toolbar, palette.toolbar_bottom);
    } else {
        HBRUSH background = CreateSolidBrush(palette.toolbar);
        FillRect(dc, &bar, background);
        DeleteObject(background);
    }

    RECT bottom = {0, ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT - 1, width,
                   ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT};
    HBRUSH line = CreateSolidBrush(palette.line);
    FillRect(dc, &bottom, line);
    DeleteObject(line);

    if (g_url && IsWindowVisible(g_url)) {
        RECT field;
        GetWindowRect(g_url, &field);
        MapWindowPoints(HWND_DESKTOP, g_hwnd, (POINT *)&field, 2);
        InflateRect(&field, 2, 2);

        const bool focused = GetFocus() == g_url;
        const COLORREF field_border =
            focused ? (aero ? palette.ink : palette.accent) : palette.line;
        HBRUSH fill = CreateSolidBrush(palette.field);
        HPEN pen = CreatePen(PS_SOLID, 1, field_border);
        HGDIOBJ old_brush = SelectObject(dc, fill);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        RoundRect(dc, field.left, field.top, field.right, field.bottom, 12, 12);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        DeleteObject(fill);
    }

    if (g_zoom_out && IsWindowVisible(g_zoom_out)) {
        RECT r;
        GetWindowRect(g_zoom_out, &r);
        MapWindowPoints(HWND_DESKTOP, g_hwnd, (POINT *)&r, 2);
        RECT divider = {r.left - 9, r.top + 7, r.left - 8, r.bottom - 7};
        HBRUSH div_brush = CreateSolidBrush(palette.line);
        FillRect(dc, &divider, div_brush);
        DeleteObject(div_brush);
    }

    /* Painted here, not by a STATIC child, so it sits on the gradient itself. */
    if (g_zoom_label) {
        RECT label;
        GetWindowRect(g_zoom_label, &label);
        MapWindowPoints(HWND_DESKTOP, g_hwnd, (POINT *)&label, 2);
        wchar_t text[32];
        swprintf(text, 32, L"%d%%", g_zoom_percent);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, palette.muted);
        HFONT font = g_tab_font ? g_tab_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HGDIOBJ old_font = SelectObject(dc, font);
        DrawTextW(dc, text, -1, &label,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        SelectObject(dc, old_font);
    }
}

/* ------------------------------------------------------------- window proc */
static LRESULT CALLBACK ZephiumWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case ZEPHIUM_ADBLOCK_WM_REFRESH:
        /* Posted by the filter-list download thread; the engine build has to run
         * on this UI thread (adblock-rust handles are thread affine). */
        zephium_adblock_on_build_message(wparam);
        return 0;

    case WM_CREATE: {
        g_hwnd = hwnd;
        HINSTANCE inst = ((LPCREATESTRUCTW)lparam)->hInstance;

        g_tab_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_url_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        g_icon_font = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
        g_edit_brush = CreateSolidBrush(Chrome().field);
        g_chrome_brush = CreateSolidBrush(Chrome().toolbar);

        const int top = ZEPHIUM_TABBAR_HEIGHT + 6;
        g_back = CreateWindowExW(0, L"BUTTON", L"\u2190",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 8, top, 30, 30, hwnd, (HMENU)ID_BACK, inst, nullptr);
        g_forward = CreateWindowExW(0, L"BUTTON", L"\u2192",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    42, top, 30, 30, hwnd, (HMENU)ID_FORWARD, inst, nullptr);
        g_reload = CreateWindowExW(0, L"BUTTON", L"\u21BB",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   76, top, 30, 30, hwnd, (HMENU)ID_RELOAD, inst, nullptr);
        g_url = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                112, top + 2, 600, 26, hwnd, (HMENU)ID_URL, inst, nullptr);
        g_theme_btn = CreateWindowExW(0, L"BUTTON", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, top, 30, 30, hwnd, (HMENU)ID_THEME, inst, nullptr);
        g_zoom_out = CreateWindowExW(0, L"BUTTON", L"\u2212",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, top, 30, 30, hwnd, (HMENU)ID_ZOOM_OUT, inst, nullptr);
        /* The percentage is painted by DrawToolbar, over the palette gradient:
         * a separate STATIC would erase its own rectangle in a solid colour. */
        g_zoom_label = CreateWindowExW(0, L"STATIC", L"100%",
                                       WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
                                       0, top, 52, 30, hwnd, (HMENU)ID_ZOOM_LABEL, inst, nullptr);
        g_zoom_in = CreateWindowExW(0, L"BUTTON", L"+",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, top, 30, 30, hwnd, (HMENU)ID_ZOOM_IN, inst, nullptr);

        SendMessageW(g_back, WM_SETFONT, (WPARAM)g_icon_font, TRUE);
        SendMessageW(g_forward, WM_SETFONT, (WPARAM)g_icon_font, TRUE);
        SendMessageW(g_reload, WM_SETFONT, (WPARAM)g_icon_font, TRUE);
        SendMessageW(g_zoom_out, WM_SETFONT, (WPARAM)g_icon_font, TRUE);
        SendMessageW(g_zoom_in, WM_SETFONT, (WPARAM)g_icon_font, TRUE);
        SendMessageW(g_url, WM_SETFONT, (WPARAM)g_url_font, TRUE);
        SendMessageW(g_zoom_label, WM_SETFONT, (WPARAM)g_tab_font, TRUE);
        SendMessageW(g_url, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        SendMessageW(g_url, EM_SETLIMITTEXT, 4096, 0);

        EnableWindow(g_back, FALSE);
        EnableWindow(g_forward, FALSE);
        UpdateZoomLabel();
        LayoutChildren();
        SetTitleBarColors();
        return 0;
    }

    case WM_SIZE:
        LayoutChildren();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* chrome is painted (double buffered) in WM_PAINT */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        const int chrome_height = ZEPHIUM_TABBAR_HEIGHT +
                                  (g_newtab_mode ? 0 : ZEPHIUM_TOOLBAR_HEIGHT);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, rc.right, chrome_height);
        HGDIOBJ old = SelectObject(memory, bitmap);
        DrawTabStrip(memory, rc.right);
        if (!g_newtab_mode) DrawToolbar(memory, rc.right);
        BitBlt(dc, 0, 0, rc.right, chrome_height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM:
        DrawChromeButton((const DRAWITEMSTRUCT *)lparam);
        return TRUE;

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wparam;
        if ((HWND)lparam == g_url) {
            SetTextColor(dc, Chrome().ink);
            SetBkColor(dc, Chrome().field);
            return (LRESULT)g_edit_brush;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, Chrome().muted);
        SetBkColor(dc, Chrome().toolbar);
        return (LRESULT)g_chrome_brush;
    }

    case WM_CTLCOLORBTN:
        return (LRESULT)g_chrome_brush;

    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.y < ZEPHIUM_TABBAR_HEIGHT) {
            RECT client;
            GetClientRect(hwnd, &client);
            ComputeTabGeometry(client.right);
            if (PtInRect(&g_geo.plus, pt)) {
                AddTab(NewTabUrl(), true, false);
                return 0;
            }
            for (size_t i = 0; i < g_geo.tabs.size() && i < g_tabs.size(); ++i) {
                if (PtInRect(&g_geo.closes[i], pt)) {
                    CloseTab((int)i);
                    return 0;
                }
                if (PtInRect(&g_geo.tabs[i], pt)) {
                    ActivateTab((int)i);
                    return 0;
                }
            }
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.y < ZEPHIUM_TABBAR_HEIGHT) {
            g_mouse = pt;
            UpdateTabStrip();
        } else if (g_mouse.x != -1) {
            g_mouse = POINT{-1, -1};
            UpdateTabStrip();
        }
        return 0;
    }

    case WM_TIMER:
        if (wparam == ZEPHIUM_TIMER_ID) {
            SyncActiveView();
            CheckHibernation();
            /* Daily filter refresh + the periodic ad-block stats line. */
            zephium_adblock_tick();
        }
        return 0;

    case WM_COMMAND: {
        if (LOWORD(wparam) == ID_URL &&
            (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS)) {
            RECT bar = {0, ZEPHIUM_TABBAR_HEIGHT, 0, 0};
            GetClientRect(hwnd, &bar);
            bar.top = ZEPHIUM_TABBAR_HEIGHT;
            bar.bottom = ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT;
            InvalidateRect(hwnd, &bar, FALSE);
            return 0;
        }
        switch (LOWORD(wparam)) {
        case ID_BACK:
            if (ZephiumTab *tab = ActiveTab()) {
                if (tab->view) tab->view->GoBack();
            }
            return 0;
        case ID_FORWARD:
            if (ZephiumTab *tab = ActiveTab()) {
                if (tab->view) tab->view->GoForward();
            }
            return 0;
        case ID_RELOAD:
            if (ZephiumTab *tab = ActiveTab()) {
                if (tab->view) tab->view->Reload();
            }
            return 0;
        case ID_THEME:
            ToggleTheme();
            return 0;
        case ID_ZOOM_IN:
            ZoomStep(true);
            return 0;
        case ID_ZOOM_OUT:
            ZoomStep(false);
            return 0;
        case ID_NEW_TAB:
            AddTab(NewTabUrl(), true, false);
            return 0;
        case ID_CLOSE_TAB:
            CloseTab(g_active);
            return 0;
        case ID_NEXT_TAB:
            NextTab(1);
            return 0;
        case ID_PREV_TAB:
            NextTab(-1);
            return 0;
        default:
            break;
        }
        return 0;
    }

    case WM_SETFOCUS:
        GiveViewFocus(ActiveTab());
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ZEPHIUM_TIMER_ID);
        for (ZephiumTab *tab : g_tabs) {
            if (tab->ctrl) {
                tab->ctrl->Close();
                tab->ctrl->Release();
            }
            if (tab->view) tab->view->Release();
            delete tab;
        }
        g_tabs.clear();
        Log(L"window destroyed");
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ------------------------------------------------------------------- entry */
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_exe_dir = ExeDirectory();
    g_log_path = g_exe_dir + L"\\zephium_win.log";
    Log(L"---- Zephium Windows shell starting ----");
    RestrictFileToOwner(g_log_path);

    /* The persisted theme is read before the window exists so the very first
     * paint already uses the right palette. */
    g_theme = LoadTheme();
    Log(L"theme: active palette %ls", g_theme == kThemeAero ? L"aero" : L"dark");

    /* Ad blocking is on unless the environment or the command line turns it off.
     * The switch only controls the native filter engine; browsing never depends
     * on it. */
    const wchar_t *adblock_env = _wgetenv(L"ZEPHIUM_ADBLOCK");
    if (adblock_env && wcscmp(adblock_env, L"0") == 0) g_adblock_enabled = false;

    std::wstring page_dir = g_exe_dir + L"\\newtab";
    if (!FileExistsW(page_dir + L"\\index.html")) {
        page_dir = g_exe_dir + L"\\..\\newtab";
    }
    if (FileExistsW(page_dir + L"\\index.html")) {
        g_newtab_dir = FullPathW(page_dir);
        g_newtab_url = ZEPHIUM_NEWTAB_URL;
        Log(L"new tab page: %ls", g_newtab_dir.c_str());
    } else {
        Log(L"new tab page not found (expected newtab\\index.html next to the exe)");
    }

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring initial = g_newtab_url.empty() ? std::wstring(L"https://duckduckgo.com")
                                                : g_newtab_url;
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--no-adblock") == 0) {
            g_adblock_enabled = false;
        } else if (wcscmp(argv[i], L"--tab-sleep") == 0 && i + 1 < argc) {
            g_sleep_seconds = _wtoi(argv[++i]) * 60;
            if (g_sleep_seconds < 0) g_sleep_seconds = 0;
        } else if (wcscmp(argv[i], L"--tab-sleep-seconds") == 0 && i + 1 < argc) {
            g_sleep_seconds = _wtoi(argv[++i]);
            if (g_sleep_seconds < 0) g_sleep_seconds = 0;
        } else if (argv[i][0] != L'-') {
            initial = argv[i];
        }
    }
    if (argv) LocalFree(argv);
    g_initial_url = initial;
    Log(L"tab hibernation: %d seconds (0 = off)", g_sleep_seconds);
    Log(L"ad blocking: %ls", g_adblock_enabled ? L"enabled (native network filtering, adblock-rust)"
                                              : L"disabled (--no-adblock / ZEPHIUM_ADBLOCK=0)");

    std::string db_path = WideToUtf8(g_exe_dir + L"\\zephium_profile.db");
    g_core = zephium_browser_init(db_path.c_str());
    if (!g_core) {
        Log(L"core init failed");
        MessageBoxW(nullptr, L"Could not open the SQLite profile database.",
                    L"Zephium", MB_ICONERROR | MB_OK);
        return 1;
    }
    /* The profile is created lazily above; -wal/-shm appear with it, so all three
     * are tightened right after the core opens the database. */
    {
        std::wstring db_file = Utf8ToWide(db_path.c_str());
        RestrictFileToOwner(db_file);
        RestrictFileToOwner(db_file + L"-wal");
        RestrictFileToOwner(db_file + L"-shm");
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ZephiumWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = ZEPHIUM_WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        Log(L"RegisterClassEx failed: %lu", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, ZEPHIUM_WINDOW_CLASS, L"Zephium Browser",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        Log(L"CreateWindowEx failed: %lu", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, show);
    SetTitleBarColors();
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    UpdateWindow(hwnd);

    /* Native ad blocking lives on the UI thread (adblock-rust handles are thread
     * affine).  Warm start loads a compiled engine cache in milliseconds; a
     * first run with cached lists takes ~1s; with no lists this returns at once
     * and the filter lists are downloaded in the background, so browsing never
     * waits for the network. */
    {
        ZephiumAdblockConfig adblock;
        adblock.hwnd = hwnd;
        adblock.exe_dir = g_exe_dir;
        adblock.enabled = g_adblock_enabled;
        adblock.logger = AdblockLogLine;
        zephium_adblock_start(adblock);
    }

    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!loader) {
        Log(L"WebView2Loader.dll not found");
        MessageBoxW(hwnd, L"WebView2Loader.dll is missing next to zephium_win.exe.",
                    L"Zephium", MB_ICONERROR | MB_OK);
        return 1;
    }
    typedef HRESULT(STDMETHODCALLTYPE *CreateEnvFn)(
        PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    CreateEnvFn create_env = nullptr;
    FARPROC proc = GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    memcpy(&create_env, &proc, sizeof(create_env));
    if (!create_env) {
        Log(L"CreateCoreWebView2EnvironmentWithOptions missing");
        return 1;
    }

    std::wstring user_data = g_exe_dir + L"\\zephium_webview2_profile";
    CreateDirectoryW(user_data.c_str(), nullptr);

    class EnvHandler : public ZephiumCom<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                         &kIID_EnvCompleted> {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override {
            if (FAILED(result) || !env) {
                Log(L"environment creation failed: 0x%08lx", (unsigned long)result);
                MessageBoxW(g_hwnd,
                            L"Could not create the WebView2 environment.\n"
                            L"Install the Microsoft Edge WebView2 Runtime and try again.",
                            L"Zephium", MB_ICONERROR | MB_OK);
                PostQuitMessage(1);
                return S_OK;
            }
            g_env = env;
            env->AddRef();
            Log(L"environment ready");
            AddTab(g_initial_url, true, true);
            SetTimer(g_hwnd, ZEPHIUM_TIMER_ID, ZEPHIUM_TIMER_MS, nullptr);
            return S_OK;
        }
    };

    ZephiumEnvOptions *options = new ZephiumEnvOptions(ZEPHIUM_WEBVIEW2_ARGS);
    HRESULT hr = create_env(nullptr, user_data.c_str(), options, new EnvHandler());
    options->Release();
    if (FAILED(hr)) {
        Log(L"create_env failed: 0x%08lx", (unsigned long)hr);
        MessageBoxW(hwnd, L"Could not start the WebView2 environment.",
                    L"Zephium", MB_ICONERROR | MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == g_url && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                NavigateFromEdit();
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                GiveViewFocus(ActiveTab());
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Same thread as zephium_adblock_start(): stops the download thread, logs the
     * final filtering stats and frees the engine handle. */
    zephium_adblock_shutdown();
    if (g_env) g_env->Release();
    if (g_core) zephium_browser_free(g_core);
    CoUninitialize();
    Log(L"---- exit ----");
    return 0;
}
