#if defined(__linux__)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

#include "browser_core/port.h"

#if defined(__linux__)
#include <unistd.h>
#include <sys/stat.h>
#endif

static FILE *g_log_file = NULL;
static const char *g_log_path = NULL;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void zephium_log_init(void) {
    if (g_log_file) return;

#if defined(__linux__)
    /* The log records which pages and media were loaded, so it belongs to this
     * user only. umask 077 covers the creation, and the chmod() below also
     * tightens a log left at 0644 by an older build. */
    mode_t saved_umask = umask(0077);
#endif

    /* Truncate on start: read_log.py must analyse one session only. Appending
     * kept stale entries from previous runs, so old failures were reported as
     * if they were happening right now. */
    g_log_file = fopen("/tmp/zephium_debug.log", "w");
    if (g_log_file) {
        g_log_path = "/tmp/zephium_debug.log";
    } else {
        g_log_file = fopen("zephium_debug.log", "w");
        if (g_log_file) g_log_path = "zephium_debug.log";
    }

#if defined(__linux__)
    umask(saved_umask);
    if (g_log_path) (void)chmod(g_log_path, 0600);
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
__attribute__((format(printf, 3, 4)))
#endif
static void zephium_log(const char *level, const char *category, const char *fmt, ...) {
    zephium_log_init();

    char time_buf[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    } else {
        (void)zephium_strcpy_safe(time_buf, sizeof(time_buf), "unknown");
    }

    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (strcmp(level, "ERROR") == 0) {
        fprintf(stderr, "\033[1;31m[%s] [%s] [%s] %s\033[0m\n", time_buf, level, category, msg);
    } else if (strcmp(level, "WARN") == 0) {
        fprintf(stderr, "\033[1;33m[%s] [%s] [%s] %s\033[0m\n", time_buf, level, category, msg);
    } else if (strcmp(level, "DEBUG") == 0) {
        fprintf(stdout, "\033[0;37m[%s] [%s] [%s] %s\033[0m\n", time_buf, level, category, msg);
    } else {
        fprintf(stdout, "\033[1;36m[%s] [%s] [%s] %s\033[0m\n", time_buf, level, category, msg);
    }

    if (g_log_file) {
        fprintf(g_log_file, "[%s] [%s] [%s] %s\n", time_buf, level, category, msg);
        fflush(g_log_file);
    }
}

static bool is_running_on_wsl(void) {
#if defined(__linux__)
    if (getenv("WSL_DISTRO_NAME") != NULL || getenv("WSL_INTEROP") != NULL) {
        return true;
    }
    FILE *f = fopen("/proc/version", "r");
    if (f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        if (strstr(buf, "microsoft") != NULL || strstr(buf, "WSL") != NULL || strstr(buf, "Microsoft") != NULL) {
            return true;
        }
    }
#endif
    return false;
}

#if defined(__linux__)
/* The WebKit sandbox is enabled by default. ZEPHIUM_NO_SANDBOX=1 is the single
 * opt-out, and it exists only for hosts where WebKit's bubblewrap launcher
 * cannot start (some WSL2 kernels refuse unprivileged user namespaces). WebKit's
 * own escape-hatch variable is still honoured, so a shell that exports it gets
 * the warning below instead of silently running unsandboxed. */
static bool zephium_sandbox_disabled(void) {
    const char *opt_out = getenv("ZEPHIUM_NO_SANDBOX");
    if (opt_out && strcmp(opt_out, "1") == 0) return true;

    const char *webkit_var = getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    return webkit_var && *webkit_var && strcmp(webkit_var, "0") != 0;
}
#endif /* __linux__ */

/* GStreamer autoplugs NVIDIA (NVDEC) and VA-API video decoders before the CPU
 * ones. On WSLg the CUDA stack is incomplete (libnvrtc is missing) and Mesa runs
 * on llvmpipe, so those elements cannot create a GL context: the first frame
 * stalls, and for AV1 nvav1dec drops every frame until the clock runs away.
 * Demote them so WebKit uses the software decoders that actually work. */
#if defined(__linux__)
static const char *ZEPHIUM_SOFTWARE_VIDEO_RANKS =
    "vaapi:NONE,vaapidecode:NONE,vaapisink:NONE,vaapipostproc:NONE,"
    "vah264dec:NONE,vah265dec:NONE,vavp8dec:NONE,vavp9dec:NONE,vaav1dec:NONE,"
    "nvdec:NONE,nvenc:NONE,nvav1dec:NONE,nvh264dec:NONE,nvh265dec:NONE,"
    "nvvp8dec:NONE,nvvp9dec:NONE,nvjpegdec:NONE,"
    "nvmpeg2videodec:NONE,nvmpeg4videodec:NONE";

static void zephium_demote_hardware_decoders(void) {
    static bool applied = false;
    if (applied) return;
    applied = true;

    const char *existing = getenv("GST_PLUGIN_FEATURE_RANK");
    char buf[1024];
    if (existing && *existing) {
        snprintf(buf, sizeof(buf), "%s,%s", ZEPHIUM_SOFTWARE_VIDEO_RANKS, existing);
    } else {
        snprintf(buf, sizeof(buf), "%s", ZEPHIUM_SOFTWARE_VIDEO_RANKS);
    }
    setenv("GST_PLUGIN_FEATURE_RANK", buf, 1);
}
#endif /* __linux__ */

static void apply_wsl_optimizations(bool force_wsl, bool disable_gpu, bool force_x11, bool force_wayland) {
#if defined(__linux__)
    bool on_wsl = force_wsl || is_running_on_wsl();
    if (on_wsl) {
        printf("[Zephium] WSL/WSLg environment detected: applying compatibility tuning...\n");

        /* 1. Keep WebKit's bubblewrap sandbox on. Some WSL2 kernels cannot create
         * unprivileged user namespaces, which is why this used to be disabled
         * unconditionally - but the sandbox is a security boundary, so disabling
         * it is an explicit opt-out (ZEPHIUM_NO_SANDBOX=1) with a warning. */
        if (zephium_sandbox_disabled()) {
            setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
        }

        /* 2. Keep accelerated compositing AND the DMA-BUF renderer enabled.
         * WebKitGTK >= 2.4x delivers decoded video frames to the UI process through
         * the compositor. With WEBKIT_DISABLE_COMPOSITING_MODE=1 (or
         * WEBKIT_DISABLE_DMABUF_RENDERER=1) the UI process segfaults a few seconds
         * after a YouTube MSE player starts and the <video> element never shows a
         * frame - "cannot load YouTube video". Do not set those variables. */

        /* 3. Keep software GL (LLVMpipe) while compositing is enabled, so the WSLg
         * Mesa D3D12/DRI3 driver bugs (missing glyphs, broken DRI3) stay away. */
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe", 0);

        /* 4. Use software video decoding: VA-API and NVDEC cannot present frames
         * under WSLg (no CUDA runtime, llvmpipe renderer). */
        zephium_demote_hardware_decoders();
        setenv("LIBVA_DRIVER_NAME", "none", 0);
        setenv("WEBKIT_GST_HOLE_PUNCH", "0", 0);

        /* 5. Redirect cache and data directories to Linux native /tmp (tmpfs) to avoid NTFS DrvFs fallocate crashes.
         * 0700: XDG_DATA_HOME holds cookies, localStorage and the HTTP cache, i.e.
         * the user's logged-in sessions. mkdir() does not change an existing
         * directory, hence the chmod() for trees created by an older build. */
        setenv("XDG_CACHE_HOME", "/tmp/zephium-cache", 0);
        setenv("XDG_DATA_HOME", "/tmp/zephium-data", 0);
        mkdir("/tmp/zephium-cache", 0700);
        mkdir("/tmp/zephium-data", 0700);
        (void)chmod("/tmp/zephium-cache", 0700);
        (void)chmod("/tmp/zephium-data", 0700);

        /* 6. Display server selection for WSLg */
        if (force_x11) {
            setenv("GDK_BACKEND", "x11", 1);
        } else if (force_wayland) {
            setenv("GDK_BACKEND", "wayland", 1);
        } else if (!getenv("GDK_BACKEND")) {
            /* WSLg supports both X11 and Wayland; X11 is significantly more stable for WebKitGTK sub-surfaces */
            setenv("GDK_BACKEND", "x11", 0);
        }

        /* 7. Display variable fallback for WSLg if not explicitly exported */
        if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
            if (access("/mnt/wslg", F_OK) == 0 || access("/tmp/.X11-unix/X0", F_OK) == 0) {
                setenv("DISPLAY", ":0", 0);
            }
        }

        /* 8. Record the render configuration: YouTube video needs both the
         * compositor and the DMA-BUF renderer, and the log reader reports it. */
        zephium_log("INFO", "RENDER_ENV",
                    "compositing=%s dmabuf=%s gl=%s webgl=enabled display=%s",
                    getenv("WEBKIT_DISABLE_COMPOSITING_MODE") ? "disabled" : "enabled",
                    getenv("WEBKIT_DISABLE_DMABUF_RENDERER") ? "disabled" : "enabled",
                    getenv("LIBGL_ALWAYS_SOFTWARE") ? "software/llvmpipe" : "driver-default",
                    getenv("DISPLAY") ? getenv("DISPLAY") :
                        (getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "none"));
    } else if (disable_gpu) {
        /* Software GL only. Accelerated compositing stays enabled because it is
         * the path that presents video frames (see the comment above). */
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe", 0);
    }
#else
    (void)force_wsl;
    (void)disable_gpu;
    (void)force_x11;
    (void)force_wayland;
#endif
}

