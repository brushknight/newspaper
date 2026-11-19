/*
 * Settings for the Linux program. The same keys work in the config file
 * ("pin_busy = 17") and on the command line ("--pin-busy 17" or
 * "--pin-busy=17"); the command line wins.
 *
 * Pin defaults are the Pimoroni Inky Impression 13.3" on a Raspberry Pi
 * header, as BCM GPIO numbers. Sources are cited in docs/HARDWARE.md.
 */
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "platform.h"

void np_config_defaults(np_linux_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->api, sizeof(cfg->api), "https://newspaper.brushknight.com");
    snprintf(cfg->data_dir, sizeof(cfg->data_dir), "/var/lib/newspaper");
    cfg->sim_refresh_ms = 3000;
    cfg->sim_rest_ms = EL133UF1_MIN_REFRESH_INTERVAL_MS;
    snprintf(cfg->spi_device, sizeof(cfg->spi_device), "/dev/spidev0.0");
    cfg->spi_hz = 10000000;
    /* Pimoroni inky/inky_el133uf1.py: CS0 26, CS1 16, DC 22, RESET 27, BUSY 17. */
    cfg->pin_cs0 = 26;
    cfg->pin_cs1 = 16;
    cfg->pin_dc = 22;
    cfg->pin_reset = 27;
    cfg->pin_busy = 17;
    /* Pimoroni examples/spectra6/buttons.py: A 5, B 6, C 25 on the 13.3", D 24. */
    cfg->button_next = 5;
    cfg->button_back = 6;
    cfg->button_home = 25;
    cfg->debounce_ms = 30;
    cfg->network_check = true;
    cfg->network_wait_s = 30;
    snprintf(cfg->wifi_help[0], sizeof(cfg->wifi_help[0]), "Set up Wi-Fi with Raspberry Pi Imager when");
    snprintf(cfg->wifi_help[1], sizeof(cfg->wifi_help[1]), "you write the card, or on the Pi itself");
    snprintf(cfg->wifi_help[2], sizeof(cfg->wifi_help[2]), "with: sudo nmtui");
}

static int fail(char *err, size_t cap, const char *fmt, const char *a, const char *b)
{
    if (err && cap) {
        snprintf(err, cap, fmt, a, b);
    }
    return -1;
}

static int set_string(char *dst, size_t cap, const char *key, const char *value, char *err, size_t err_cap)
{
    if (strlen(value) >= cap) {
        return fail(err, err_cap, "%s: value too long (%s)", key, value);
    }
    snprintf(dst, cap, "%s", value);
    return 0;
}

static int set_long(long *out, long lo, long hi, const char *key, const char *value, char *err, size_t err_cap)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (errno || end == value || *end || v < lo || v > hi) {
        return fail(err, err_cap, "%s: not a number in range (%s)", key, value);
    }
    *out = v;
    return 0;
}

static int set_bool(bool *out, const char *key, const char *value, char *err, size_t err_cap)
{
    static const char *yes[] = { "1", "yes", "true", "on" };
    static const char *no[] = { "0", "no", "false", "off" };
    for (size_t i = 0; i < 4; i++) {
        if (strcasecmp(value, yes[i]) == 0) {
            *out = true;
            return 0;
        }
        if (strcasecmp(value, no[i]) == 0) {
            *out = false;
            return 0;
        }
    }
    return fail(err, err_cap, "%s: expected yes or no (%s)", key, value);
}

typedef enum { T_STR, T_PIN, T_U32, T_BOOL } kind_t;

typedef struct {
    const char *key;
    kind_t kind;
    size_t offset;
    size_t size; /* T_STR: buffer size */
    long lo, hi; /* T_U32 range */
} option_t;

#define OPT_STR(k, f) { k, T_STR, offsetof(np_linux_config_t, f), sizeof(((np_linux_config_t *)0)->f), 0, 0 }
#define OPT_PIN(k, f) { k, T_PIN, offsetof(np_linux_config_t, f), 0, -1, 511 }
#define OPT_U32(k, f, lo, hi) { k, T_U32, offsetof(np_linux_config_t, f), 0, lo, hi }
#define OPT_BOOL(k, f) { k, T_BOOL, offsetof(np_linux_config_t, f), 0, 0, 0 }

