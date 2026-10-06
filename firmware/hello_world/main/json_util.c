#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Returns a pointer just past `"key":` and any spaces, or NULL */
static const char *find_value(const char *json, const char *key)
{
    char pattern[48];

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(json, pattern);

    if (p == NULL) {
        return NULL;
    }

    p += strlen(pattern);

    while (*p == ' ') {
        p++;
    }

    return p;
}

bool json_get_string(const char *json, const char *key,
                     char *out, size_t out_len)
{
    const char *p = find_value(json, key);

    if (p == NULL || out_len == 0 || *p != '"') {
        return false;           /* missing, null or not a string */
    }

    p++;

    size_t n = 0;

    while (*p != '\0' && *p != '"' && n + 1 < out_len) {

        if (*p == '\\' && p[1] != '\0') {
            p++;

            if (*p == 'u') {
                out[n++] = '?';

                for (int i = 0; i < 4 && p[1] != '\0'; i++) {
                    p++;
                }

                p++;
                continue;
            }
        }

        out[n++] = *p++;
    }

    out[n] = '\0';

    return n > 0;
}

bool json_get_int(const char *json, const char *key, long *out)
{
    const char *p = find_value(json, key);

    if (p == NULL || !(*p == '-' || (*p >= '0' && *p <= '9'))) {
        return false;
    }

    char *end = NULL;
    long value = strtol(p, &end, 10);

    if (end == p) {
        return false;
    }

    *out = value;

    return true;
}

void json_escape(const char *in, char *out, size_t out_len)
{
    size_t n = 0;

    if (out_len == 0) {
        return;
    }

    for (; *in != '\0'; in++) {

        if ((unsigned char)*in < 0x20) {
            continue;
        }

        bool needs_escape = (*in == '"' || *in == '\\');

        if (n + (needs_escape ? 2 : 1) >= out_len) {
            break;
        }

        if (needs_escape) {
            out[n++] = '\\';
        }

        out[n++] = *in;
    }

    out[n] = '\0';
}
