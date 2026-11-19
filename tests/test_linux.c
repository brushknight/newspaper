/*
 * Host tests for the portable pieces of the Linux target (targets/linux/src):
 * config parsing, the key/value file, the PNG writer, the /proc parsers and
 * newspaper-wifi.txt. Same style as test_core.c: `linux_tests <name>` runs one.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "frame.h"
#include "platform.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* store.c logs write errors through the program's logger. */
void np_log(np_log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void test_config(void)
{
    np_linux_config_t c;
    char err[200];
    np_config_defaults(&c);
    CHECK(c.pin_cs0 == 26 && c.pin_cs1 == 16 && c.pin_dc == 22 && c.pin_reset == 27 && c.pin_busy == 17);
    CHECK(c.button_next == 5 && c.button_back == 6 && c.button_home == 25);
    CHECK(c.sim_rest_ms == 180000);
    const char *text = "# comment\n"
                       "api = http://192.168.1.20:8787\r\n"
                       "  pin-busy=4\n"
                       "button_home = -1\n"
                       "no_hardware = yes\n"
                       "wifi_help1 = \"  spaced  \"\n"
                       "\n";
    CHECK(np_config_parse(&c, text, err, sizeof(err)) == 0);
    CHECK(strcmp(c.api, "http://192.168.1.20:8787") == 0);
    CHECK(c.pin_busy == 4);
    CHECK(c.button_home == -1);
    CHECK(c.no_hardware);
    CHECK(strcmp(c.wifi_help[0], "  spaced  ") == 0);

    CHECK(np_config_parse(&c, "pin_busy = 17\nbogus = 1\n", err, sizeof(err)) == -1);
    CHECK(strstr(err, "line 2") && strstr(err, "bogus"));
    CHECK(np_config_parse(&c, "spi_hz = fast\n", err, sizeof(err)) == -1);
    CHECK(np_config_parse(&c, "verbose = maybe\n", err, sizeof(err)) == -1);
    CHECK(np_config_parse(&c, "just words\n", err, sizeof(err)) == -1);
    CHECK(np_config_set(&c, "sim-rest-ms", "1000", err, sizeof(err)) == 0 && c.sim_rest_ms == 1000);
    CHECK(np_config_load_file(&c, "/nonexistent/newspaper.conf", err, sizeof(err)) == 1);
}

static void test_store(void)
{
    char dir[] = "/tmp/np-store-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[300];
    snprintf(path, sizeof(path), "%s/state", dir);

    np_store_t s;
    CHECK(np_store_open(&s, path, true) == 0);
    char buf[NP_STORE_VALUE_MAX];
    CHECK(!np_store_get(&s, "token", buf, sizeof(buf)));
    CHECK(np_store_set(&s, "token", "abc=def\\x\nline2\r"));
    CHECK(np_store_set(&s, "ver", "v1"));
    CHECK(np_store_set(&s, "ver", "v2"));
    CHECK(np_store_set(&s, "gone", "x"));
    CHECK(np_store_set(&s, "gone", NULL));
    CHECK(!np_store_set(&s, "a_key_that_is_too_long", "x"));

    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    np_store_t again;
    CHECK(np_store_open(&again, path, true) == 0);
    CHECK(again.count == 2);
    CHECK(np_store_get(&again, "token", buf, sizeof(buf)) && strcmp(buf, "abc=def\\x\nline2\r") == 0);
    CHECK(np_store_get(&again, "ver", buf, sizeof(buf)) && strcmp(buf, "v2") == 0);
    CHECK(!np_store_get(&again, "gone", buf, sizeof(buf)));
    char tiny[2];
    CHECK(!np_store_get(&again, "ver", tiny, sizeof(tiny)));

    CHECK(np_store_clear(&again));
    np_store_t empty;
    CHECK(np_store_open(&empty, path, true) == 0 && empty.count == 0);

    np_store_t mem;
    CHECK(np_store_open(&mem, "", false) == 0);
    CHECK(np_store_set(&mem, "rt_check", "12345"));
    CHECK(np_store_get(&mem, "rt_check", buf, sizeof(buf)) && strcmp(buf, "12345") == 0);

    unlink(path);
    rmdir(dir);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void test_png(void)
{
    uint8_t *frame = malloc(NP_FRAME_BYTES);
    for (size_t i = 0; i < NP_FRAME_BYTES; i++) {
        frame[i] = (uint8_t)((i % 7 == 4 ? 1 : i % 7) << 4 | (i % 3));
    }
    size_t len = 0;
    uint8_t *png = np_frame_to_png(frame, &len);
    CHECK(png != NULL);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    CHECK(memcmp(png, sig, 8) == 0);

    /* Walk the chunks, check every CRC, and inflate the stored blocks back into rows. */
    size_t off = 8;
    int chunks = 0;
    bool ihdr_ok = false, iend = false;
    uint8_t *raw = malloc(NP_PANEL_HEIGHT * 601);
    size_t raw_len = 0;
    while (off + 12 <= len && !iend) {
        uint32_t n = be32(png + off);
        const uint8_t *type = png + off + 4;
        const uint8_t *data = png + off + 8;
        CHECK(np_crc32(type, n + 4, 0) == be32(data + n));
        if (memcmp(type, "IHDR", 4) == 0) {
            ihdr_ok = be32(data) == NP_PANEL_WIDTH && be32(data + 4) == NP_PANEL_HEIGHT && data[8] == 4 && data[9] == 3;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            CHECK(data[0] == 0x78 && ((data[0] << 8) | data[1]) % 31 == 0);
            size_t p = 2;
            bool last = false;
            while (!last && p + 5 <= n) {
                last = data[p] & 1;
                size_t bl = data[p + 1] | (size_t)data[p + 2] << 8;
                size_t nl = data[p + 3] | (size_t)data[p + 4] << 8;
                CHECK((bl ^ 0xffff) == nl);
                memcpy(raw + raw_len, data + p + 5, bl);
                raw_len += bl;
                p += 5 + bl;
            }
            uint32_t a = 1, b = 0;
            for (size_t i = 0; i < raw_len; i++) {
                a = (a + raw[i]) % 65521u;
                b = (b + a) % 65521u;
            }
            CHECK(be32(data + p) == ((b << 16) | a));
        } else if (memcmp(type, "IEND", 4) == 0) {
            iend = true;
        }
        off += 12 + n;
        chunks++;
    }
    CHECK(ihdr_ok);
    CHECK(iend && off == len);
    CHECK(chunks == 4);
    CHECK(raw_len == (size_t)NP_PANEL_HEIGHT * 601);
    /* Row 7: filter 0, then left half row 7, then right half row 7. */
    const uint8_t *row = raw + 7 * 601;
    CHECK(row[0] == 0);
    CHECK(memcmp(row + 1, frame + 7 * NP_ROW_BYTES, NP_ROW_BYTES) == 0);
    CHECK(memcmp(row + 301, frame + NP_FRAME_HALF_BYTES + 7 * NP_ROW_BYTES, NP_ROW_BYTES) == 0);
    free(raw);
    free(png);
    free(frame);
}

static void test_netinfo(void)
{
    const char *route_up = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                           "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                           "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n";
    const char *route_lan = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                            "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n";
    const char *v6 = "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
                     "fe800000000000000000000000000001 00000400 00000001 00000000 00450003 wlan0\n";
    const char *v6_none = "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
                          "00000000000000000000000000000000 ffffffff 00000001 00000000 00200200 lo\n";
    CHECK(np_net_has_default_route(route_up, NULL));
    CHECK(!np_net_has_default_route(route_lan, NULL));
    CHECK(!np_net_has_default_route(route_lan, v6_none));
    CHECK(np_net_has_default_route(route_lan, v6));
    CHECK(!np_net_has_default_route(NULL, NULL));

    const char *wireless = "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
                           " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"
                           " wlan0: 0000   70.  -40.  -256        0      0      0      0      0        0\n";
    CHECK(np_net_wireless_dbm(wireless) == -40);
    CHECK(np_net_wireless_dbm("Inter-|\n face |\n") == 0);
    CHECK(np_net_wireless_dbm(NULL) == 0);
}

static void test_wifi_file(void)
{
    /* IEEE 802.11i-2004 Annex H.4 test vector. */
    uint8_t psk[32];
    np_wpa_psk("password", (const uint8_t *)"IEEE", 4, psk);
    static const uint8_t want[32] = { 0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b,
                                      0x90, 0xb3, 0x8a, 0x5f, 0x90, 0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a,
                                      0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e };
    CHECK(memcmp(psk, want, 32) == 0);

    np_wifi_file_t w;
    char err[160];
    const char *untouched = "# The Morning Newspaper: Wi-Fi\nssid=\npassword=\ncountry=\n";
    CHECK(np_wifi_file_parse(untouched, &w, err, sizeof(err)) == 1);

    const char *filled = "\xef\xbb\xbf# edited in Notepad\r\nssid = IEEE \r\npassword=password\r\ncountry=de\r\n";
    CHECK(np_wifi_file_parse(filled, &w, err, sizeof(err)) == 0);
    CHECK(strcmp(w.ssid, "IEEE") == 0 && strcmp(w.password, "password") == 0 && strcmp(w.country, "DE") == 0);
    char conf[2048];
    CHECK(np_wifi_file_to_wpa(&w, conf, sizeof(conf), err, sizeof(err)) == 0);
    CHECK(strstr(conf, "country=DE\n") != NULL);
    CHECK(strstr(conf, "\tssid=49454545\n") != NULL);
    CHECK(strstr(conf, "psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n") != NULL);
    CHECK(strstr(conf, "password") == NULL);

    const char *quoted = "ssid=\" Cafe \"#1\" \"\npassword=\"  spaces kept  \"\nhidden=yes\n";
    CHECK(np_wifi_file_parse(quoted, &w, err, sizeof(err)) == 0);
    CHECK(strcmp(w.ssid, " Cafe \"#1\" ") == 0);
    CHECK(strcmp(w.password, "  spaces kept  ") == 0 && w.hidden);
    CHECK(np_wifi_file_to_wpa(&w, conf, sizeof(conf), err, sizeof(err)) == 0);
    CHECK(strstr(conf, "scan_ssid=1") != NULL);

    CHECK(np_wifi_file_parse("ssid=Open\n", &w, err, sizeof(err)) == 0);
    CHECK(np_wifi_file_to_wpa(&w, conf, sizeof(conf), err, sizeof(err)) == 0);
    CHECK(strstr(conf, "key_mgmt=NONE") != NULL && strstr(conf, "country=") == NULL);

    CHECK(np_wifi_file_parse("ssid=x\npassword=short\n", &w, err, sizeof(err)) == -1);
    CHECK(np_wifi_file_parse("ssid=x\ncountry=Germany\n", &w, err, sizeof(err)) == -1);
    CHECK(np_wifi_file_parse("ssid=x\nwifi=1\n", &w, err, sizeof(err)) == -1);
    CHECK(np_wifi_file_parse("ssid=0123456789012345678901234567890123\n", &w, err, sizeof(err)) == -1);
}

static const struct {
    const char *name;
    void (*fn)(void);
} TESTS[] = {
    { "config", test_config },
    { "store", test_store },
    { "png", test_png },
    { "netinfo", test_netinfo },
    { "wifi_file", test_wifi_file },
};

int main(int argc, char **argv)
{
    int ran = 0;
    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        if (argc > 1 && strcmp(argv[1], TESTS[i].name) != 0) {
            continue;
        }
        int before = failures;
        printf("%s\n", TESTS[i].name);
        TESTS[i].fn();
        printf("  %s\n", failures == before ? "ok" : "FAILED");
        ran++;
    }
    if (ran == 0) {
        printf("no such test\n");
        return 2;
    }
    return failures ? 1 : 0;
}
