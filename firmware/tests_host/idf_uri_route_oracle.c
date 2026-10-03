/*
 * esp_http_server's URI routing, as a host tool: the oracle the dashboard
 * route tests ask which registration answers a request (repo sweep F214;
 * firmware/tests_host/test_dashboard_route_match.test.js and
 * test_canary_dashboard_routes.test.js drive it).
 *
 * httpd_uri_match_wildcard() below is ESP-IDF's own, copied verbatim from
 * components/esp_http_server/src/httpd_uri.c on the release/v5.5 branch
 * (that file's sha256 f336daea98adb1dfc32c31ce45e05749a48737f50ecb8b7ae792a08b4973d093,
 * fetched 2026-10-03). The function is byte-identical in the v4.4.7 tag (the
 * IDF under arduino-esp32 2.0.17, the PlatformIO s3c3 line) and in v5.3.2.
 * Both trees' servers set `uri_match_fn = httpd_uri_match_wildcard`.
 *
 * find_uri_handler() is httpd_find_uri_handler() from the same file, and
 * register_uri_handler() is the duplicate check and slot fill of
 * httpd_register_uri_handler(), with the server's handler array reduced to a
 * plain table and the logging dropped. Kept as written: handlers are tried
 * in registration order, the URI is matched before the method, a URI match
 * with another method leaves 405 behind and the search goes on, the first
 * match with the method wins, and a registration whose template an earlier
 * one already matches (same method) is refused. httpd_uri() hands the
 * matcher the request target's path (http_parser's UF_PATH: everything
 * before a '?' or '#' for an origin-form target), and so does this tool.
 * The v5.5 method test also takes HTTP_ANY (v4.4.7's does not); no route in
 * either tree registers HTTP_ANY.
 *
 * Protocol, one command per stdin line, one answer line each:
 *   S                    start a new, empty server table      -> "S"
 *   R <METHOD> <template> register                            -> "R <slot>" | "R EXISTS <slot>"
 *   Q <METHOD> <target>   route a request                     -> "Q <slot>" | "Q E404" | "Q E405"
 *   M <template> <target> the matcher alone, on the target's path -> "M 1" | "M 0"
 * <slot> is the 0-based registration index of the answering (or, for
 * EXISTS, the earlier matching) handler. Templates and targets carry no
 * spaces. Any other input exits 2.
 */

/* --- copied from ESP-IDF release/v5.5, components/esp_http_server/src/httpd_uri.c ---
 * SPDX-FileCopyrightText: 2018-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * (The function is built with -Wno-sign-compare, as IDF builds it: its
 * `exact_match_chars < asterisk + quest*2` compares a size_t with an int.)
 */
#define _POSIX_C_SOURCE 200809L  /* strdup */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool httpd_uri_match_wildcard(const char *template, const char *uri, size_t len)
{
    const size_t tpl_len = strlen(template);
    size_t exact_match_chars = tpl_len;

    /* Check for trailing question mark and asterisk */
    const char last = (const char) (tpl_len > 0 ? template[tpl_len - 1] : 0);
    const char prevlast = (const char) (tpl_len > 1 ? template[tpl_len - 2] : 0);
    const bool asterisk = last == '*' || (prevlast == '*' && last == '?');
    const bool quest = last == '?' || (prevlast == '?' && last == '*');

    /* Minimum template string length must be:
     *      0 : if neither of '*' and '?' are present
     *      1 : if only '*' is present
     *      2 : if only '?' is present
     *      3 : if both are present
     *
     * The expression (asterisk + quest*2) serves as a
     * case wise generator of these length values
     */

    /* abort in cases such as "?" with no preceding character (invalid template) */
    if (exact_match_chars < asterisk + quest*2) {
        return false;
    }

    /* account for special characters and the optional character if "?" is used */
    exact_match_chars -= asterisk + quest*2;

    if (len < exact_match_chars) {
        return false;
    }

    if (!quest) {
        if (!asterisk && len != exact_match_chars) {
            /* no special characters and different length - strncmp would return false */
            return false;
        }
        /* asterisk allows arbitrary trailing characters, we ignore these using
         * exact_match_chars as the length limit */
        return (strncmp(template, uri, exact_match_chars) == 0);
    } else {
        /* question mark present */
        if (len > exact_match_chars && template[exact_match_chars] != uri[exact_match_chars]) {
            /* the optional character is present, but different */
            return false;
        }
        if (strncmp(template, uri, exact_match_chars) != 0) {
            /* the mandatory part differs */
            return false;
        }
        /* Now we know the URI is longer than the required part of template,
         * the mandatory part matches, and if the optional character is present, it is correct.
         * Match is OK if we have asterisk, i.e. any trailing characters are OK, or if
         * there are no characters beyond the optional character. */
        return asterisk || len <= exact_match_chars + 1;
    }
}
/* --- end of the verbatim copy --- */

/* The handler table: httpd_data's hd_calls[] and the two httpd_uri_t fields
 * routing reads. The slot count is far above either tree's
 * max_uri_handlers; the budgets have their own checks
 * (firmware/canary/scripts/check_route_security.py,
 * firmware/projects/canary-wap/tests_host/check_route_budget.py). */