#if defined(__linux__) && defined(ZEPHIUM_ENABLE_WEBKITGTK)
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *entry_url;
    GtkWidget *lbl_zoom;
    GtkWidget *btn_back;
    GtkWidget *btn_forward;
    GtkWidget *btn_reload;
    GtkWidget *find_bar;       /* Ctrl+F find-in-page strip, hidden by default */
    GtkWidget *find_entry;
    GtkWidget *lbl_find;
    zephium_browser_context_t *core;
    WebKitUserContentFilter *adblock_filter;
    bool disable_gpu;
    bool is_wsl;
    const char *screenshot_path;
    int screenshot_delay;
} zephium_gtk_app_t;

static zephium_gtk_app_t g_app;

static WebKitWebView *get_active_web_view(zephium_gtk_app_t *app);
static GtkWidget *create_tab_view(zephium_gtk_app_t *app, const char *url, bool reuse_initial_tab);
static void on_find_found(WebKitFindController *controller, guint match_count, gpointer user_data);
static void on_find_not_found(WebKitFindController *controller, gpointer user_data);

static void update_active_tab_ui(zephium_gtk_app_t *app) {
    gint page_idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (page_idx < 0) return;
    app->core->active_tab_index = (size_t)page_idx;

    zephium_tab_t *tab = zephium_browser_get_active_tab(app->core);
    if (!tab) return;

    /* Only touch the widgets when the value really changed: notify::uri/title fire
     * often, and gtk_entry_set_text() resets the caret and repaints the toolbar. */
    const gchar *entry_text = gtk_entry_get_text(GTK_ENTRY(app->entry_url));
    if (!entry_text || strcmp(entry_text, tab->current_url) != 0) {
        gtk_entry_set_text(GTK_ENTRY(app->entry_url), tab->current_url);
    }

    char zoom_buf[32];
    snprintf(zoom_buf, sizeof(zoom_buf), "%u%%", tab->zoom_percent);
    if (strcmp(gtk_label_get_text(GTK_LABEL(app->lbl_zoom)), zoom_buf) != 0) {
        gtk_label_set_text(GTK_LABEL(app->lbl_zoom), zoom_buf);
    }

    char title_buf[600];
    snprintf(title_buf, sizeof(title_buf), "%s - Zephium Browser", tab->title[0] ? tab->title : "New Tab");
    const gchar *current_title = gtk_window_get_title(GTK_WINDOW(app->window));
    if (!current_title || strcmp(current_title, title_buf) != 0) {
        gtk_window_set_title(GTK_WINDOW(app->window), title_buf);
    }

    /* Grey out the history buttons when there is nothing to go back/forward to. */
    WebKitWebView *view = get_active_web_view(app);
    if (app->btn_back) {
        gtk_widget_set_sensitive(app->btn_back, view && webkit_web_view_can_go_back(view));
    }
    if (app->btn_forward) {
        gtk_widget_set_sensitive(app->btn_forward, view && webkit_web_view_can_go_forward(view));
    }
}

/* ---------------------------------------------------------------------------
 * Page diagnostics bridge
 *
 * WebKitGTK only prints page console output to stdout, so a JS/WebGL failure
 * was invisible to read_log.py. A tiny content script forwards uncaught errors
 * and console.error() to the UI process, which logs them (rate limited) so the
 * diagnostic reader can report them.
 * ------------------------------------------------------------------------- */
#define ZEPHIUM_DIAG_MAX_PER_WINDOW 8
#define ZEPHIUM_DIAG_WINDOW_SECONDS 1
#define ZEPHIUM_DIAG_MAX_TOTAL 200

static const char *ZEPHIUM_DIAG_SCRIPT =
    "(function(){\n"
    "  if (window.__zephiumDiag) return; window.__zephiumDiag = 1;\n"
    "  function send(kind, text) {\n"
    "    try {\n"
    "      var s = String(text == null ? '' : text);\n"
    "      window.webkit.messageHandlers.zephiumDiag.postMessage(kind + ': ' + s.slice(0, 400));\n"
    "    } catch (e) {}\n"
    "  }\n"
    "  window.addEventListener('error', function (e) {\n"
    "    if (!e || !e.message) return;  /* resource load error, not a JS exception */\n"
    "    send('JS_ERROR', e.message + ' @ ' + (e.filename || '') + ':' + (e.lineno || 0));\n"
    "  }, true);\n"
    "  window.addEventListener('unhandledrejection', function (e) {\n"
    "    var r = e && e.reason;\n"
    "    send('JS_REJECTION', (r && (r.message || r.stack)) || String(r));\n"
    "  }, true);\n"
    "  if (window.console && console.error) {\n"
    "    var original = console.error;\n"
    "    console.error = function () {\n"
    "      try { send('CONSOLE_ERROR', Array.prototype.map.call(arguments, function (a) {\n"
    "        return (a && a.message) ? a.message : String(a); }).join(' ')); } catch (e) {}\n"
    "      return original.apply(console, arguments);\n"
    "    };\n"
    "  }\n"
    "})();\n";

static guint g_diag_logged = 0;
static guint g_diag_in_window = 0;
static gint64 g_diag_window_start_us = 0;

static void on_page_diagnostic(WebKitUserContentManager *manager,
                               WebKitJavascriptResult *result,
                               gpointer user_data) {
    (void)manager;
    (void)user_data;

    if (g_diag_logged >= ZEPHIUM_DIAG_MAX_TOTAL) return;

    gint64 now = g_get_monotonic_time();
    if (g_diag_window_start_us == 0 ||
        now - g_diag_window_start_us > (gint64)ZEPHIUM_DIAG_WINDOW_SECONDS * G_USEC_PER_SEC) {
        g_diag_window_start_us = now;
        g_diag_in_window = 0;
    }
    if (g_diag_in_window >= ZEPHIUM_DIAG_MAX_PER_WINDOW) return;

    JSCValue *value = webkit_javascript_result_get_js_value(result);
    if (!value) return;
    char *message = jsc_value_to_string(value);
    if (!message || !*message) {
        g_free(message);
        return;
    }

    g_diag_in_window++;
    g_diag_logged++;
    /* Page errors routinely embed the full page URL (with its query string) or
     * form values, so only the first 200 characters reach the log. */
    zephium_log("WARN", "PAGE_JS_ERROR", "%.200s", message);
    g_free(message);
}

