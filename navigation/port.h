#ifndef ZEPHIUM_NAVIGATION_PORT_H
#define ZEPHIUM_NAVIGATION_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEPHIUM_MAX_URL_BYTES (8 * 1024)
#define ZEPHIUM_SEARCH_BASE "https://duckduckgo.com/"

typedef enum {
    ZEPHIUM_APP_LINK_ASK = 0,
    ZEPHIUM_APP_LINK_SHOW = 1,
    ZEPHIUM_APP_LINK_SILENT = 2
} zephium_app_link_outcome_t;

/**
 * Turns raw omnibox text into an allowed destination URL.
 * Rejects explicit forbidden/malformed targets or local paths without searching.
 * Returns true if an allowed URL was produced in out_url, false if rejected.
 */
bool zephium_nav_classify(const char *input, size_t input_len, char *out_url, size_t out_len);

/**
 * Returns true if the omnibox input will be treated as a web search rather than a URL.
 */
bool zephium_nav_is_query(const char *input, size_t input_len);

/**
 * Converts an explicit browser search into the configured search provider URL.
 * Retains exfiltration guards against local paths and credentials.
 */
bool zephium_nav_search_query(const char *input, size_t input_len, char *out_url, size_t out_len);

/**
 * Gate for whether a URL is allowed to commit in a browser tab.
 * Allows exact "about:blank" and clean http(s) URLs without credentials and not targeting reserved hosts.
 */
bool zephium_nav_is_allowed(const char *url_str, size_t len);

/**
 * Typed text read strictly as a web address (e.g. for bookmarking).
 */
bool zephium_nav_web_address(const char *input, size_t input_len, char *out_url, size_t out_len);

/**
 * An address handed from another application (e.g. clicked link or command line).
 */
bool zephium_nav_external_target(const char *arg, size_t arg_len, char *out_url, size_t out_len);

/**
 * Checks whether the URL is a subframe document (about:blank, about:srcdoc, data:, blob:).
 */
bool zephium_nav_is_subframe_document(const char *target, size_t target_len);

/**
 * Checks whether a scheme is a valid external application link scheme.
 */
bool zephium_nav_app_link_scheme(const char *scheme, size_t len);

/**
 * Validates an external application link (e.g. zoommtg:, mailto:, slack:).
 */
bool zephium_nav_external_app_link(const char *target, size_t target_len, char *out_url, size_t out_len);

/**
 * Evaluates the outcome for an external application link based on registration and user gesture.
 */
zephium_app_link_outcome_t zephium_nav_app_link_outcome(bool app_known, bool user_gesture);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_NAVIGATION_PORT_H */
