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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void zephium_log_init(void) {
    if (g_log_file) return;
    /* Truncate on start: read_log.py must analyse one session only. Appending
     * kept stale entries from previous runs, so old failures were reported as
     * if they were happening right now. */
    g_log_file = fopen("/tmp/zephium_debug.log", "w");
    if (!g_log_file) {
        g_log_file = fopen("zephium_debug.log", "w");
    }
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
        strcpy(time_buf, "unknown");
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

        /* 1. Disable WebKit bubblewrap sandbox because WSL2 lacks unprivileged user namespace mounting */
        setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 0);

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

        /* 5. Redirect cache and data directories to Linux native /tmp (tmpfs) to avoid NTFS DrvFs fallocate crashes */
        setenv("XDG_CACHE_HOME", "/tmp/zephium-cache", 0);
        setenv("XDG_DATA_HOME", "/tmp/zephium-data", 0);
        mkdir("/tmp/zephium-cache", 0755);
        mkdir("/tmp/zephium-data", 0755);

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
                    "compositing=%s dmabuf=%s gl=%s display=%s",
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
    zephium_browser_context_t *core;
    WebKitUserContentFilter *adblock_filter;
    bool disable_gpu;
    bool is_wsl;
} zephium_gtk_app_t;

static zephium_gtk_app_t g_app;

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
}

static WebKitWebView *get_active_web_view(zephium_gtk_app_t *app) {
    gint page_idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (page_idx < 0) return NULL;
    GtkWidget *scrolled = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), page_idx);
    if (!scrolled) return NULL;
    return WEBKIT_WEB_VIEW(gtk_bin_get_child(GTK_BIN(scrolled)));
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
        strncpy(tab->current_url, uri, sizeof(tab->current_url) - 1);
        tab->current_url[sizeof(tab->current_url) - 1] = '\0';
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

static void on_resource_failed(WebKitWebResource *resource,
                              GError *error,
                              gpointer user_data) {
    (void)user_data;
    const gchar *uri = webkit_web_resource_get_uri(resource);
    /* Code 302 in WebKitNetworkError is WEBKIT_NETWORK_ERROR_CANCELLED (user navigated or redirected).
     * Do not log normal subresource cancellation as an alarming ERROR. */
    if (error && error->code == 302 && error->domain == WEBKIT_NETWORK_ERROR) {
        zephium_log("DEBUG", "RESOURCE_CANCELLED", "URI: %s (load cancelled / redirected)", uri ? uri : "(null)");
        return;
    }
    zephium_log("ERROR", "RESOURCE_FAILED", "URI: %s | Error: %s (code: %d, domain: %s)",
                uri ? uri : "(null)",
                error ? error->message : "unknown",
                error ? error->code : 0,
                error ? g_quark_to_string(error->domain) : "unknown");
}

/* A full googlevideo URL is ~1KB and these lines are written - with an fflush -
 * from the UI process while the page is loading, so log the compact form only. */
static void zephium_format_url(const char *uri, char *out, size_t out_size) {
    size_t olen = 0;
    out[0] = '\0';
    if (!uri) {
        snprintf(out, out_size, "(null)");
        return;
    }
    const char *scheme_end = strstr(uri, "://");
    if (!scheme_end) {
        snprintf(out, out_size, "%s", uri);
        return;
    }
    const char *host_end = scheme_end + 3;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#') host_end++;
    const char *path_end = host_end;
    while (*path_end && *path_end != '?' && *path_end != '#') path_end++;
    const char *param_end = path_end;
    if (*param_end == '?') {
        param_end++;
        while (*param_end && *param_end != '&' && *param_end != '#') param_end++;
    }

    olen = (size_t)(param_end - uri);
    if (olen > out_size - 4) olen = out_size - 4;
    memcpy(out, uri, olen);
    out[olen] = '\0';
    if (param_end < uri + strlen(uri)) {
        strncat(out, "...", out_size - strlen(out) - 1);
    }
}

