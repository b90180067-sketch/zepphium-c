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
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

#include "WebView2.h"

extern "C" {
#include "../browser_core/port.h"
}

/* ---------------------------------------------------------------- constants */
#define ZEPHIUM_WINDOW_CLASS L"ZephiumWin32TabbedWindow"
#define ZEPHIUM_TABBAR_HEIGHT 30
#define ZEPHIUM_TOOLBAR_HEIGHT 36
#define ZEPHIUM_TIMER_ID 1
#define ZEPHIUM_TIMER_MS 400

#define ID_BACK 1001
#define ID_FORWARD 1002
#define ID_RELOAD 1003
#define ID_URL 1004
#define ID_ZOOM_OUT 1005
#define ID_ZOOM_IN 1006
#define ID_ZOOM_LABEL 1007
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

static ICoreWebView2Environment *g_env = nullptr;
static zephium_browser_context_t *g_core = nullptr;

static std::vector<ZephiumTab *> g_tabs;
static int g_active = -1;
static int g_next_tab_id = 0;
static TabGeometry g_geo;
static POINT g_mouse = {-1, -1};

static int g_zoom_percent = 100;
static int g_sleep_seconds = 600;          /* 0 disables hibernation */
static std::wstring g_initial_url;
static std::wstring g_exe_dir;
static std::wstring g_log_path;

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

static std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