static void install_page_diagnostics(WebKitUserContentManager *ucm) {
    if (!ucm) return;
    if (!webkit_user_content_manager_register_script_message_handler(ucm, "zephiumDiag")) return;
    g_signal_connect(ucm, "script-message-received::zephiumDiag",
                     G_CALLBACK(on_page_diagnostic), NULL);

    WebKitUserScript *script = webkit_user_script_new(
        ZEPHIUM_DIAG_SCRIPT,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    if (script) {
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }
}

/* ---------------------------------------------------------------------------
 * Ad blocking
 *
 * The portable blocker module emits WebKit content-blocker JSON; WebKit needs it
 * compiled into a binary filter, which it does asynchronously in the UI process.
 * Tabs that already exist when the compilation finishes (the first one always
 * does) receive the filter through adblock_apply_to_tabs(). Any failure - rules
 * that do not compile, an unwritable store directory - only costs ad blocking:
 * pages keep loading.
 * ------------------------------------------------------------------------- */
#define ZEPHIUM_ADBLOCK_STORE_IDENTIFIER "zephium-adblock"

static WebKitUserContentFilterStore *g_adblock_store = NULL;

static bool zephium_adblock_enabled(bool cli_disabled) {
    if (cli_disabled) return false;
    const char *value = getenv("ZEPHIUM_ADBLOCK");
    return !(value && strcmp(value, "0") == 0);
}

/* Give the compiled filter to every tab that already exists. remove_all_filters()
 * keeps exactly one copy, including in tabs that were created after the filter
 * became available. */
static void adblock_apply_to_tabs(WebKitUserContentFilter *filter) {
    if (!filter || !g_app.notebook) return;
    gint pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_app.notebook));
    for (gint i = 0; i < pages; ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_app.notebook), i);
        if (!page || !WEBKIT_IS_WEB_VIEW(page)) continue;
        WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(page));
        if (!ucm) continue;
        webkit_user_content_manager_remove_all_filters(ucm);
        webkit_user_content_manager_add_filter(ucm, filter);
    }
}

static void on_adblock_filter_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)user_data;
    GError *error = NULL;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
    if (!filter) {
        /* An ad blocker is never worth an error page: browse without it. */
        zephium_log("WARN", "ADBLOCK_FAILED",
                    "Content filter did not compile (%s); continuing without ad blocking.",
                    error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return;
    }

    g_app.adblock_filter = filter;
    adblock_apply_to_tabs(filter);
    zephium_log("INFO", "ADBLOCK", "Content filter active.");
}

static void adblock_init(bool cli_disabled) {
    if (!zephium_adblock_enabled(cli_disabled)) {
        zephium_log("INFO", "ADBLOCK", "Disabled (ZEPHIUM_ADBLOCK=0 or --no-adblock).");
        return;
    }

    char rules[4096];
    size_t rules_len = zephium_browser_generate_adblock_rules(rules, sizeof(rules));
    if (rules_len == 0 || rules_len >= sizeof(rules)) {
        zephium_log("WARN", "ADBLOCK_FAILED", "Rule generation failed; continuing without ad blocking.");
        return;
    }

    char *store_dir = g_build_filename(g_get_user_cache_dir(), "zephium", "content-filters", NULL);
    if (g_mkdir_with_parents(store_dir, 0700) != 0) {
        zephium_log("WARN", "ADBLOCK_FAILED",
                    "Cannot create the filter store directory %s; continuing without ad blocking.",
                    store_dir);
        g_free(store_dir);
        return;
    }
    g_adblock_store = webkit_user_content_filter_store_new(store_dir);
    g_free(store_dir);

    GBytes *source = g_bytes_new(rules, rules_len);
    webkit_user_content_filter_store_save(g_adblock_store, ZEPHIUM_ADBLOCK_STORE_IDENTIFIER, source,
                                          NULL, on_adblock_filter_ready, NULL);
    g_bytes_unref(source);
    zephium_log("INFO", "ADBLOCK", "Compiling %zu bytes of blocker rules...", rules_len);
}

static WebKitWebView *get_active_web_view(zephium_gtk_app_t *app) {
    gint page_idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (page_idx < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), page_idx);
    if (!page) return NULL;
    /* Pages used to be wrapped in a GtkScrolledWindow, so this returned a
     * GtkViewport: every webkit_web_view_* call then failed its type assertion,
     * which is why back / forward / reload / zoom / omnibox silently did nothing. */
    if (!WEBKIT_IS_WEB_VIEW(page)) return NULL;
    return WEBKIT_WEB_VIEW(page);
}

/* History is written once per committed document instead of on every URI change:
 * each insert is a synchronous SQLite commit on a DrvFs mount, i.e. a 8-26ms
 * stall on the GTK main thread in the middle of a navigation. */
static char g_last_history_uri[2048];

static void record_history_once(const char *uri) {
    if (!uri || !*uri || !g_app.core) return;
    if (strcmp(uri, g_last_history_uri) == 0) return;
    snprintf(g_last_history_uri, sizeof(g_last_history_uri), "%s", uri);
    zephium_browser_navigate(g_app.core, uri);
}

static void on_url_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri) return;

    zephium_tab_t *tab = zephium_browser_get_active_tab(g_app.core);
    if (tab && strcmp(tab->current_url, uri) != 0) {
        (void)zephium_strcpy_safe(tab->current_url, sizeof(tab->current_url), uri);
        update_active_tab_ui(&g_app);
    }
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    (void)user_data;
    if (load_event != WEBKIT_LOAD_COMMITTED) return;
    record_history_once(webkit_web_view_get_uri(web_view));
}

static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    const gchar *title = webkit_web_view_get_title(web_view);
    if (!title) return;

    zephium_tab_t *tab = zephium_browser_get_active_tab(g_app.core);
    if (tab) {
        zephium_browser_set_tab_title(g_app.core, g_app.core->active_tab_index, title);
    }
    update_active_tab_ui(&g_app);
}

/* Every URL that reaches a log line or the history table is passed through
 * zephium_nav_redact_url(): query strings and fragments carry session tokens,
 * signed CDN parameters and search terms, and this log is written with an fflush
 * from the UI process while pages load. A full googlevideo URL is ~1KB, so
 * dropping the query also keeps the lines short. */
static const char *redacted_url(const char *url, char *buf, size_t buf_len) {
    if (!url) return "(null)";
    if (!zephium_nav_redact_url(url, strlen(url), buf, buf_len)) return "(unprintable)";
    return buf;
}

static void on_resource_failed(WebKitWebResource *resource,
                              GError *error,
                              gpointer user_data) {
    (void)user_data;
    const gchar *uri = webkit_web_resource_get_uri(resource);
    char safe_uri[ZEPHIUM_MAX_URL_BYTES];
    /* Code 302 in WebKitNetworkError is WEBKIT_NETWORK_ERROR_CANCELLED (user navigated or redirected).
     * Do not log normal subresource cancellation as an alarming ERROR. */
    if (error && error->code == 302 && error->domain == WEBKIT_NETWORK_ERROR) {
        zephium_log("DEBUG", "RESOURCE_CANCELLED", "URI: %s (load cancelled / redirected)",
                    redacted_url(uri, safe_uri, sizeof(safe_uri)));
        return;
    }
    zephium_log("ERROR", "RESOURCE_FAILED", "URI: %s | Error: %s (code: %d, domain: %s)",
                redacted_url(uri, safe_uri, sizeof(safe_uri)),
                error ? error->message : "unknown",
                error ? error->code : 0,
                error ? g_quark_to_string(error->domain) : "unknown");
}

static void on_resource_finished(WebKitWebResource *resource,
                                gpointer user_data) {
    (void)user_data;
    const gchar *uri = webkit_web_resource_get_uri(resource);
    if (uri && (strstr(uri, "videoplayback") || strstr(uri, "manifest") || strstr(uri, "m3u8"))) {
        char safe_uri[ZEPHIUM_MAX_URL_BYTES];
        zephium_log("INFO", "MEDIA_CHUNK_LOADED", "Media resource loaded: %s",
                    redacted_url(uri, safe_uri, sizeof(safe_uri)));
    }
}

static void on_resource_load_started(WebKitWebView *web_view,
                                     WebKitWebResource *resource,
                                     WebKitURIRequest *request,
                                     gpointer user_data) {
    (void)web_view;
    (void)user_data;
    const gchar *uri = webkit_uri_request_get_uri(request);
    g_signal_connect(resource, "failed", G_CALLBACK(on_resource_failed), NULL);
    g_signal_connect(resource, "finished", G_CALLBACK(on_resource_finished), NULL);

    if (uri && (strstr(uri, "videoplayback") || strstr(uri, "blob:") || strstr(uri, "pagead") || strstr(uri, "doubleclick"))) {
        char safe_uri[ZEPHIUM_MAX_URL_BYTES];
        zephium_log("INFO", "REQUEST_START", "Request: %s",
                    redacted_url(uri, safe_uri, sizeof(safe_uri)));
    }
}

static guint g_web_process_crash_count = 0;

