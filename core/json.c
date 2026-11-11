#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/* p points at the opening quote. Returns the character after the closing quote, or NULL. */
static const char *skip_string(const char *p)
{
    if (*p != '"') {
        return NULL;
    }
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) {
                return NULL;
            }
        }
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

/* Returns the character after the value that starts at p, or NULL on malformed input. */
static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        return skip_string(p);
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    if (!*p) {
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

static const char *find_member(const char *obj, const char *key, size_t key_len)
{
    const char *p = skip_ws(obj);
    if (*p != '{') {
        return NULL;
    }
    p = skip_ws(p + 1);
    while (*p && *p != '}') {
        if (*p != '"') {
            return NULL;
        }
        const char *name = p + 1;
        const char *end = skip_string(p);
        if (!end) {
            return NULL;
        }
        size_t name_len = (size_t)(end - 1 - name);
        p = skip_ws(end);
        if (*p != ':') {
            return NULL;
        }
        p = skip_ws(p + 1);
        if (name_len == key_len && memcmp(name, key, key_len) == 0) {
            return p;
        }
        p = skip_value(p);
        if (!p) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
        }
    }
    return NULL;
}

const char *np_json_find(const char *json, const char *path)
{
    const char *value = json;
    while (value && *path) {
        const char *dot = strchr(path, '.');
        size_t len = dot ? (size_t)(dot - path) : strlen(path);
        value = find_member(value, path, len);
        path += len;
        if (*path == '.') {
            path++;
        }
    }
    return value ? skip_ws(value) : NULL;
}

bool np_json_is_null(const char *value)
{
    return value && strncmp(value, "null", 4) == 0;
}

bool np_json_string(const char *value, char *out, size_t cap)
{
    if (!value || *value != '"' || cap == 0) {
        return false;
    }
    const char *p = value + 1;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *p;
                    if (!h) {
                        return false;
                    }
                    p++;
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                c = code < 128 ? (char)code : '?';
                break;
            }
            case 0:
                return false;
            default:
                break; /* quote, backslash, slash */
            }
        }
        if (n + 1 >= cap) {
            return false;
        }
        out[n++] = c;
    }
    if (*p != '"') {
        return false;
    }
    out[n] = 0;
    return true;
}

bool np_json_long(const char *value, long *out)
{
    if (!value || !(*value == '-' || (*value >= '0' && *value <= '9'))) {
        return false;
    }
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }
    *out = v;
    return true;
}

int np_json_array_length(const char *value)
{
    if (!value || *value != '[') {
        return -1;
    }
    const char *p = skip_ws(value + 1);
    int n = 0;
    while (*p && *p != ']') {
        p = skip_value(p);
        if (!p) {
            return -1;
        }
        n++;
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
        }
    }
    return *p == ']' ? n : -1;
}

bool np_json_get_string(const char *json, const char *path, char *out, size_t cap)
{
    return np_json_string(np_json_find(json, path), out, cap);
}

bool np_json_get_long(const char *json, const char *path, long *out)
{
    return np_json_long(np_json_find(json, path), out);
}

bool np_json_quote(const char *text, char *out, size_t cap)
{
    size_t n = 0;
    if (cap < 3) {
        return false;
    }
    out[n++] = '"';
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (n + 8 >= cap) {
            return false;
        }
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            out[n++] = '\\';
            out[n++] = 'u';
            out[n++] = '0';
            out[n++] = '0';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        } else {
            out[n++] = (char)c;
        }
    }
    out[n++] = '"';
    out[n] = 0;
    return true;
}