static void on_resource_finished(WebKitWebResource *resource,
                                gpointer user_data) {
    (void)user_data;
    const gchar *uri = webkit_web_resource_get_uri(resource);
    if (uri && (strstr(uri, "videoplayback") || strstr(uri, "manifest") || strstr(uri, "m3u8"))) {
        char compact[256];
        zephium_format_url(uri, compact, sizeof(compact));
        zephium_log("INFO", "MEDIA_CHUNK_LOADED", "Media resource loaded: %s", compact);
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
        char compact[256];
        zephium_format_url(uri, compact, sizeof(compact));
        zephium_log("INFO", "REQUEST_START", "Request: %s", compact);
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
    zephium_log("ERROR", "PAGE_LOAD_FAILED", "Failed loading '%s': %s (code: %d)",
                failing_uri ? failing_uri : "(null)",
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
                    g_printerr("[Zephium Security] Blocked link navigation to disallowed target: %s\n", uri);
                    webkit_policy_decision_ignore(decision);
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

static gboolean on_permission_request(WebKitWebView *web_view,
                                      WebKitPermissionRequest *request,
                                      gpointer user_data) {
    (void)web_view;
    (void)user_data;
    /* Automatically allow media playback / EME permission requests */
    webkit_permission_request_allow(request);
    return TRUE;
}

static GtkWidget *create_tab_view(zephium_gtk_app_t *app, const char *url) {
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    if (app->adblock_filter) {
        webkit_user_content_manager_add_filter(ucm, app->adblock_filter);
    }

    GtkWidget *view = webkit_web_view_new_with_user_content_manager(ucm);
    WebKitSettings *settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));

    /* Accelerated compositing must stay available: WebKitGTK paints decoded video
     * frames into the compositor. Setting WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER
     * is what used to segfault the UI process on YouTube, so only the GL-heavy
     * WebGL path is disabled here while Mesa runs on llvmpipe. */
    if (app->disable_gpu || app->is_wsl) {
        webkit_settings_set_enable_webgl(settings, FALSE);
    }
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

    zephium_tab_t *tab = zephium_browser_new_tab(app->core, url);
    if (tab && tab->current_url[0]) {
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), tab->current_url);
    }

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), view);
    gtk_widget_show_all(scrolled);

    GtkWidget *tab_label = gtk_label_new(tab ? tab->title : "New Tab");
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), scrolled, tab_label);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), -1);

    return view;
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
    create_tab_view(app, "about:blank");
}

static gboolean on_key_press_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    zephium_gtk_app_t *app = (zephium_gtk_app_t *)user_data;
    guint state = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK);

    if (state == GDK_CONTROL_MASK) {
        if (event->keyval == GDK_KEY_t || event->keyval == GDK_KEY_T) {
            create_tab_view(app, "about:blank");
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
        }
    } else if (event->keyval == GDK_KEY_F5) {
        on_reload_clicked(NULL, app);
        return TRUE;
    }
    return FALSE;
}

static int run_gtk_browser(int argc, char **argv, const char *initial_url, bool disable_gpu, bool is_wsl) {
    gtk_init(&argc, &argv);

    memset(&g_app, 0, sizeof(g_app));
    g_app.disable_gpu = disable_gpu;
    g_app.is_wsl = is_wsl;

    g_app.core = zephium_browser_init("zephium_profile.db");
    if (!g_app.core) {
        fprintf(stderr, "Failed to initialize Zephium browser core\n");
        return 1;
    }

    WebKitWebContext *web_context = webkit_web_context_get_default();
    if (is_wsl) {
        /* Crucial: disable WebKit bubblewrap sandbox in WSL to prevent bwrap permission crashes */
        webkit_web_context_set_sandbox_enabled(web_context, FALSE);
    }
    webkit_web_context_set_cache_model(web_context, WEBKIT_CACHE_MODEL_WEB_BROWSER);

    g_app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 1280, 800);
    g_signal_connect(g_app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_app.window, "key-press-event", G_CALLBACK(on_key_press_event), &g_app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(g_app.window), vbox);

    /* Navigation Toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 2);

    GtkWidget *btn_back = gtk_button_new_with_label("<");
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 1);

    GtkWidget *btn_forward = gtk_button_new_with_label(">");
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_forward, FALSE, FALSE, 1);

    GtkWidget *btn_reload = gtk_button_new_with_label("↻");
    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload_clicked), &g_app);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_reload, FALSE, FALSE, 1);

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

    /* Notebook for multi-tab support */
    g_app.notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_app.notebook, TRUE, TRUE, 0);

    create_tab_view(&g_app, initial_url ? initial_url : "https://duckduckgo.com");

    gtk_widget_show_all(g_app.window);
    update_active_tab_ui(&g_app);

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
        return run_gtk_browser(argc, argv, initial_url, disable_gpu, on_wsl);
    }
#endif

    (void)headless;
    (void)on_wsl;
    return run_cli_browser(initial_url);
}