static void on_web_process_terminated(WebKitWebView *web_view,
                                      WebKitWebProcessTerminationReason reason,
                                      gpointer user_data) {
    (void)user_data;
    const char *desc = "CRASHED";
#if WEBKIT_CHECK_VERSION(2, 34, 0)
    if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT) desc = "MEMORY_EXCEEDED";
#endif
    zephium_log("ERROR", "WEB_PROCESS_TERMINATED", "WebKit WebProcess terminated unexpectedly (%s).", desc);

    if (++g_web_process_crash_count <= 2) {
        zephium_log("WARN", "CRASH_RECOVERY", "Attempting automatic tab reload (attempt %u/2)...", g_web_process_crash_count);
        webkit_web_view_reload(web_view);
    } else {
        zephium_log("ERROR", "CRASH_HALTED", "Crash loop halted. Showing diagnostic screen.");
        webkit_web_view_load_html(web_view,
            "<html><body style='background:#181818;color:#f0f0f0;font-family:sans-serif;padding:40px;line-height:1.6;'>"
            "<h2>Zephium Media Recovery</h2>"
            "<p>The WebProcess stopped while loading media streams.</p>"
            "<p>To ensure smooth playback on WSL, ensure all GStreamer codecs are installed:</p>"
            "<pre style='background:#2a2a2a;padding:12px;border-radius:6px;font-family:monospace;'>"
            "sudo apt-get install -y gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-plugins-ugly\n"
            "</pre>"
            "<p>Run <code style='background:#333;padding:2px 6px;'>python3 read_log.py</code> for details.</p>"
            "</body></html>",
            "about:blank");
    }
}

static gboolean on_load_failed(WebKitWebView *web_view,
                               WebKitLoadEvent load_event,
                               gchar *failing_uri,
                               GError *error,
                               gpointer user_data) {
    (void)web_view;
    (void)load_event;
    (void)user_data;
    char safe_uri[ZEPHIUM_MAX_URL_BYTES];
    zephium_log("ERROR", "PAGE_LOAD_FAILED", "Failed loading '%s': %s (code: %d)",
                redacted_url(failing_uri, safe_uri, sizeof(safe_uri)),
                error ? error->message : "unknown error",
                error ? error->code : 0);
    return FALSE;
}

static gboolean on_decide_policy(WebKitWebView *web_view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 gpointer user_data) {
    (void)web_view;
    (void)user_data;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        WebKitNavigationType nav_type = webkit_navigation_action_get_navigation_type(action);

        /* Only restrict explicit user link clicks.
         * Never block WEBKIT_NAVIGATION_TYPE_OTHER or subresources (which breaks YouTube ad/video players) */
        if (nav_type == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
            WebKitURIRequest *req = webkit_navigation_action_get_request(action);
            const gchar *uri = webkit_uri_request_get_uri(req);

            if (uri && *uri) {
                if (strncmp(uri, "blob:", 5) == 0 ||
                    strncmp(uri, "about:", 6) == 0 ||
                    strncmp(uri, "data:", 5) == 0) {
                    return FALSE;
                }
                if (!zephium_browser_is_nav_allowed(g_app.core, uri)) {
                    char safe_uri[ZEPHIUM_MAX_URL_BYTES];
                    g_printerr("[Zephium Security] Blocked link navigation to disallowed target: %s\n",
                               redacted_url(uri, safe_uri, sizeof(safe_uri)));
                    webkit_policy_decision_ignore(decision);
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

#define ZEPHIUM_PERMISSION_DENIAL_LOG_LIMIT 20
static unsigned g_denied_permissions = 0;

/* Permission requests are denied by default. Encrypted Media Extensions (EME)
 * is the only one granted: it is required to play DRM-protected media, it
 * exposes no device, and the renderer cannot use it without the page already
 * having asked for it. Camera, microphone, geolocation, notifications, pointer
 * lock, XR, clipboard, device information and cross-site data access are all
 * refused, so a page cannot reach the user's hardware, location or other sites
 * - the browser never had a prompt to ask for them. The legacy "install missing
 * media plugins" request is refused too: granting it would let any page start a
 * system package installation; the denial is logged so the cause is visible. */
static gboolean on_permission_request(WebKitWebView *web_view,
                                      WebKitPermissionRequest *request,
                                      gpointer user_data) {
    (void)web_view;
    (void)user_data;

    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request)) {
        webkit_permission_request_allow(request);
        return TRUE;
    }

    /* Log the first few refusals: without a trace, a site that silently loses the
     * camera looks like a Zephium bug. The cap stops a page from flooding the log
     * by requesting a permission in a loop. */
    if (g_denied_permissions < ZEPHIUM_PERMISSION_DENIAL_LOG_LIMIT) {
        ++g_denied_permissions;
        zephium_log("INFO", "PERMISSION_DENIED", "Denied permission request: %s",
                    G_OBJECT_TYPE_NAME(request));
    }
    webkit_permission_request_deny(request);
    return TRUE;
}

/* Escapes text for embedding in the diagnostic pages below. */
static void html_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; p && *p && o + 6 < cap; ++p) {
        switch (*p) {
            case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
            case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
            case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
            case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
            default: out[o++] = *p; break;
        }
    }
    out[o] = '\0';
}

/* A page asked for a new window (target="_blank", window.open). WebKit hands over
 * the navigation action and expects the new view back; returning NULL cancels the
 * popup. The target goes through the same allow-list as a typed address, so a page
 * cannot use a popup to reach something the omnibox would refuse. */
static GtkWidget *on_create_web_view(WebKitWebView *web_view, WebKitNavigationAction *action,
                                     gpointer user_data) {
    (void)web_view;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    char safe_uri[ZEPHIUM_MAX_URL_BYTES];

    WebKitURIRequest *request = action ? webkit_navigation_action_get_request(action) : NULL;
    const gchar *uri = request ? webkit_uri_request_get_uri(request) : NULL;
    if (!uri || !*uri) {
        zephium_log("WARN", "POPUP_BLOCKED", "Popup without a target URL was refused.");
        return NULL;
    }
    if (!zephium_nav_is_allowed(uri, strlen(uri))) {
        zephium_log("WARN", "POPUP_BLOCKED", "Blocked popup to %s",
                    redacted_url(uri, safe_uri, sizeof(safe_uri)));
        return NULL;
    }

    zephium_log("INFO", "POPUP_OPENED", "Opening popup in a new tab: %s",
                redacted_url(uri, safe_uri, sizeof(safe_uri)));

    /* The popup becomes one of our own tabs and loads the target itself, so the
     * handler does not hand a view back to WebKit: returning NULL drops the
     * request (WebKit would otherwise drive a view we did not create as a related
     * view). */
    (void)create_tab_view(app, uri, false);
    return NULL;
}

/* A certificate error (expired, wrong host, self-signed, untrusted chain) aborts
 * the load. There is deliberately no "continue anyway" path: the page is replaced
 * with an explanation and the connection is dropped. */
static gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view, gchar *failing_uri,
                                               GTlsCertificate *certificate,
                                               GTlsCertificateFlags errors, gpointer user_data) {
    (void)certificate;
    (void)user_data;

    char safe_uri[ZEPHIUM_MAX_URL_BYTES];
    zephium_log("ERROR", "TLS_ERROR",
                "Certificate rejected for %s (flags=0x%x); the page was not loaded.",
                redacted_url(failing_uri, safe_uri, sizeof(safe_uri)), (unsigned int)errors);

    char host[256] = "unknown host";
    if (failing_uri && *failing_uri) {
        GUri *parsed = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL);
        if (parsed) {
            const char *name = g_uri_get_host(parsed);
            if (name && *name) (void)zephium_strcpy_safe(host, sizeof(host), name);
            g_uri_unref(parsed);
        }
    }

    char escaped_host[1024];
    html_escape(host, escaped_host, sizeof(escaped_host));

    char page[4096];
    snprintf(page, sizeof(page),
             "<html><head><meta charset='utf-8'></head>"
             "<body style='background:#181818;color:#f0f0f0;font-family:sans-serif;padding:40px;line-height:1.6;'>"
             "<h2>Insecure connection blocked</h2>"
             "<p>Zephium did not load <b>%s</b> because its TLS certificate failed validation.</p>"
             "<p>The connection was closed. There is no &quot;continue anyway&quot; option: a certificate "
             "that cannot be verified may belong to an attacker on the same network.</p>"
             "<p>Things worth checking: the system clock, a Wi-Fi captive portal that intercepts "
             "traffic, or a proxy that replaces certificates.</p>"
             "<p style='color:#888;font-size:13px;'>TLS flags: 0x%x</p>"
             "</body></html>",
             escaped_host, (unsigned int)errors);
    webkit_web_view_load_html(web_view, page, "about:blank");
    return TRUE;
}

