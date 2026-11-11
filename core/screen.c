#include "screen.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "frame.h"
#include "qrcodegen.h"

/*
 * The accent is the product's orange. The panel has no orange ink, so it is
 * a one pixel checkerboard of yellow and red. It only works for fills, not
 * for thin strokes or small text.
 */
#define INK_ACCENT 0x10

/* QR codes up to version 12 hold a 150 character URL at medium error correction. */
#define QR_MAX_VERSION 12
#define QR_BUFFER_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)

#define MAX_ITEMS 24
#define MAX_TEXT 72
#define ROWS_PER_CHUNK 8

typedef enum { ITEM_RECT, ITEM_TEXT, ITEM_QR } item_kind_t;

typedef struct {
    item_kind_t kind;
    int x, y, w, h;
    uint8_t ink;
    const np_font_t *font;
    char text[MAX_TEXT];
    int scale; /* QR: pixels per module */
} item_t;

/* One scene at a time; the core is single threaded. Static to keep it off small stacks. */
static struct {
    item_t items[MAX_ITEMS];
    int count;
    uint8_t qr[QR_BUFFER_LEN];
    uint8_t qr_temp[QR_BUFFER_LEN];
    int qr_size;
    uint8_t row[NP_PANEL_WIDTH];
    uint8_t out[NP_ROW_BYTES * ROWS_PER_CHUNK];
} scene;

/* ---- fonts ---- */

const np_glyph_t *np_font_glyph(const np_font_t *font, char c)
{
    for (int i = 0; i < font->count; i++) {
        if (font->glyphs[i].code == (uint16_t)(unsigned char)c) {
            return &font->glyphs[i];
        }
    }
    return NULL;
}

int np_font_text_width(const np_font_t *font, const char *text)
{
    int w = 0;
    for (; *text; text++) {
        const np_glyph_t *g = np_font_glyph(font, *text);
        if (!g) {
            g = np_font_glyph(font, '?');
        }
        if (g) {
            w += g->advance;
        }
    }
    return w;
}

/* ---- scene building ---- */

static void scene_reset(void)
{
    scene.count = 0;
    scene.qr_size = 0;
}

