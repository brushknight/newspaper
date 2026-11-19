/*
 * Key/value storage for the core, in a small text file: one "key=value"
 * line per key, with backslash, newline and carriage return escaped.
 *
 * Every write rewrites the file through a temporary file and a rename, so
 * a power cut leaves either the old or the new version. The file is created
 * with mode 0600: it holds the device token.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

static int find(const np_store_t *s, const char *key)
{
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->e[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

long np_store_serialize(const np_store_t *s, char *buf, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < s->count; i++) {
        const char *k = s->e[i].key;
        size_t kl = strlen(k);
        if (n + kl + 2 > cap) {
            return -1;
        }
        memcpy(buf + n, k, kl);
        n += kl;
        buf[n++] = '=';
        for (const char *v = s->e[i].value; *v; v++) {
            const char *esc = *v == '\\' ? "\\\\" : *v == '\n' ? "\\n" : *v == '\r' ? "\\r" : NULL;
            size_t el = esc ? 2 : 1;
            if (n + el + 1 > cap) {
                return -1;
            }
            if (esc) {
                memcpy(buf + n, esc, 2);
            } else {
                buf[n] = *v;
            }
            n += el;
        }
        if (n + 1 > cap) {
            return -1;
        }
        buf[n++] = '\n';
    }
    if (n + 1 > cap) {
        return -1;
    }
    buf[n] = 0;
    return (long)n;
}

void np_store_parse(np_store_t *s, const char *text)
{
    s->count = 0;
    const char *p = text;
    while (*p && s->count < NP_STORE_MAX_KEYS) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && eq > p && (size_t)(eq - p) < sizeof(s->e[0].key)) {
            int i = s->count;
            memcpy(s->e[i].key, p, (size_t)(eq - p));
            s->e[i].key[eq - p] = 0;
            size_t n = 0;
            bool ok = true;
            for (const char *v = eq + 1; v < end; v++) {
                char c = *v;
                if (c == '\\' && v + 1 < end) {
                    v++;
                    c = *v == 'n' ? '\n' : *v == 'r' ? '\r' : *v;
                }
                if (n + 1 >= sizeof(s->e[0].value)) {
                    ok = false;
                    break;
                }
                s->e[i].value[n++] = c;
            }
            s->e[i].value[n] = 0;
            if (ok && find(s, s->e[i].key) < 0) {
                s->count++;
            }
        }
        p = eol ? eol + 1 : end;
    }
}

int np_store_open(np_store_t *s, const char *path, bool durable)
{
    memset(s, 0, sizeof(*s));
    s->durable = durable;
    if (!path || !*path) {
        return 0;
    }
    snprintf(s->path, sizeof(s->path), "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? 0 : -1;
    }
    static char text[NP_STORE_MAX_KEYS * (NP_STORE_VALUE_MAX * 2 + 20)];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    np_store_parse(s, text);
    return 0;
}

static bool save(np_store_t *s)
{
    if (!s->path[0]) {
        return true;
    }
    static char text[NP_STORE_MAX_KEYS * (NP_STORE_VALUE_MAX * 2 + 20)];
    long n = np_store_serialize(s, text, sizeof(text));
    if (n < 0) {
        return false;
    }
    char tmp[sizeof(s->path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        np_log(NP_LOG_ERROR, "storage: cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    fchmod(fd, 0600); /* in case an older file had looser bits */
    bool ok = write(fd, text, (size_t)n) == n;
    if (ok && s->durable) {
        ok = fsync(fd) == 0;
    }
    ok = close(fd) == 0 && ok;
    if (ok) {
        ok = rename(tmp, s->path) == 0;
    }
    if (!ok) {
        np_log(NP_LOG_ERROR, "storage: cannot save %s: %s", s->path, strerror(errno));
        unlink(tmp);
        return false;
    }
    if (s->durable) {
        /* Make the rename itself durable. */
        char dir[sizeof(s->path)];
        snprintf(dir, sizeof(dir), "%s", s->path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *(slash == dir ? slash + 1 : slash) = 0;
            int dfd = open(dir, O_RDONLY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
    }
    return true;
}

bool np_store_get(const np_store_t *s, const char *key, char *buf, size_t cap)
{
    int i = find(s, key);
    if (i < 0 || strlen(s->e[i].value) + 1 > cap) {
        return false;
    }
    memcpy(buf, s->e[i].value, strlen(s->e[i].value) + 1);
    return true;
}

bool np_store_set(np_store_t *s, const char *key, const char *value)
{
    if (strlen(key) >= sizeof(s->e[0].key) || strchr(key, '=') || strchr(key, '\n')) {
        return false;
    }
    int i = find(s, key);
    if (!value) {
        if (i < 0) {
            return true;
        }
        s->e[i] = s->e[s->count - 1];
        s->count--;
        return save(s);
    }
    if (strlen(value) >= sizeof(s->e[0].value)) {
        return false;
    }
    if (i < 0) {
        if (s->count >= NP_STORE_MAX_KEYS) {
            return false;
        }
        i = s->count++;
        snprintf(s->e[i].key, sizeof(s->e[i].key), "%s", key);
    } else if (strcmp(s->e[i].value, value) == 0) {
        return true;
    }
    snprintf(s->e[i].value, sizeof(s->e[i].value), "%s", value);
    return save(s);
}

bool np_store_clear(np_store_t *s)
{
    s->count = 0;
    return save(s);
}
