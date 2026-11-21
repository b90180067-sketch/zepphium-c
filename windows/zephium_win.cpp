/* =============================================================================
 * Zephium Browser - native Windows shell (Win32 + WebView2 / Edge Chromium)
 *
 * WebKitGTK does not exist on Windows, so this shell embeds the WebView2 engine
 * instead and reuses the portable C17 core (zoom, navigation, SQLite history).
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

#include "WebView2.h"

extern "C" {
#include "../browser_core/port.h"
}

/* ---------------------------------------------------------------- constants */
#define ZEPHIUM_WINDOW_CLASS L"ZephiumWin32Window"
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
static ICoreWebView2Controller *g_ctrl = nullptr;
static ICoreWebView2 *g_view = nullptr;
static zephium_browser_context_t *g_core = nullptr;

static int g_zoom_percent = 100;
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

/* Minimal ICoreWebView2EnvironmentOptions: the SDK helper needs WRL, which
 * MinGW does not ship. */
static HRESULT CoStrDup(const std::wstring &src, LPWSTR *out) {
    if (!out) return E_POINTER;
    size_t bytes = (src.size() + 1) * sizeof(wchar_t);
    LPWSTR copy = (LPWSTR)CoTaskMemAlloc(bytes);
    if (!copy) return E_OUTOFMEMORY;
    memcpy(copy, src.c_str(), bytes);
    *out = copy;
    return S_OK;
}