static GtkWidget *create_tab_view(zephium_gtk_app_t *app, const char *url, bool reuse_initial_tab) {
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    if (app->adblock_filter) {
        webkit_user_content_manager_add_filter(ucm, app->adblock_filter);
    }
    install_page_diagnostics(ucm);

    GtkWidget *view = webkit_web_view_new_with_user_content_manager(ucm);
    WebKitSettings *settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));

    /* Accelerated compositing must stay available: WebKitGTK paints decoded video
     * frames into the compositor. Setting WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER
     * is what used to segfault the UI process on YouTube.
     *
     * WebGL is deliberately left enabled: llvmpipe provides WebGL 1.0/2.0, and
     * disabling it makes every three.js / 3D site fail with
     * "Error creating WebGL context" (e.g. the grokipedia.com book viewer). */
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);

    /* Media & video streaming settings for YouTube & HTML5 MSE */
    webkit_settings_set_enable_mediasource(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);

    /* Explicit standard font families ensuring glyphs render properly on WSL/Linux */
    webkit_settings_set_default_font_family(settings, "sans-serif");
    webkit_settings_set_sans_serif_font_family(settings, "sans-serif");
    webkit_settings_set_serif_font_family(settings, "serif");
    webkit_settings_set_monospace_font_family(settings, "monospace");
    webkit_settings_set_default_font_size(settings, 16);
    webkit_settings_set_default_monospace_font_size(settings, 13);

    g_signal_connect(view, "notify::uri", G_CALLBACK(on_url_changed), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title_changed), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(view, "resource-load-started", G_CALLBACK(on_resource_load_started), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(view, "create", G_CALLBACK(on_create_web_view), app);
    g_signal_connect(view, "load-failed-with-tls-errors",
                     G_CALLBACK(on_load_failed_with_tls_errors), app);

    WebKitFindController *find_controller = webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(view));
    if (find_controller) {
        g_signal_connect(find_controller, "found-text", G_CALLBACK(on_find_found), NULL);
        g_signal_connect(find_controller, "failed-to-find-text", G_CALLBACK(on_find_not_found), NULL);
    }

    /* zephium_browser_init() already created the first tab. Reusing it keeps the
     * notebook page index and the core tab index in sync (they drifted by one
     * before, so closing the first tab removed the wrong core tab). */
    zephium_tab_t *tab = NULL;
    if (reuse_initial_tab && app->core->tab_count == 1) {
        tab = zephium_browser_get_tab(app->core, 0);
        if (tab) {
            app->core->active_tab_index = 0;
            if (url && *url && zephium_browser_navigate(app->core, url)) {
                /* the initial URL counts as a visit; make sure the load does not
                 * record the same URL a second time */
                (void)zephium_strcpy_safe(g_last_history_uri, sizeof(g_last_history_uri), tab->current_url);
            }
        }
    } else {
        tab = zephium_browser_new_tab(app->core, url);
    }
    if (tab && tab->current_url[0]) {
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), tab->current_url);
    }

    /* The WebView scrolls itself; adding a GtkScrolledWindow only inserted a
     * viewport (double scrolling + broken view lookup). */
    gtk_widget_show(view);

    GtkWidget *tab_label = gtk_label_new(tab ? tab->title : "New Tab");
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), view, tab_label);

    gint new_page = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook)) - 1;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), new_page);
    update_active_tab_ui(app);

    return view;
}

/* ---------------------------------------------------------------------------
 * Find in page (Ctrl+F)
 *
 * The strip is hidden by default and drives the active tab's
 * WebKitFindController. The search restarts on every edit (incremental), Enter
 * goes forward and Shift+Enter backward, Escape closes the strip and clears the
 * highlight.
 * ------------------------------------------------------------------------- */
#define ZEPHIUM_FIND_MAX_MATCHES 1000

static void find_finish(void) {
    WebKitWebView *view = get_active_web_view(&g_app);
    if (!view) return;
    WebKitFindController *controller = webkit_web_view_get_find_controller(view);
    if (controller) webkit_find_controller_search_finish(controller);
}

static void find_search(bool forward, bool restart) {
    WebKitWebView *view = get_active_web_view(&g_app);
    if (!view || !g_app.find_entry) return;
    WebKitFindController *controller = webkit_web_view_get_find_controller(view);
    if (!controller) return;

    const char *text = gtk_entry_get_text(GTK_ENTRY(g_app.find_entry));
    if (!text || !*text) {
        webkit_find_controller_search_finish(controller);
        if (g_app.lbl_find) gtk_label_set_text(GTK_LABEL(g_app.lbl_find), "");
        return;
    }

    if (restart) {
        webkit_find_controller_search(controller, text,
                                      WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND,
                                      ZEPHIUM_FIND_MAX_MATCHES);
    } else if (forward) {
        webkit_find_controller_search_next(controller);
    } else {
        webkit_find_controller_search_previous(controller);
    }
}

static void on_find_found(WebKitFindController *controller, guint match_count, gpointer user_data) {
    (void)controller;
    (void)user_data;
    if (!g_app.lbl_find) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%u match%s", match_count, match_count == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(g_app.lbl_find), buf);
}

static void on_find_not_found(WebKitFindController *controller, gpointer user_data) {
    (void)controller;
    (void)user_data;
    if (g_app.lbl_find) gtk_label_set_text(GTK_LABEL(g_app.lbl_find), "No matches");
}

static void on_find_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    (void)user_data;
    find_search(true, true);
}

static void on_find_next_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    find_search(true, false);
}

static void on_find_prev_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    find_search(false, false);
}

static void on_find_close(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    find_finish();
    if (g_app.find_bar) gtk_widget_hide(g_app.find_bar);
    WebKitWebView *view = get_active_web_view(&g_app);
    if (view) gtk_widget_grab_focus(GTK_WIDGET(view));
}

static gboolean on_find_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (event->keyval == GDK_KEY_Escape) {
        on_find_close(NULL, NULL);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        bool backward = (event->state & GDK_SHIFT_MASK) != 0;
        find_search(!backward, false);
        return TRUE;
    }
    return FALSE;
}

static void on_omnibox_activate(GtkEntry *entry, gpointer user_data) {
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    const gchar *text = gtk_entry_get_text(entry);
    if (!text || !*text) return;

    if (zephium_browser_navigate(app->core, text)) {
        zephium_tab_t *tab = zephium_browser_get_active_tab(app->core);
        WebKitWebView *view = get_active_web_view(app);
        if (tab && view) {
            webkit_web_view_load_uri(view, tab->current_url);
        }
    }
    update_active_tab_ui(app);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    WebKitWebView *view = get_active_web_view(app);
    if (view && webkit_web_view_can_go_back(view)) {
        webkit_web_view_go_back(view);
    }
}

static void on_forward_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    WebKitWebView *view = get_active_web_view(app);
    if (view && webkit_web_view_can_go_forward(view)) {
        webkit_web_view_go_forward(view);
    }
}

static void on_reload_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    WebKitWebView *view = get_active_web_view(app);
    if (view) {
        webkit_web_view_reload(view);
    }
}

static void on_zoom_in_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    uint16_t new_zoom = zephium_browser_step_zoom(app->core, true);

    WebKitWebView *view = get_active_web_view(app);
    if (view) {
        webkit_web_view_set_zoom_level(view, (gdouble)new_zoom / 100.0);
    }
    update_active_tab_ui(app);
}

static void on_zoom_out_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    uint16_t new_zoom = zephium_browser_step_zoom(app->core, false);

    WebKitWebView *view = get_active_web_view(app);
    if (view) {
        webkit_web_view_set_zoom_level(view, (gdouble)new_zoom / 100.0);
    }
    update_active_tab_ui(app);
}

static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    create_tab_view(app, "about:blank", false);
}

