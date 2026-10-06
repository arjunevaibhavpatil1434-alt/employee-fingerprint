#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Minimal helpers for the small, flat JSON documents exchanged
 * with the server and the phone. They look up the first
 * occurrence of "key" anywhere in the document, so keys must be
 * unique across nesting levels for the value that is wanted.
 */

/* Copies a string value; \uXXXX escapes become '?' */
bool json_get_string(const char *json, const char *key,
                     char *out, size_t out_len);

bool json_get_int(const char *json, const char *key, long *out);

/* Copies `in` into `out`, escaping '"' and '\' and dropping control chars */
void json_escape(const char *in, char *out, size_t out_len);

#endif /* JSON_UTIL_H */