static RECT ContentRect() {
    RECT rc = {0, 0, 0, 0};
    if (!g_hwnd) return rc;
    GetClientRect(g_hwnd, &rc);
    rc.top = ZEPHIUM_TABBAR_HEIGHT + ZEPHIUM_TOOLBAR_HEIGHT;
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

class ZephiumEnvOptions : public ZephiumCom<ICoreWebView2EnvironmentOptions, &kIID_EnvOptions> {
public:
    /* Must match the SDK: an empty TargetCompatibleBrowserVersion makes
     * CreateCoreWebView2EnvironmentWithOptions fail with E_INVALIDARG. */
    static constexpr wchar_t kTargetVersion[] = L"158.0.4370.26";

    explicit ZephiumEnvOptions(const std::wstring &args) : m_args(args), m_version(kTargetVersion) {}

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

static void DrawTabStrip(HDC dc, int client_width) {
    ComputeTabGeometry(client_width);

    RECT strip = {0, 0, client_width, ZEPHIUM_TABBAR_HEIGHT};
    HBRUSH background = CreateSolidBrush(RGB(232, 232, 232));
    FillRect(dc, &strip, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old_font = SelectObject(dc, font);

    for (size_t i = 0; i < g_tabs.size(); ++i) {
        const RECT &r = g_geo.tabs[i];
        const bool active = ((int)i == g_active);
        const bool hovered = PtInRect(&r, g_mouse) != 0;

        COLORREF face = active ? RGB(255, 255, 255) : (hovered ? RGB(242, 242, 242) : RGB(216, 216, 216));
        HBRUSH brush = CreateSolidBrush(face);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));

        std::wstring label = g_tabs[i]->sleeping ? (L"\u23FE  " + g_tabs[i]->title) : g_tabs[i]->title;
        if (label.empty()) label = L"New Tab";
        RECT text = {r.left + 8, r.top, r.right - 22, r.bottom};
        SetTextColor(dc, active ? RGB(0, 0, 0) : RGB(90, 90, 90));
        DrawTextW(dc, label.c_str(), -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        const RECT &close = g_geo.closes[i];
        const bool close_hovered = PtInRect(&close, g_mouse) != 0;
        if (close_hovered) {
            HBRUSH hot = CreateSolidBrush(RGB(232, 120, 120));
            FillRect(dc, &close, hot);
            DeleteObject(hot);
        }
        SetTextColor(dc, close_hovered ? RGB(255, 255, 255) : RGB(80, 80, 80));
        DrawTextW(dc, L"\u00D7", -1, (RECT *)&close, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    const bool plus_hovered = PtInRect(&g_geo.plus, g_mouse) != 0;
    HBRUSH plus_brush = CreateSolidBrush(plus_hovered ? RGB(255, 255, 255) : RGB(222, 222, 222));
    FillRect(dc, &g_geo.plus, plus_brush);
    DeleteObject(plus_brush);
    FrameRect(dc, &g_geo.plus, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetTextColor(dc, RGB(40, 40, 40));
    DrawTextW(dc, L"+", -1, &g_geo.plus, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    SelectObject(dc, old_font);
}

/* --------------------------------------------------------------- functions */
static void LayoutChildren() {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    const int width = rc.right - rc.left;
    const int top = ZEPHIUM_TABBAR_HEIGHT + 4;
    const int height = 26;
    int x = 6;

    HWND buttons[3] = {g_back, g_forward, g_reload};
    const int btn_w[3] = {32, 32, 34};
    for (int i = 0; i < 3; ++i) {
        if (buttons[i]) MoveWindow(buttons[i], x, top, btn_w[i], height, TRUE);
        x += btn_w[i] + 4;
    }

    const int right_w = 32 + 4 + 56 + 4 + 32 + 6;
    int url_w = width - x - right_w - 8;
    if (url_w < 120) url_w = 120;
    if (g_url) MoveWindow(g_url, x, top, url_w, height, TRUE);
    x += url_w + 6;
    if (g_zoom_out) MoveWindow(g_zoom_out, x, top, 32, height, TRUE);
    x += 36;
    if (g_zoom_label) MoveWindow(g_zoom_label, x, top, 56, height, TRUE);
    x += 60;
    if (g_zoom_in) MoveWindow(g_zoom_in, x, top, 32, height, TRUE);

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
            if (observed != tab->url) {
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
    tab->title = L"New Tab";
    tab->last_active = GetTickCount64();

    g_tabs.push_back(tab);
    const int index = (int)g_tabs.size() - 1;
    Log(L"new tab %d id=%d url=%ls (core tabs=%zu)", index, tab->id, tab->url.c_str(), g_core->tab_count);

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
    tab->ctrl->Close();
    tab->ctrl->Release();
    tab->ctrl = nullptr;
    if (tab->view) {
        tab->view->Release();
        tab->view = nullptr;
    }
    Log(L"tab %d hibernated, url=%ls", index, tab->url.c_str());
    UpdateTabStrip();
}

static void WakeTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    ZephiumTab *tab = g_tabs[(size_t)index];
    if (!tab || !tab->sleeping) return;
    tab->sleeping = false;
    tab->creating = true;
    Log(L"tab %d waking, url=%ls", index, tab->url.c_str());
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
    if (changed) Log(L"activate tab %d (%ls)", index, tab->url.c_str());
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
        AddTab(L"about:blank", true, false);
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
        Log(L"navigation refused by core: %ls", raw.c_str());
        return;
    }
    zephium_tab_t *core_tab = zephium_browser_get_tab(g_core, (size_t)g_active);
    std::wstring resolved = core_tab ? Utf8ToWide(core_tab->current_url) : raw;
    tab->url = resolved;
    Log(L"navigate tab %d: %ls -> %ls", g_active, raw.c_str(), resolved.c_str());

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

    const int index = (int)(std::find(g_tabs.begin(), g_tabs.end(), tab) - g_tabs.begin());
    zephium_tab_t *core_tab = g_core ? zephium_browser_get_tab(g_core, (size_t)index) : nullptr;
    const int zoom = (core_tab && core_tab->zoom_percent) ? (int)core_tab->zoom_percent : 100;
    controller->put_ZoomFactor((double)zoom / 100.0);
    controller->put_Bounds(ContentRect());
    controller->put_IsVisible(index == g_active ? TRUE : FALSE);

    if (!tab->url.empty() && tab->url != L"about:blank") {
        tab->view->Navigate(tab->url.c_str());
    }
    Log(L"tab %d ready (id=%d, url=%ls)", index, tab->id, tab->url.c_str());

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
        AddTab(L"about:blank", true, false);
        args->put_Handled(TRUE);
    } else if (ctrl && key == 'W') {
        CloseTab(g_active);
        args->put_Handled(TRUE);
    } else if (ctrl && key == VK_TAB) {
        NextTab(shift ? -1 : 1);
        args->put_Handled(TRUE);
    } else if (ctrl && key == 'L') {
        SetFocus(g_url);
        SendMessageW(g_url, EM_SETSEL, 0, -1);
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

/* ------------------------------------------------------------- window proc */
static LRESULT CALLBACK ZephiumWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        HINSTANCE inst = ((LPCREATESTRUCTW)lparam)->hInstance;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        const int top = ZEPHIUM_TABBAR_HEIGHT + 4;
        g_back = CreateWindowExW(0, L"BUTTON", L"\u2190", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 6, top, 32, 26, hwnd, (HMENU)ID_BACK, inst, nullptr);
        g_forward = CreateWindowExW(0, L"BUTTON", L"\u2192", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    42, top, 32, 26, hwnd, (HMENU)ID_FORWARD, inst, nullptr);
        g_reload = CreateWindowExW(0, L"BUTTON", L"\u21BB", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   78, top, 34, 26, hwnd, (HMENU)ID_RELOAD, inst, nullptr);
        g_url = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                118, top, 600, 26, hwnd, (HMENU)ID_URL, inst, nullptr);
        g_zoom_out = CreateWindowExW(0, L"BUTTON", L"-", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, top, 32, 26, hwnd, (HMENU)ID_ZOOM_OUT, inst, nullptr);
        g_zoom_label = CreateWindowExW(0, L"STATIC", L"100%",
                                       WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                       0, top, 56, 26, hwnd, (HMENU)ID_ZOOM_LABEL, inst, nullptr);
        g_zoom_in = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, top, 32, 26, hwnd, (HMENU)ID_ZOOM_IN, inst, nullptr);

        HWND controls[] = {g_back, g_forward, g_reload, g_url, g_zoom_out, g_zoom_label, g_zoom_in};
        for (HWND control : controls) {
            if (control) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
        }
        EnableWindow(g_back, FALSE);
        EnableWindow(g_forward, FALSE);
        UpdateZoomLabel();
        return 0;
    }

    case WM_SIZE:
        LayoutChildren();
        UpdateTabStrip();
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* the tab strip is painted (double buffered) in WM_PAINT */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, rc.right, ZEPHIUM_TABBAR_HEIGHT);
        HGDIOBJ old = SelectObject(memory, bitmap);
        DrawTabStrip(memory, rc.right);
        BitBlt(dc, 0, 0, rc.right, ZEPHIUM_TABBAR_HEIGHT, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.y < ZEPHIUM_TABBAR_HEIGHT) {
            RECT client;
            GetClientRect(hwnd, &client);
            ComputeTabGeometry(client.right);
            if (PtInRect(&g_geo.plus, pt)) {
                AddTab(L"about:blank", true, false);
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
        }
        return 0;

    case WM_COMMAND: {
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
        case ID_ZOOM_IN:
            ZoomStep(true);
            return 0;
        case ID_ZOOM_OUT:
            ZoomStep(false);
            return 0;
        case ID_NEW_TAB:
            AddTab(L"about:blank", true, false);
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

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring initial = L"https://duckduckgo.com";
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--tab-sleep") == 0 && i + 1 < argc) {
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

    std::string db_path = WideToUtf8(g_exe_dir + L"\\zephium_profile.db");
    g_core = zephium_browser_init(db_path.c_str());
    if (!g_core) {
        Log(L"core init failed");
        MessageBoxW(nullptr, L"Could not open the SQLite profile database.",
                    L"Zephium", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ZephiumWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
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
    UpdateWindow(hwnd);

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

    if (g_env) g_env->Release();
    if (g_core) zephium_browser_free(g_core);
    CoUninitialize();
    Log(L"---- exit ----");
    return 0;
}