static const option_t OPTIONS[] = {
    OPT_STR("api", api),
    OPT_STR("data_dir", data_dir),
    OPT_STR("runtime_dir", runtime_dir),
    OPT_STR("hardware_id", hardware_id),
    OPT_STR("ca_file", ca_file),
    OPT_BOOL("verbose", verbose),
    OPT_BOOL("no_hardware", no_hardware),
    OPT_STR("frames_dir", frames_dir),
    OPT_U32("sim_refresh_ms", sim_refresh_ms, 0, 600000),
    OPT_U32("sim_rest_ms", sim_rest_ms, 0, 3600000),
    OPT_STR("spi_device", spi_device),
    OPT_U32("spi_hz", spi_hz, 100000, 50000000),
    OPT_STR("gpiochip", gpiochip),
    OPT_PIN("pin_cs0", pin_cs0),
    OPT_PIN("pin_cs1", pin_cs1),
    OPT_PIN("pin_dc", pin_dc),
    OPT_PIN("pin_reset", pin_reset),
    OPT_PIN("pin_busy", pin_busy),
    OPT_PIN("button_next", button_next),
    OPT_PIN("button_back", button_back),
    OPT_PIN("button_home", button_home),
    OPT_U32("debounce_ms", debounce_ms, 0, 1000),
    OPT_BOOL("network_check", network_check),
    OPT_U32("network_wait_s", network_wait_s, 0, 3600),
    OPT_STR("wifi_help1", wifi_help[0]),
    OPT_STR("wifi_help2", wifi_help[1]),
    OPT_STR("wifi_help3", wifi_help[2]),
};

int np_config_set(np_linux_config_t *cfg, const char *key, const char *value, char *err, size_t err_cap)
{
    char norm[40];
    size_t n = 0;
    for (; key[n] && n + 1 < sizeof(norm); n++) {
        norm[n] = key[n] == '-' ? '_' : (char)tolower((unsigned char)key[n]);
    }
    norm[n] = 0;
    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); i++) {
        const option_t *o = &OPTIONS[i];
        if (strcmp(o->key, norm) != 0) {
            continue;
        }
        char *field = (char *)cfg + o->offset;
        long v = 0;
        switch (o->kind) {
        case T_STR:
            return set_string(field, o->size, o->key, value, err, err_cap);
        case T_PIN:
            if (set_long(&v, o->lo, o->hi, o->key, value, err, err_cap) != 0) {
                return -1;
            }
            *(int *)field = (int)v;
            return 0;
        case T_U32:
            if (set_long(&v, o->lo, o->hi, o->key, value, err, err_cap) != 0) {
                return -1;
            }
            *(uint32_t *)field = (uint32_t)v;
            return 0;
        case T_BOOL:
            return set_bool((bool *)field, o->key, value, err, err_cap);
        }
    }
    return fail(err, err_cap, "unknown setting \"%s\"%s", key, "");
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = 0;
    }
    return s;
}

int np_config_parse(np_linux_config_t *cfg, const char *text, char *err, size_t err_cap)
{
    int line_no = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        line_no++;
        if (len >= sizeof(line)) {
            snprintf(err, err_cap, "line %d: too long", line_no);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (eol ? 1 : 0);

        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, err_cap, "line %d: expected key = value", line_no);
            return -1;
        }
        *eq = 0;
        char *key = trim(s);
        char *value = trim(eq + 1);
        size_t vl = strlen(value);
        if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') {
            value[vl - 1] = 0;
            value++;
        }
        char why[160];
        if (np_config_set(cfg, key, value, why, sizeof(why)) != 0) {
            snprintf(err, err_cap, "line %d: %s", line_no, why);
            return -1;
        }
    }
    return 0;
}

int np_config_load_file(np_linux_config_t *cfg, const char *path, char *err, size_t err_cap)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            return 1;
        }
        snprintf(err, err_cap, "%s: %s", path, strerror(errno));
        return -1;
    }
    static char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    bool too_big = !feof(f);
    fclose(f);
    if (too_big) {
        snprintf(err, err_cap, "%s: file too large", path);
        return -1;
    }
    text[n] = 0;
    char why[200];
    if (np_config_parse(cfg, text, why, sizeof(why)) != 0) {
        snprintf(err, err_cap, "%s: %s", path, why);
        return -1;
    }
    return 0;
}