static gboolean on_key_press_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    guint state = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK);

    if (state == GDK_CONTROL_MASK) {
        if (event->keyval == GDK_KEY_t || event->keyval == GDK_KEY_T) {
            create_tab_view(app, "about:blank", false);
            return TRUE;
        } else if (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W) {
            gint page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
            if (page >= 0 && gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook)) > 1) {
                gtk_notebook_remove_page(GTK_NOTEBOOK(app->notebook), page);
                zephium_browser_close_tab(app->core, (size_t)page);
                update_active_tab_ui(app);
            }
            return TRUE;
        } else if (event->keyval == GDK_KEY_plus || event->keyval == GDK_KEY_equal) {
            on_zoom_in_clicked(NULL, app);
            return TRUE;
        } else if (event->keyval == GDK_KEY_minus) {
            on_zoom_out_clicked(NULL, app);
            return TRUE;
        } else if (event->keyval == GDK_KEY_0) {
            zephium_tab_t *tab = zephium_browser_get_active_tab(app->core);
            if (tab) tab->zoom_percent = ZEPHIUM_ZOOM_DEFAULT_PERCENT;
            WebKitWebView *view = get_active_web_view(app);
            if (view) webkit_web_view_set_zoom_level(view, 1.0);
            update_active_tab_ui(app);
            return TRUE;
        } else if (event->keyval == GDK_KEY_l || event->keyval == GDK_KEY_L) {
            gtk_widget_grab_focus(app->entry_url);
            return TRUE;
        } else if (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
            if (app->find_bar) {
                gtk_widget_show(app->find_bar);
                gtk_widget_grab_focus(app->find_entry);
                gtk_editable_select_region(GTK_EDITABLE(app->find_entry), 0, -1);
            }
            return TRUE;
        }
    } else if (event->keyval == GDK_KEY_F5) {
        on_reload_clicked(NULL, app);
        return TRUE;
    }
    return FALSE;
}

/* --screenshot: dump what the page really renders. On WSL there is no window
 * manager tooling to grab a screenshot, and "the site shows no UI" otherwise
 * cannot be verified from the command line. */
static void on_screenshot_ready(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    GError *err = NULL;
    cairo_surface_t *surface = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src), res, &err);
    const char *path = g_app.screenshot_path;
    if (!surface) {
        zephium_log("ERROR", "SCREENSHOT_FAILED", "%s", err ? err->message : "unknown error");
        if (err) g_error_free(err);
    } else {
        cairo_status_t status = cairo_surface_write_to_png(surface, path);
        if (status == CAIRO_STATUS_SUCCESS) {
            zephium_log("INFO", "SCREENSHOT", "Saved %s (%dx%d)", path,
                        cairo_image_surface_get_width(surface),
                        cairo_image_surface_get_height(surface));
        } else {
            zephium_log("ERROR", "SCREENSHOT_FAILED", "cairo status %d writing %s",
                        (int)status, path);
        }
        cairo_surface_destroy(surface);
    }
    gtk_main_quit();
}

static gboolean take_screenshot(gpointer data) {
    (void)data;

    /* Capture the whole toplevel: this is exactly what the user sees, including
     * the toolbar, and it does not depend on WebKit's snapshot machinery. */
    GdkWindow *gdk_window = gtk_widget_get_window(g_app.window);
    if (gdk_window) {
        gint width = gtk_widget_get_allocated_width(g_app.window);
        gint height = gtk_widget_get_allocated_height(g_app.window);
        GdkPixbuf *pixbuf = gdk_pixbuf_get_from_window(gdk_window, 0, 0, width, height);
        if (pixbuf) {
            GError *error = NULL;
            if (gdk_pixbuf_save(pixbuf, g_app.screenshot_path, "png", &error, NULL)) {
                zephium_log("INFO", "SCREENSHOT", "Saved %s (%dx%d)",
                            g_app.screenshot_path, width, height);
            } else {
                zephium_log("ERROR", "SCREENSHOT_FAILED", "%s",
                            error ? error->message : "png save failed");
                if (error) g_error_free(error);
            }
            g_object_unref(pixbuf);
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }
    }

    /* Fallback for backends that do not expose window pixels (e.g. Wayland). */
    WebKitWebView *view = get_active_web_view(&g_app);
    if (!view) {
        zephium_log("ERROR", "SCREENSHOT_FAILED", "no active web view");
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    webkit_web_view_get_snapshot(view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, NULL,
                                 on_screenshot_ready, NULL);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * Downloads
 *
 * WebKit reports a download on the WebKitWebContext ("download-started") and then
 * asks where to put it ("decide-destination", which carries the server-suggested
 * name). The shell keeps every file inside the user's Downloads directory, never
 * uses the suggested name verbatim (no path separators, no leading dots), mirrors
 * the transfer into the store, and never auto-opens the result.
 * ------------------------------------------------------------------------- */
typedef struct {
    char id[32];
    int64_t total_bytes;
    gint64 last_update_us;
} download_tracker_t;

static unsigned g_download_seq = 0;

static const char *downloads_directory(void) {
    static char *dir = NULL;
    if (dir) return dir;

    const char *special = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (special && *special) {
        dir = g_strdup(special);
    } else {
        dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    }
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        zephium_log("WARN", "DOWNLOAD_DIR", "No writable Downloads directory; download cancelled.");
        g_free(dir);
        dir = NULL;
    }
    return dir;
}

static void sanitize_download_name(const char *suggested, char *out, size_t cap) {
    const char *base = suggested ? suggested : "";

    /* Only the last path component may survive, whatever the server sent. */
    const char *slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    const char *backslash = strrchr(base, '\\');
    if (backslash) base = backslash + 1;

    size_t o = 0;
    for (const char *p = base; *p && o + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        out[o++] = (c == '/' || c == '\\' || c == ':' || c < 0x20) ? '_' : (char)c;
    }
    out[o] = '\0';

    while (out[0] == '.') memmove(out, out + 1, strlen(out)); /* no leading dots */
    if (out[0] == '\0') (void)zephium_strcpy_safe(out, cap, "download");
}

static char *unique_download_path(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return path;

    for (unsigned n = 1; n < 1000; ++n) {
        char candidate[600];
        snprintf(candidate, sizeof(candidate), "%.400s (%u)", name, n);
        g_free(path);
        path = g_build_filename(dir, candidate, NULL);
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) return path;
    }
    return path;
}

static download_tracker_t *download_tracker(WebKitDownload *download) {
    return (download_tracker_t *)g_object_get_data(G_OBJECT(download), "zephium-download");
}

static void on_download_received(WebKitDownload *download, guint64 data_length, gpointer user_data) {
    (void)user_data;
    download_tracker_t *tracker = download_tracker(download);
    if (!tracker || !g_app.core || !g_app.core->store) return;

    /* "received-data" fires per chunk; the store is a synchronous SQLite commit, so
     * progress is written at most twice a second. */
    gint64 now = g_get_monotonic_time();
    if (tracker->last_update_us != 0 && now - tracker->last_update_us < 500000) return;
    tracker->last_update_us = now;

    (void)zephium_store_update_download_progress(g_app.core->store, tracker->id,
                                                 (int64_t)data_length, tracker->total_bytes);
}

static void on_download_finished(WebKitDownload *download, gpointer user_data) {
    (void)user_data;
    download_tracker_t *tracker = download_tracker(download);
    guint64 received = webkit_download_get_received_data_length(download);

    if (tracker && g_app.core && g_app.core->store) {
        int64_t total = tracker->total_bytes > 0 ? tracker->total_bytes : (int64_t)received;
        (void)zephium_store_update_download_progress(g_app.core->store, tracker->id,
                                                     (int64_t)received, total);
        (void)zephium_store_update_download(g_app.core->store, tracker->id, (int64_t)received,
                                            ZEPHIUM_DOWNLOAD_COMPLETED);
    }
    zephium_log("INFO", "DOWNLOAD_FINISHED", "%llu bytes", (unsigned long long)received);
}

static void on_download_failed(WebKitDownload *download, GError *error, gpointer user_data) {
    (void)user_data;
    download_tracker_t *tracker = download_tracker(download);
    if (tracker && g_app.core && g_app.core->store) {
        guint64 received = webkit_download_get_received_data_length(download);
        (void)zephium_store_update_download_progress(g_app.core->store, tracker->id,
                                                     (int64_t)received, tracker->total_bytes);
        (void)zephium_store_update_download(g_app.core->store, tracker->id, (int64_t)received,
                                            ZEPHIUM_DOWNLOAD_FAILED);
    }
    zephium_log("ERROR", "DOWNLOAD_FAILED", "%s", error ? error->message : "unknown error");
}