static item_t *add_item(item_kind_t kind)
{
    if (scene.count >= MAX_ITEMS) {
        return NULL;
    }
    item_t *it = &scene.items[scene.count++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    return it;
}

static void add_rect(int x, int y, int w, int h, uint8_t ink)
{
    item_t *it = add_item(ITEM_RECT);
    if (it) {
        it->x = x;
        it->y = y;
        it->w = w;
        it->h = h;
        it->ink = ink;
    }
}

/* Text centred on the panel at line top y. Too long text is cut at the right. */
static void add_text(const np_font_t *font, int y, const char *text, uint8_t ink)
{
    if (!text || !*text) {
        return;
    }
    item_t *it = add_item(ITEM_TEXT);
    if (!it) {
        return;
    }
    snprintf(it->text, sizeof(it->text), "%s", text);
    it->font = font;
    it->w = np_font_text_width(font, it->text);
    it->h = font->line_height;
    it->x = (NP_PANEL_WIDTH - it->w) / 2;
    if (it->x < 20) {
        it->x = 20;
    }
    it->y = y;
    it->ink = ink;
}

/* A QR code in a white box with an accent border, centred, box_size pixels square. */
static bool add_qr(const char *text, int y, int box_size)
{
    if (!qrcodegen_encodeText(text, scene.qr_temp, scene.qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              QR_MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
        return false;
    }
    scene.qr_size = qrcodegen_getSize(scene.qr);
    const int border = 14;
    int x = (NP_PANEL_WIDTH - box_size) / 2;
    add_rect(x, y, box_size, box_size, INK_ACCENT);
    add_rect(x + border, y + border, box_size - 2 * border, box_size - 2 * border, NP_INK_WHITE);
    int inner = box_size - 2 * border;
    int scale = inner / (scene.qr_size + 4); /* at least two modules of quiet zone on each side */
    if (scale < 1) {
        return false;
    }
    item_t *it = add_item(ITEM_QR);
    if (!it) {
        return false;
    }
    it->scale = scale;
    it->w = it->h = scene.qr_size * scale;
    it->x = x + (box_size - it->w) / 2;
    it->y = y + (box_size - it->h) / 2;
    it->ink = NP_INK_BLACK;
    return true;
}

static void add_header(void)
{
    add_rect(0, 0, NP_PANEL_WIDTH, NP_PANEL_HEIGHT, NP_INK_WHITE);
    add_text(&np_font_nameplate, 36, "The Morning Newspaper", NP_INK_BLACK);
    add_rect(60, 204, NP_PANEL_WIDTH - 120, 6, NP_INK_BLACK);
    add_rect(60, 216, NP_PANEL_WIDTH - 120, 2, NP_INK_BLACK);
}

static void add_footer(const char *footer)
{
    add_rect(60, 1496, NP_PANEL_WIDTH - 120, 2, NP_INK_BLACK);
    add_text(&np_font_text, 1512, footer, NP_INK_BLACK);
}

/* ---- rendering ---- */

static void render_row(int y)
{
    uint8_t *row = scene.row;
    memset(row, NP_INK_WHITE, NP_PANEL_WIDTH);
    for (int i = 0; i < scene.count; i++) {
        const item_t *it = &scene.items[i];
        if (y < it->y || y >= it->y + it->h) {
            continue;
        }
        switch (it->kind) {
        case ITEM_RECT: {
            int x0 = it->x < 0 ? 0 : it->x;
            int x1 = it->x + it->w > NP_PANEL_WIDTH ? NP_PANEL_WIDTH : it->x + it->w;
            if (it->ink == INK_ACCENT) {
                for (int x = x0; x < x1; x++) {
                    row[x] = ((x + y) & 1) ? NP_INK_RED : NP_INK_YELLOW;
                }
            } else if (x1 > x0) {
                memset(row + x0, it->ink, (size_t)(x1 - x0));
            }
            break;
        }
        case ITEM_TEXT: {
            int pen = it->x;
            int ty = y - it->y;
            for (const char *c = it->text; *c; c++) {
                const np_glyph_t *g = np_font_glyph(it->font, *c);
                if (!g) {
                    g = np_font_glyph(it->font, '?');
                }
                if (!g) {
                    continue;
                }
                int gy = ty - g->yoff;
                if (gy >= 0 && gy < g->h) {
                    int stride = (g->w + 7) / 8;
                    const uint8_t *bits = it->font->bitmap + g->offset + (size_t)gy * (size_t)stride;
                    for (int bx = 0; bx < g->w; bx++) {
                        if (bits[bx >> 3] & (0x80 >> (bx & 7))) {
                            int x = pen + g->xoff + bx;
                            if (x >= 0 && x < NP_PANEL_WIDTH) {
                                row[x] = it->ink;
                            }
                        }
                    }
                }
                pen += g->advance;
            }
            break;
        }
        case ITEM_QR: {
            int my = (y - it->y) / it->scale;
            for (int mx = 0; mx < scene.qr_size; mx++) {
                if (qrcodegen_getModule(scene.qr, mx, my)) {
                    int x = it->x + mx * it->scale;
                    if (x >= 0 && x + it->scale <= NP_PANEL_WIDTH) {
                        memset(row + x, it->ink, (size_t)it->scale);
                    }
                }
            }
            break;
        }
        }
    }
}

static int render(np_frame_sink_t sink, void *user)
{
    for (int half = 0; half < 2; half++) {
        const uint8_t *src = scene.row + half * NP_HALF_WIDTH;
        int filled = 0;
        for (int y = 0; y < NP_PANEL_HEIGHT; y++) {
            render_row(y);
            uint8_t *dst = scene.out + filled * NP_ROW_BYTES;
            for (int i = 0; i < NP_ROW_BYTES; i++) {
                dst[i] = (uint8_t)((src[2 * i] << 4) | (src[2 * i + 1] & 0x0F));
            }
            filled++;
            if (filled == ROWS_PER_CHUNK || y == NP_PANEL_HEIGHT - 1) {
                int rc = sink(user, half, scene.out, (size_t)filled * NP_ROW_BYTES);
                if (rc != 0) {
                    return rc;
                }
                filled = 0;
            }
        }
    }
    return 0;
}

/* ---- screens ---- */

/* "https://host/path" is shown as "host/path": shorter, and people type it that way. */
static const char *strip_scheme(const char *url)
{
    const char *p = strstr(url, "://");
    return p ? p + 3 : url;
}

int np_screen_pairing(const np_pair_view_t *view, np_frame_sink_t sink, void *user)
{
    if (!view || !view->qr_text || !view->user_code) {
        return -1;
    }
    scene_reset();
    add_header();
    add_text(&np_font_heading, 244, "Add this display", NP_INK_BLACK);
    add_text(&np_font_text, 344, "Scan the code with your phone camera", NP_INK_BLACK);
    if (!add_qr(view->qr_text, 414, 600)) {
        return -1;
    }
    add_text(&np_font_text, 1034, "or open", NP_INK_BLACK);
    add_text(&np_font_text, 1086, strip_scheme(view->display_url ? view->display_url : ""), NP_INK_BLACK);
    add_text(&np_font_text, 1138, "and enter this code", NP_INK_BLACK);
    add_rect(90, 1206, NP_PANEL_WIDTH - 180, 262, INK_ACCENT);
    add_rect(104, 1220, NP_PANEL_WIDTH - 208, 234, NP_INK_WHITE);
    add_text(&np_font_code, 1226, view->user_code, NP_INK_BLACK);
    add_footer(view->footer);
    return render(sink, user);
}

int np_screen_provisioning(const np_prov_view_t *view, np_frame_sink_t sink, void *user)
{
    if (!view || !view->ssid) {
        return -1;
    }
    /* The Wi-Fi QR format that phone cameras understand. Special characters are escaped. */
    char qr[160];
    size_t n = 0;
    bool open = !view->password || !*view->password;
    n += (size_t)snprintf(qr + n, sizeof(qr) - n, "WIFI:T:%s;S:", open ? "nopass" : "WPA");
    for (const char *p = view->ssid; *p && n + 3 < sizeof(qr); p++) {
        if (strchr("\\;,:\"", *p)) {
            qr[n++] = '\\';
        }
        qr[n++] = *p;
    }
    if (!open) {
        n += (size_t)snprintf(qr + n, sizeof(qr) - n, ";P:");
        for (const char *p = view->password; *p && n + 3 < sizeof(qr); p++) {
            if (strchr("\\;,:\"", *p)) {
                qr[n++] = '\\';
            }
            qr[n++] = *p;
        }
    }
    snprintf(qr + n, sizeof(qr) - n, ";;");

    scene_reset();
    add_header();
    add_text(&np_font_heading, 244, "Connect to Wi-Fi", NP_INK_BLACK);
    add_text(&np_font_text, 344, "Scan to join the setup network", NP_INK_BLACK);
    if (!add_qr(qr, 414, 600)) {
        return -1;
    }
    add_text(&np_font_text, 1040, "or join this Wi-Fi network by hand", NP_INK_BLACK);
    add_rect(90, 1106, NP_PANEL_WIDTH - 180, 130, INK_ACCENT);
    add_rect(104, 1120, NP_PANEL_WIDTH - 208, 102, NP_INK_WHITE);
    add_text(&np_font_heading, 1126, view->ssid, NP_INK_BLACK);
    if (!open) {
        char line[MAX_TEXT];
        snprintf(line, sizeof(line), "Password: %s", view->password);
        add_text(&np_font_text, 1250, line, NP_INK_BLACK);
    }
    add_text(&np_font_text, 1310, "A setup page opens. If it does not, go to", NP_INK_BLACK);
    add_text(&np_font_text, 1362, view->url ? view->url : "", NP_INK_BLACK);
    add_text(&np_font_text, 1414, "and pick your home network.", NP_INK_BLACK);
    add_footer(view->footer);
    return render(sink, user);
}

int np_screen_message(const np_message_view_t *view, np_frame_sink_t sink, void *user)
{
    if (!view || !view->title) {
        return -1;
    }
    scene_reset();
    add_header();
    add_rect(90, 560, NP_PANEL_WIDTH - 180, 14, INK_ACCENT);
    add_text(&np_font_heading, 620, view->title, NP_INK_BLACK);
    int y = 750;
    for (int i = 0; i < NP_MESSAGE_MAX_LINES; i++) {
        if (view->lines[i]) {
            add_text(&np_font_text, y, view->lines[i], NP_INK_BLACK);
            y += 60;
        }
    }
    add_rect(90, y + 40, NP_PANEL_WIDTH - 180, 14, INK_ACCENT);
    add_footer(view->footer);
    return render(sink, user);
}