enum { MAX_URI_HANDLERS = 1024, LINE_MAX_LEN = 1024 };
enum { HTTP_ANY = -1 };  /* IDF 5.x's wildcard method; never registered here */
typedef enum { ERR_NONE = 0, HTTPD_404_NOT_FOUND = 404, HTTPD_405_METHOD_NOT_ALLOWED = 405 } httpd_err_code_t;

typedef struct {
    char *uri;
    int   method;
} uri_handler_t;

static uri_handler_t *hd_calls[MAX_URI_HANDLERS];

/* httpd_find_uri_handler(): returns the slot, or -1 with *err set. */
static int find_uri_handler(const char *uri, size_t uri_len, int method, httpd_err_code_t *err)
{
    if (err) {
        *err = HTTPD_404_NOT_FOUND;
    }

    for (int i = 0; i < MAX_URI_HANDLERS; i++) {
        if (!hd_calls[i]) {
            break;
        }
        /* uri_match_fn is httpd_uri_match_wildcard on every server here */
        if (httpd_uri_match_wildcard(hd_calls[i]->uri, uri, uri_len)) {
            /* URIs match. Now check if method is supported */
            if (hd_calls[i]->method == method || hd_calls[i]->method == HTTP_ANY) {
                /* Match found! */
                if (err) {
                    *err = ERR_NONE;
                }
                return i;
            }
            /* URI found but method not allowed.
             * If URI is found later then this
             * error must be set to 0 */
            if (err) {
                *err = HTTPD_405_METHOD_NOT_ALLOWED;
            }
        }
    }
    return -1;
}

/* httpd_register_uri_handler(): the slot taken, or -(1 + slot) of the
 * earlier handler whose template already matches this one (the
 * ESP_ERR_HTTPD_HANDLER_EXISTS refusal). */
static int register_uri_handler(const char *uri, int method)
{
    /* Make sure another handler with matching URI and method
     * is not already registered. This will also catch cases
     * when a registered URI wildcard pattern already accounts
     * for the new URI being registered */
    const int existing = find_uri_handler(uri, strlen(uri), method, NULL);
    if (existing >= 0) {
        return -(1 + existing);
    }

    for (int i = 0; i < MAX_URI_HANDLERS; i++) {
        if (hd_calls[i] == NULL) {
            hd_calls[i] = malloc(sizeof(uri_handler_t));
            if (hd_calls[i] == NULL) {
                exit(3);
            }
            hd_calls[i]->uri = strdup(uri);
            if (hd_calls[i]->uri == NULL) {
                exit(3);
            }
            hd_calls[i]->method = method;
            return i;
        }
    }
    exit(4);  /* no slot: MAX_URI_HANDLERS is above any real budget */
}

static void reset_table(void)
{
    for (int i = 0; i < MAX_URI_HANDLERS && hd_calls[i]; i++) {
        free(hd_calls[i]->uri);
        free(hd_calls[i]);
        hd_calls[i] = NULL;
    }
}

/* http_method values as esp_http_server's http_parser numbers them. */
static int method_of(const char *name)
{
    static const struct { const char *name; int value; } methods[] = {
        {"DELETE", 0}, {"GET", 1}, {"HEAD", 2}, {"POST", 3}, {"PUT", 4},
        {"OPTIONS", 6}, {"PATCH", 28},
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (strcmp(methods[i].name, name) == 0) {
            return methods[i].value;
        }
    }
    fprintf(stderr, "idf_uri_route_oracle: unknown method %s\n", name);
    exit(2);
}

int main(void)
{
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') {
            line[--n] = '\0';
        } else if (!feof(stdin)) {
            fprintf(stderr, "idf_uri_route_oracle: line too long\n");
            return 2;
        }
        if (strcmp(line, "S") == 0) {
            reset_table();
            puts("S");
            continue;
        }
        char cmd[2], first[LINE_MAX_LEN], arg[LINE_MAX_LEN];
        if (sscanf(line, "%1s %1023s %1023s", cmd, first, arg) != 3) {
            fprintf(stderr, "idf_uri_route_oracle: bad line: %s\n", line);
            return 2;
        }
        if (strcmp(cmd, "M") == 0) {
            printf("M %d\n", httpd_uri_match_wildcard(first, arg, strcspn(arg, "?#")) ? 1 : 0);
            continue;
        }
        const int m = method_of(first);
        if (strcmp(cmd, "R") == 0) {
            const int slot = register_uri_handler(arg, m);
            if (slot >= 0) {
                printf("R %d\n", slot);
            } else {
                printf("R EXISTS %d\n", -slot - 1);
            }
        } else if (strcmp(cmd, "Q") == 0) {
            httpd_err_code_t err;
            const int slot = find_uri_handler(arg, strcspn(arg, "?#"), m, &err);
            if (slot >= 0) {
                printf("Q %d\n", slot);
            } else {
                printf("Q E%d\n", (int)err);
            }
        } else {
            fprintf(stderr, "idf_uri_route_oracle: bad command: %s\n", line);
            return 2;
        }
    }
    reset_table();
    return 0;
}
