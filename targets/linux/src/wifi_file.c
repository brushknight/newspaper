/*
 * newspaper-wifi.txt: the Wi-Fi settings of the flashable image, a plain
 * text file on the FAT boot partition that the owner edits after flashing.
 *
 *   ssid=My home network
 *   password=correct horse battery staple
 *   country=DE
 *
 * Rules: one "key=value" per line; # starts a comment line; keys are ssid,
 * password, country and hidden (yes/no); spaces around key and value are
 * dropped, so put a value in double quotes if it starts or ends with a
 * space. Windows line ends and a UTF-8 byte order mark are fine.
 *
 * The output is a wpa_supplicant.conf. The SSID is written in hex and the
 * passphrase is turned into the 256-bit PSK here, so no character in either
 * needs escaping and the passphrase itself is not written out.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "platform.h"

/* ---- SHA-1, HMAC-SHA1, PBKDF2 (only what the PSK needs) ---- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} sha1_t;

static uint32_t rol(uint32_t x, int s)
{
    return (x << s) | (x >> (32 - s));
}

static void sha1_block(sha1_t *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ cc ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_t *c)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1_t *c, const uint8_t *p, size_t len)
{
    c->len += len;
    while (len > 0) {
        size_t take = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) {
            sha1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha1_final(sha1_t *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) {
        sha1_update(c, &pad, 1);
    }
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) {
        lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(c, lenbuf, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

static void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[20])
{
    uint8_t k[64] = { 0 };
    if (key_len > 64) {
        sha1_t c;
        sha1_init(&c);
        sha1_update(&c, key, key_len);
        sha1_final(&c, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64], inner[20];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha1_t c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, msg_len);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

void np_wpa_psk(const char *passphrase, const uint8_t *ssid, size_t ssid_len, uint8_t out[32])
{
    size_t plen = strlen(passphrase);
    for (uint32_t block = 1; block <= 2; block++) {
        uint8_t salt[36];
        memcpy(salt, ssid, ssid_len);
        salt[ssid_len] = (uint8_t)(block >> 24);
        salt[ssid_len + 1] = (uint8_t)(block >> 16);
        salt[ssid_len + 2] = (uint8_t)(block >> 8);
        salt[ssid_len + 3] = (uint8_t)block;
        uint8_t u[20], t[20];
        hmac_sha1((const uint8_t *)passphrase, plen, salt, ssid_len + 4, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            hmac_sha1((const uint8_t *)passphrase, plen, u, 20, u);
            for (int j = 0; j < 20; j++) {
                t[j] ^= u[j];
            }
        }
        memcpy(out + (block - 1) * 20, t, block == 1 ? 20 : 12);
    }
}

/* ---- the text file ---- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = 0;
    }
    return s;
}

int np_wifi_file_parse(const char *text, np_wifi_file_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if ((unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf) {
        text += 3;
    }
    int line_no = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        line_no++;
        if (len >= sizeof(line)) {
            snprintf(err, err_cap, "line %d is too long", line_no);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (eol ? 1 : 0);

        char *s = trim(line);
        if (!*s || *s == '#') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, err_cap, "line %d: expected key=value", line_no);
            return -1;
        }
        *eq = 0;
        char *key = trim(s);
        char *value = trim(eq + 1);
        size_t vl = strlen(value);
        if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') {
            value[vl - 1] = 0;
            value++;
            vl -= 2;
        }
        if (strcasecmp(key, "ssid") == 0) {
            if (vl > 32) {
                snprintf(err, err_cap, "line %d: an SSID has at most 32 bytes", line_no);
                return -1;
            }
            memcpy(out->ssid, value, vl + 1);
        } else if (strcasecmp(key, "password") == 0 || strcasecmp(key, "psk") == 0) {
            if (vl > 64) {
                snprintf(err, err_cap, "line %d: a Wi-Fi password has at most 63 characters", line_no);
                return -1;
            }
            memcpy(out->password, value, vl + 1);
        } else if (strcasecmp(key, "country") == 0) {
            if (vl != 0 && (vl != 2 || !isalpha((unsigned char)value[0]) || !isalpha((unsigned char)value[1]))) {
                snprintf(err, err_cap, "line %d: country is a two letter code such as DE or GB", line_no);
                return -1;
            }
            out->country[0] = (char)toupper((unsigned char)value[0]);
            out->country[1] = vl ? (char)toupper((unsigned char)value[1]) : 0;
            out->country[2] = 0;
        } else if (strcasecmp(key, "hidden") == 0) {
            out->hidden = strcasecmp(value, "yes") == 0 || strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0;
        } else {
            snprintf(err, err_cap, "line %d: unknown key \"%.40s\"", line_no, key);
            return -1;
        }
    }
    if (!out->ssid[0]) {
        return 1;
    }
    size_t pl = strlen(out->password);
    bool hex64 = pl == 64 && strspn(out->password, "0123456789abcdefABCDEF") == 64;
    if (pl != 0 && (pl < 8 || (pl > 63 && !hex64))) {
        snprintf(err, err_cap, "a WPA password has 8 to 63 characters (or is empty for an open network)");
        return -1;
    }
    return 0;
}

int np_wifi_file_to_wpa(const np_wifi_file_t *w, char *buf, size_t cap, char *err, size_t err_cap)
{
    static const char hex[] = "0123456789abcdef";
    char ssid_hex[65];
    size_t sl = strlen(w->ssid);
    for (size_t i = 0; i < sl; i++) {
        ssid_hex[2 * i] = hex[(unsigned char)w->ssid[i] >> 4];
        ssid_hex[2 * i + 1] = hex[(unsigned char)w->ssid[i] & 15];
    }
    ssid_hex[2 * sl] = 0;

    char security[200];
    size_t pl = strlen(w->password);
    if (pl == 0) {
        snprintf(security, sizeof(security), "\tkey_mgmt=NONE\n");
    } else {
        char psk_hex[65];
        if (pl == 64) {
            for (int i = 0; i < 64; i++) {
                psk_hex[i] = (char)tolower((unsigned char)w->password[i]);
            }
            psk_hex[64] = 0;
        } else {
            uint8_t psk[32];
            np_wpa_psk(w->password, (const uint8_t *)w->ssid, sl, psk);
            for (int i = 0; i < 32; i++) {
                psk_hex[2 * i] = hex[psk[i] >> 4];
                psk_hex[2 * i + 1] = hex[psk[i] & 15];
            }
            psk_hex[64] = 0;
        }
        snprintf(security, sizeof(security), "\tkey_mgmt=WPA-PSK\n\tpsk=%s\n", psk_hex);
    }
    int n = snprintf(buf, cap,
                     "# Written at boot by newspaper-wifi from newspaper-wifi.txt on the boot partition.\n"
                     "%s%s%s"
                     "update_config=0\n"
                     "network={\n"
                     "\tssid=%s\n"
                     "%s"
                     "%s"
                     "}\n",
                     w->country[0] ? "country=" : "", w->country, w->country[0] ? "\n" : "", ssid_hex,
                     w->hidden ? "\tscan_ssid=1\n" : "", security);
    if (n < 0 || (size_t)n >= cap) {
        snprintf(err, err_cap, "output buffer too small");
        return -1;
    }
    return 0;
}
