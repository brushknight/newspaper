/*
 * Minimal read-only JSON access for the few small documents in the device
 * protocol. Works on a NUL terminated buffer, allocates nothing.
 */
#ifndef NP_JSON_H
#define NP_JSON_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Find a value by a dot separated path of object keys, for example
 * "issue.version". Returns a pointer to the first character of the value
 * inside `json`, or NULL when any key is missing.
 */
const char *np_json_find(const char *json, const char *path);

bool np_json_is_null(const char *value);
/* Copy a string value, decoding escapes. False if not a string or too long. */
bool np_json_string(const char *value, char *out, size_t cap);
bool np_json_long(const char *value, long *out);
/* Number of elements of an array value, or -1 if it is not an array. */
int np_json_array_length(const char *value);

/* Shortcuts: find + convert. */
bool np_json_get_string(const char *json, const char *path, char *out, size_t cap);
bool np_json_get_long(const char *json, const char *path, long *out);

/* Write `text` as a JSON string literal with quotes. False if it does not fit. */
bool np_json_quote(const char *text, char *out, size_t cap);

#endif