static const GUID kIID_IUnknown_ = {0x00000000, 0x0000, 0x0000,
                                   {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_EnvOptions = {0x2fde08a8, 0x1e9a, 0x4766,
                                     {0x8c, 0x05, 0x95, 0xa9, 0xce, 0xb9, 0xd1, 0xc5}};
static const GUID kIID_EnvCompleted = {0x4e8a3389, 0xc9d8, 0x4bd2,
                                       {0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d}};
static const GUID kIID_CtrlCompleted = {0x6c4819f3, 0xc9b7, 0x4260,
                                        {0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c}};

static inline bool GuidEq(REFIID a, const GUID &b) {
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

/* COM base: the SDK's helper classes need WRL, which MinGW does not ship. A
 * correct QueryInterface is mandatory - returning E_NOINTERFACE for IUnknown
 * makes CreateCoreWebView2EnvironmentWithOptions fail with E_NOINTERFACE. */
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

class ZephiumEnvOptions : public ZephiumCom<ICoreWebView2EnvironmentOptions, &kIID_EnvOptions> {
public:
    /* Must match the SDK: TargetCompatibleBrowserVersion has no empty default,
     * an empty value makes CreateCoreWebView2EnvironmentWithOptions fail with
     * E_INVALIDARG. Keep in sync with CORE_WEBVIEW_TARGET_PRODUCT_VERSION. */
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

/* --------------------------------------------------------------- functions */
static void LayoutChildren() {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int width = rc.right - rc.left;
    int top = 4;
    int height = 26;
    int x = 6;

    HWND buttons[3] = {g_back, g_forward, g_reload};
    const int btn_w[3] = {32, 32, 34};
    for (int i = 0; i < 3; ++i) {
        if (buttons[i]) MoveWindow(buttons[i], x, top, btn_w[i], height, TRUE);
        x += btn_w[i] + 4;
    }

    int right_w = 32 + 4 + 56 + 4 + 32 + 6;   /* zoom -, label, zoom + */
    int url_w = width - x - right_w - 8;
    if (url_w < 120) url_w = 120;
    if (g_url) MoveWindow(g_url, x, top, url_w, height, TRUE);
    x += url_w + 6;

    if (g_zoom_out) MoveWindow(g_zoom_out, x, top, 32, height, TRUE);
    x += 36;
    if (g_zoom_label) MoveWindow(g_zoom_label, x, top, 56, height, TRUE);
    x += 60;
    if (g_zoom_in) MoveWindow(g_zoom_in, x, top, 32, height, TRUE);

    if (g_ctrl) {
        RECT web = {0, 0, width, rc.bottom - rc.top};
        web.top = ZEPHIUM_TOOLBAR_HEIGHT;
        if (web.bottom < web.top) web.bottom = web.top;
        g_ctrl->put_Bounds(web);
    }
}

static void UpdateZoomLabel() {
    if (!g_zoom_label) return;
    wchar_t buf[32];
    swprintf(buf, 32, L"%d%%", g_zoom_percent);
    SetWindowTextW(g_zoom_label, buf);
}

static void ApplyZoom(int percent) {
    if (percent < 25) percent = 25;
    if (percent > 500) percent = 500;
    g_zoom_percent = percent;
    if (g_ctrl) g_ctrl->put_ZoomFactor((double)percent / 100.0);
    UpdateZoomLabel();
}

static void NavigateToResolved(const std::wstring &raw) {
    if (raw.empty() || !g_core) return;

    std::string input = WideToUtf8(raw);
    if (!zephium_browser_navigate(g_core, input.c_str())) {
        Log(L"navigation refused by core: %ls", raw.c_str());
        return;
    }
    zephium_tab_t *tab = zephium_browser_get_active_tab(g_core);
    if (!tab) return;
    std::wstring resolved = Utf8ToWide(tab->current_url);
    Log(L"navigate: %ls -> %ls", raw.c_str(), resolved.c_str());
    if (g_view) {
        g_view->Navigate(resolved.c_str());
        if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

static void NavigateFromEdit() {
    wchar_t buf[4096] = {0};
    GetWindowTextW(g_url, buf, 4096);
    NavigateToResolved(buf);
}

static void SyncFromView() {
    if (!g_view) return;
    LPWSTR src = nullptr;
    if (SUCCEEDED(g_view->get_Source(&src)) && src) {
        if (GetFocus() != g_url) SetWindowTextW(g_url, src);
        CoTaskMemFree(src);
    }
    LPWSTR title = nullptr;
    if (SUCCEEDED(g_view->get_DocumentTitle(&title)) && title) {
        std::wstring full = std::wstring(title) + L" - Zephium Browser";
        wchar_t current[512] = {0};
        GetWindowTextW(g_hwnd, current, 512);
        if (wcscmp(current, full.c_str()) != 0) SetWindowTextW(g_hwnd, full.c_str());
        CoTaskMemFree(title);
    }
    BOOL can = FALSE;
    if (SUCCEEDED(g_view->get_CanGoBack(&can))) EnableWindow(g_back, can);
    if (SUCCEEDED(g_view->get_CanGoForward(&can))) EnableWindow(g_forward, can);
}

/* ----------------------------------------------------------- COM handlers */
class ZephiumControllerHandler
    : public ZephiumCom<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, &kIID_CtrlCompleted> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override;
};

class ZephiumEnvHandler
    : public ZephiumCom<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, &kIID_EnvCompleted> {
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
        Log(L"environment ready; creating controller");
        HRESULT hr = env->CreateCoreWebView2Controller(g_hwnd, new ZephiumControllerHandler());
        if (FAILED(hr)) Log(L"CreateCoreWebView2Controller failed: 0x%08lx", (unsigned long)hr);
        return S_OK;
    }
};

HRESULT STDMETHODCALLTYPE ZephiumControllerHandler::Invoke(HRESULT result, ICoreWebView2Controller *controller) {
    if (FAILED(result) || !controller) {
        Log(L"controller creation failed: 0x%08lx", (unsigned long)result);
        MessageBoxW(g_hwnd, L"Could not create the WebView2 controller.",
                    L"Zephium", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
        return S_OK;
    }

    g_ctrl = controller;
    controller->AddRef();
    controller->put_IsVisible(TRUE);
    controller->put_ZoomFactor((double)g_zoom_percent / 100.0);

    if (FAILED(controller->get_CoreWebView2(&g_view)) || !g_view) {
        Log(L"get_CoreWebView2 failed");
        PostQuitMessage(1);
        return S_OK;
    }
    g_view->AddRef();

    ICoreWebView2Settings *settings = nullptr;
    if (SUCCEEDED(g_view->get_Settings(&settings)) && settings) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_AreDefaultContextMenusEnabled(TRUE);
        settings->put_IsBuiltInErrorPageEnabled(TRUE);
        settings->Release();
    }

    LayoutChildren();
    Log(L"webview ready; loading %ls", g_initial_url.c_str());
    if (!g_initial_url.empty()) g_view->Navigate(g_initial_url.c_str());

    SetTimer(g_hwnd, ZEPHIUM_TIMER_ID, ZEPHIUM_TIMER_MS, nullptr);
    SyncFromView();
    return S_OK;
}

/* ------------------------------------------------------------- window proc */
static LRESULT CALLBACK ZephiumWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        HINSTANCE inst = ((LPCREATESTRUCTW)lparam)->hInstance;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_back = CreateWindowExW(0, L"BUTTON", L"\u2190", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hwnd, (HMENU)ID_BACK, inst, nullptr);
        g_forward = CreateWindowExW(0, L"BUTTON", L"\u2192", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, hwnd, (HMENU)ID_FORWARD, inst, nullptr);
        g_reload = CreateWindowExW(0, L"BUTTON", L"\u21BB", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)ID_RELOAD, inst, nullptr);
        g_url = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                0, 0, 0, 0, hwnd, (HMENU)ID_URL, inst, nullptr);
        g_zoom_out = CreateWindowExW(0, L"BUTTON", L"-", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 0, 0, hwnd, (HMENU)ID_ZOOM_OUT, inst, nullptr);
        g_zoom_label = CreateWindowExW(0, L"STATIC", L"100%",
                                       WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                       0, 0, 0, 0, hwnd, (HMENU)ID_ZOOM_LABEL, inst, nullptr);
        g_zoom_in = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, hwnd, (HMENU)ID_ZOOM_IN, inst, nullptr);

        HWND controls[] = {g_back, g_forward, g_reload, g_url, g_zoom_out, g_zoom_label, g_zoom_in};
        for (HWND c : controls) {
            if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        }
        EnableWindow(g_back, FALSE);
        EnableWindow(g_forward, FALSE);
        UpdateZoomLabel();
        LayoutChildren();
        return 0;
    }

    case WM_SIZE:
        LayoutChildren();
        return 0;

    case WM_TIMER:
        if (wparam == ZEPHIUM_TIMER_ID) SyncFromView();
        return 0;

    case WM_COMMAND: {
        switch (LOWORD(wparam)) {
        case ID_BACK:
            Log(L"action: back");
            if (g_view) g_view->GoBack();
            return 0;
        case ID_FORWARD:
            Log(L"action: forward");
            if (g_view) g_view->GoForward();
            return 0;
        case ID_RELOAD:
            Log(L"action: reload");
            if (g_view) g_view->Reload();
            return 0;
        case ID_ZOOM_IN:
            if (g_core) ApplyZoom((int)zephium_browser_step_zoom(g_core, true));
            Log(L"action: zoom in -> %d%%", g_zoom_percent);
            return 0;
        case ID_ZOOM_OUT:
            if (g_core) ApplyZoom((int)zephium_browser_step_zoom(g_core, false));
            Log(L"action: zoom out -> %d%%", g_zoom_percent);
            return 0;
        default:
            break;
        }
        return 0;
    }

    case WM_SETFOCUS:
        if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ZEPHIUM_TIMER_ID);
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
    if (argv && argc > 1 && argv[1][0]) initial = argv[1];
    if (argv) LocalFree(argv);

    std::string db_path = WideToUtf8(g_exe_dir + L"\\zephium_profile.db");
    g_core = zephium_browser_init(db_path.c_str());
    if (!g_core) {
        Log(L"core init failed");
        MessageBoxW(nullptr, L"Could not open the SQLite profile database.",
                    L"Zephium", MB_ICONERROR | MB_OK);
        return 1;
    }
    zephium_tab_t *tab = zephium_browser_get_tab(g_core, 0);
    if (tab && tab->zoom_percent) g_zoom_percent = tab->zoom_percent;

    std::string initial_utf8 = WideToUtf8(initial);
    if (!zephium_browser_navigate(g_core, initial_utf8.c_str())) {
        Log(L"initial navigation refused: %ls", initial.c_str());
    }
    tab = zephium_browser_get_active_tab(g_core);
    g_initial_url = tab ? Utf8ToWide(tab->current_url) : initial;

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

    /* The loader is shipped next to the executable (see windows/Makefile). */
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

    ZephiumEnvOptions *options = new ZephiumEnvOptions(
        L"--autoplay-policy=no-user-gesture-required");
    HRESULT hr = create_env(nullptr, user_data.c_str(), options, new ZephiumEnvHandler());
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
                if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_view) g_view->Release();
    if (g_ctrl) g_ctrl->Release();
    if (g_env) g_env->Release();
    if (g_core) zephium_browser_free(g_core);
    CoUninitialize();
    Log(L"---- exit ----");
    return 0;
}