/* WebKit asks for the destination after the response arrived; suggested_filename is
 * what the server asked for (Content-Disposition) or the last URL component. It is
 * sanitised first, then placed in the Downloads directory. */
static gboolean on_download_decide_destination(WebKitDownload *download, gchar *suggested_filename,
                                               gpointer user_data) {
    (void)user_data;
    download_tracker_t *tracker = download_tracker(download);

    char name[512];
    sanitize_download_name(suggested_filename, name, sizeof(name));

    const char *dir = downloads_directory();
    if (!dir) {
        webkit_download_cancel(download);
        return TRUE;
    }

    char *path = unique_download_path(dir, name);
    if (!path) {
        webkit_download_cancel(download);
        return TRUE;
    }
    webkit_download_set_destination(download, path);

    if (tracker && g_app.core && g_app.core->store) {
        WebKitURIRequest *request = webkit_download_get_request(download);
        const gchar *uri = request ? webkit_uri_request_get_uri(request) : "";
        if (!zephium_store_record_download(g_app.core->store, tracker->id, uri, path,
                                           tracker->total_bytes)) {
            zephium_log("WARN", "DOWNLOAD_STORE", "Could not record the download in the store.");
        }
    }

    /* The filename only: the log must not contain the user's home directory. */
    zephium_log("INFO", "DOWNLOAD_STARTED", "Saving %s", name);
    g_free(path);
    return TRUE;
}

static void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data) {
    (void)context;
    (void)user_data;

    if (!g_app.core || !g_app.core->store) {
        webkit_download_cancel(download);
        return;
    }

    WebKitURIResponse *response = webkit_download_get_response(download);
    download_tracker_t *tracker = g_new0(download_tracker_t, 1);
    snprintf(tracker->id, sizeof(tracker->id), "dl-%u", ++g_download_seq);
    tracker->total_bytes = response ? (int64_t)webkit_uri_response_get_content_length(response) : 0;
    g_object_set_data_full(G_OBJECT(download), "zephium-download", tracker, g_free);

    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), NULL);
    g_signal_connect(download, "received-data", G_CALLBACK(on_download_received), NULL);
    g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), NULL);
    g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), NULL);
}

static int run_gtk_browser(int argc, char **argv, const char *initial_url, bool disable_gpu,
                           bool is_wsl, const char *screenshot_path, int screenshot_delay,
                           bool no_adblock) {
    gtk_init(&argc, &argv);

    memset(&g_app, 0, sizeof(g_app));
    g_app.disable_gpu = disable_gpu;
    g_app.is_wsl = is_wsl;
    g_app.screenshot_path = screenshot_path;
    g_app.screenshot_delay = screenshot_delay;

    g_app.core = zephium_browser_init("zephium_profile.db");
    if (!g_app.core) {
        fprintf(stderr, "Failed to initialize Zephium browser core\n");
        return 1;
    }

    WebKitWebContext *web_context = webkit_web_context_get_default();
    /* The WebKit sandbox is a security boundary and stays enabled. It used to be
     * turned off whenever WSL was detected, which removed the process isolation
     * from every page for the whole session; the opt-out is now explicit. */
    if (zephium_sandbox_disabled()) {
        webkit_web_context_set_sandbox_enabled(web_context, FALSE);
        zephium_log("WARN", "SANDBOX",
                    "WebKit sandbox DISABLED (ZEPHIUM_NO_SANDBOX=1): web content runs with the "
                    "full privileges of your user account. Unset ZEPHIUM_NO_SANDBOX to restore "
                    "the sandbox.");
    } else {
        webkit_web_context_set_sandbox_enabled(web_context, TRUE);
    }
    webkit_web_context_set_cache_model(web_context, WEBKIT_CACHE_MODEL_WEB_BROWSER);

    /* Start compiling the blocker rules before the first page loads; the filter
     * arrives asynchronously and is attached to the tab created further down. */
    adblock_init(no_adblock);

    g_signal_connect(web_context, "download-started", G_CALLBACK(on_download_started), &g_app);

    g_app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 1280, 800);
    g_signal_connect(g_app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_app.window, "key-press-event", G_CALLBACK(on_key_press_event), &g_app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(g_app.window), vbox);

    /* Navigation Toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 2);

    g_app.btn_back = gtk_button_new_with_label("<");
    g_signal_connect(g_app.btn_back, "clicked", G_CALLBACK(on_back_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), g_app.btn_back, FALSE, FALSE, 1);

    g_app.btn_forward = gtk_button_new_with_label(">");
    g_signal_connect(g_app.btn_forward, "clicked", G_CALLBACK(on_forward_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), g_app.btn_forward, FALSE, FALSE, 1);

    g_app.btn_reload = gtk_button_new_with_label("↻");
    g_signal_connect(g_app.btn_reload, "clicked", G_CALLBACK(on_reload_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), g_app.btn_reload, FALSE, FALSE, 1);

    GtkWidget *btn_new_tab = gtk_button_new_with_label("+ Tab");
    g_signal_connect(btn_new_tab, "clicked", G_CALLBACK(on_new_tab_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new_tab, FALSE, FALSE, 2);

    g_app.entry_url = gtk_entry_new();
    g_signal_connect(g_app.entry_url, "activate", G_CALLBACK(on_omnibox_activate), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), g_app.entry_url, TRUE, TRUE, 2);

    GtkWidget *btn_zoom_out = gtk_button_new_with_label("-");
    g_signal_connect(btn_zoom_out, "clicked", G_CALLBACK(on_zoom_out_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_zoom_out, FALSE, FALSE, 1);

    g_app.lbl_zoom = gtk_label_new("100%");
    gtk_box_pack_start(GTK_BOX(toolbar), g_app.lbl_zoom, FALSE, FALSE, 2);

    GtkWidget *btn_zoom_in = gtk_button_new_with_label("+");
    g_signal_connect(btn_zoom_in, "clicked", G_CALLBACK(on_zoom_in_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_zoom_in, FALSE, FALSE, 1);

    /* Find-in-page strip: built hidden and excluded from gtk_widget_show_all(),
     * so Ctrl+F is the only way it appears. */
    g_app.find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_app.find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.find_entry), "Find in page");
    g_signal_connect(g_app.find_entry, "changed", G_CALLBACK(on_find_changed), &g_app);
    g_signal_connect(g_app.find_entry, "key-press-event", G_CALLBACK(on_find_key_press), &g_app);
    gtk_box_pack_start(GTK_BOX(g_app.find_bar), g_app.find_entry, TRUE, TRUE, 2);

    g_app.lbl_find = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(g_app.find_bar), g_app.lbl_find, FALSE, FALSE, 4);

    GtkWidget *btn_find_prev = gtk_button_new_with_label("<");
    g_signal_connect(btn_find_prev, "clicked", G_CALLBACK(on_find_prev_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(g_app.find_bar), btn_find_prev, FALSE, FALSE, 1);

    GtkWidget *btn_find_next = gtk_button_new_with_label(">");
    g_signal_connect(btn_find_next, "clicked", G_CALLBACK(on_find_next_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(g_app.find_bar), btn_find_next, FALSE, FALSE, 1);

    GtkWidget *btn_find_close = gtk_button_new_with_label("x");
    g_signal_connect(btn_find_close, "clicked", G_CALLBACK(on_find_close), &g_app);
    gtk_box_pack_start(GTK_BOX(g_app.find_bar), btn_find_close, FALSE, FALSE, 1);

    gtk_box_pack_start(GTK_BOX(vbox), g_app.find_bar, FALSE, FALSE, 0);
    gtk_widget_show_all(g_app.find_bar);
    gtk_widget_hide(g_app.find_bar);
    gtk_widget_set_no_show_all(g_app.find_bar, TRUE);

    /* Notebook for multi-tab support */
    g_app.notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_app.notebook, TRUE, TRUE, 0);

    create_tab_view(&g_app, initial_url ? initial_url : "https://duckduckgo.com", true);

    gtk_widget_show_all(g_app.window);
    update_active_tab_ui(&g_app);

    if (g_app.screenshot_path && *g_app.screenshot_path) {
        int delay = g_app.screenshot_delay > 0 ? g_app.screenshot_delay : 8;
        if (delay > 300) delay = 300;
        zephium_log("INFO", "SCREENSHOT", "Capturing %s in %d s", g_app.screenshot_path, delay);
        g_timeout_add_seconds((guint)delay, take_screenshot, NULL);
    }

    gtk_main();

    zephium_browser_free(g_app.core);
    return 0;
}
#endif /* ZEPHIUM_ENABLE_WEBKITGTK */

static void print_usage(const char *prog) {
    printf("Zephium Browser 1.0.5 (C Port - Standalone Engine & WebKitGTK)\n");
    printf("Usage:\n");
    printf("  %s [options] [URL]\n\n", prog);
    printf("Options:\n");
    printf("  --help              Show this help message\n");
    printf("  --version           Display version and compilation flags\n");
    printf("  --headless          Run in headless command loop (interactive REPL)\n");
    printf("  --batch             Execute batch automated regression suite\n");
    printf("  --wsl               Force apply WSL/WSLg compatibility tuning\n");
    printf("  --disable-gpu       Disable hardware GPU compositing (software rendering)\n");
    printf("  --x11               Force GDK X11 display backend\n");
    printf("  --wayland           Force GDK Wayland display backend\n");
    printf("  --screenshot <file> Save a PNG of the rendered page and exit\n");
    printf("  --screenshot-delay  Seconds to wait before the screenshot (default 8)\n");
    printf("  --no-adblock        Disable the content filter (same as ZEPHIUM_ADBLOCK=0)\n");
}

static int run_cli_browser(const char *initial_url) {
    zephium_browser_context_t *ctx = zephium_browser_init("zephium_profile.db");
    if (!ctx) {
        fprintf(stderr, "Error: unable to initialize browser store.\n");
        return 1;
    }

    if (initial_url && *initial_url) {
        zephium_browser_navigate(ctx, initial_url);
    }

    printf("============================================================\n");
    printf("  Zephium Standalone Browser Core (C17 Shell)\n");
    printf("  Commands: open <url> | new <url> | tab <idx> | close <idx>\n");
    printf("            zoom + | zoom - | history | bookmarks | adblock | quit\n");
    printf("============================================================\n");

    char line[1024];
    while (1) {
        zephium_tab_t *act = zephium_browser_get_active_tab(ctx);
        printf("[Tab %zu/%zu | Zoom %u%% | %s] > ",
               ctx->active_tab_index + 1, ctx->tab_count,
               act ? act->zoom_percent : 100,
               act ? act->current_url : "none");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            break;
        } else if (strncmp(line, "open ", 5) == 0) {
            const char *target = line + 5;
            if (zephium_browser_navigate(ctx, target)) {
                act = zephium_browser_get_active_tab(ctx);
                printf("Navigated to: %s\n", act ? act->current_url : "");
            } else {
                printf("Blocked or invalid address: %s\n", target);
            }
        } else if (strncmp(line, "new ", 4) == 0) {
            zephium_tab_t *t = zephium_browser_new_tab(ctx, line + 4);
            if (t) {
                printf("Opened tab %zu: %s\n", ctx->tab_count, t->current_url);
            }
        } else if (strncmp(line, "tab ", 4) == 0) {
            size_t idx = (size_t)atoi(line + 4);
            if (idx > 0 && idx <= ctx->tab_count) {
                zephium_browser_switch_tab(ctx, idx - 1);
            } else {
                printf("Invalid tab index (1..%zu)\n", ctx->tab_count);
            }
        } else if (strncmp(line, "close ", 6) == 0) {
            size_t idx = (size_t)atoi(line + 6);
            if (idx > 0 && idx <= ctx->tab_count) {
                zephium_browser_close_tab(ctx, idx - 1);
            } else {
                printf("Invalid tab index (1..%zu)\n", ctx->tab_count);
            }
        } else if (strcmp(line, "zoom +") == 0) {
            uint16_t z = zephium_browser_step_zoom(ctx, true);
            printf("Zoom: %u%%\n", z);
        } else if (strcmp(line, "zoom -") == 0) {
            uint16_t z = zephium_browser_step_zoom(ctx, false);
            printf("Zoom: %u%%\n", z);
        } else if (strcmp(line, "history") == 0) {
            size_t n = zephium_store_count_history(ctx->store);
            printf("History records: %zu total entries in SQLite store.\n", n);
        } else if (strcmp(line, "bookmarks") == 0) {
            size_t n = zephium_store_count_bookmarks(ctx->store);
            printf("Bookmarks count: %zu entries in SQLite store.\n", n);
        } else if (strcmp(line, "adblock") == 0) {
            char ab[2048];
            size_t nb = zephium_browser_generate_adblock_rules(ab, sizeof(ab));
            printf("WebKit Adblock Filter JSON (%zu bytes):\n%s\n", nb, ab);
        } else {
            /* Treat raw line as omnibox input */
            if (zephium_browser_navigate(ctx, line)) {
                act = zephium_browser_get_active_tab(ctx);
                printf("Navigated to: %s\n", act ? act->current_url : "");
            } else {
                printf("Unrecognized command or invalid address: %s\n", line);
            }
        }
    }

    zephium_browser_free(ctx);
    printf("Exited Zephium.\n");
    return 0;
}

static int run_batch_test(void) {
    printf("Running Zephium Batch Automated Regression Suite...\n");
    zephium_browser_context_t *ctx = zephium_browser_init(":memory:");
    if (!ctx) return 1;

    bool ok = true;
    ok = ok && (ctx->tab_count == 1);
    ok = ok && zephium_browser_navigate(ctx, "duckduckgo.com");
    ok = ok && (zephium_browser_new_tab(ctx, "https://gnu.org") != NULL);
    ok = ok && (ctx->tab_count == 2);
    ok = ok && (zephium_browser_step_zoom(ctx, true) == 110);
    ok = ok && zephium_browser_close_tab(ctx, 0);
    ok = ok && (ctx->tab_count == 1);

    zephium_browser_free(ctx);
    if (ok) {
        printf("Batch regression suite: ALL CHECKS PASSED.\n");
        return 0;
    }
    fprintf(stderr, "Batch regression suite: FAILED.\n");
    return 1;
}

int main(int argc, char **argv) {
    const char *initial_url = NULL;
    bool headless = false;
    bool batch = false;
    bool force_wsl = false;
    bool disable_gpu = false;
    bool force_x11 = false;
    bool force_wayland = false;
    bool no_adblock = false;
    const char *screenshot_path = NULL;
    int screenshot_delay = 8;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("Zephium Browser 1.0.5 (C17 Standalone Core & WebKitGTK Shell)\n");
            printf("C Standard: ISO/IEC 9899:2017 (C17)\n");
            printf("Integrated: SQLite3, PCRE2, libsodium, yyjson, libcurl\n");
            printf("WSL Support: Native auto-tuning (sandbox/compositing/display fallback)\n");
            return 0;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--batch") == 0) {
            batch = true;
        } else if (strcmp(argv[i], "--wsl") == 0) {
            force_wsl = true;
        } else if (strcmp(argv[i], "--disable-gpu") == 0 || strcmp(argv[i], "--software-rendering") == 0) {
            disable_gpu = true;
        } else if (strcmp(argv[i], "--x11") == 0) {
            force_x11 = true;
        } else if (strcmp(argv[i], "--wayland") == 0) {
            force_wayland = true;
        } else if (strcmp(argv[i], "--no-adblock") == 0) {
            no_adblock = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-delay") == 0 && i + 1 < argc) {
            screenshot_delay = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            initial_url = argv[i];
        }
    }

    if (batch) {
        return run_batch_test();
    }

    bool on_wsl = force_wsl || is_running_on_wsl();
    apply_wsl_optimizations(force_wsl, disable_gpu, force_x11, force_wayland);

#if defined(__linux__) && defined(ZEPHIUM_ENABLE_WEBKITGTK)
    if (!headless && (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))) {
        return run_gtk_browser(argc, argv, initial_url, disable_gpu, on_wsl,
                               screenshot_path, screenshot_delay, no_adblock);
    }
#endif

    (void)headless;
    (void)on_wsl;
    (void)screenshot_path;
    (void)screenshot_delay;
    (void)no_adblock;
    return run_cli_browser(initial_url);
}
