/*
 * el133uf1 frame to PNG, for no-hardware mode. No zlib: the image data goes
 * into "stored" deflate blocks, which every PNG reader accepts. The result
 * is about 965 kB, the same size as the frame.
 *
 * Four bits per pixel with a palette, so a PNG row is simply the left half
 * row followed by the right half row of the frame (the left pixel is in the
 * high four bits in both formats). Colours are the "ideal" palette of
 * tools/mock-server/frames.js; unused ink codes show as mid grey.
 */
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "platform.h"

uint32_t np_crc32(const uint8_t *data, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Write a chunk at out: length, type, data (already in place at out + 8), crc. */
static size_t finish_chunk(uint8_t *out, const char *type, size_t len)
{
    put32(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    put32(out + 8 + len, np_crc32(out + 4, len + 4, 0));
    return len + 12;
}

uint8_t *np_frame_to_png(const uint8_t *frame, size_t *out_len)
{
    static const uint8_t ideal[8][3] = {
        { 0, 0, 0 }, { 255, 255, 255 }, { 255, 255, 0 }, { 255, 0, 0 },
        { 0x80, 0x80, 0x80 }, { 0, 0, 255 }, { 0, 255, 0 }, { 0x80, 0x80, 0x80 },
    };
    const size_t row = 1 + 2 * NP_ROW_BYTES; /* filter byte + 600 bytes */
    const size_t raw_len = row * NP_PANEL_HEIGHT;
    const size_t block = 65535;
    const size_t blocks = (raw_len + block - 1) / block;
    const size_t zlen = 2 + raw_len + blocks * 5 + 4;
    const size_t total = 8 + (12 + 13) + (12 + 16 * 3) + (12 + zlen) + 12;

    uint8_t *png = malloc(total);
    uint8_t *raw = malloc(raw_len);
    if (!png || !raw) {
        free(png);
        free(raw);
        return NULL;
    }
    for (int y = 0; y < NP_PANEL_HEIGHT; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0;
        memcpy(r + 1, frame + (size_t)y * NP_ROW_BYTES, NP_ROW_BYTES);
        memcpy(r + 1 + NP_ROW_BYTES, frame + NP_FRAME_HALF_BYTES + (size_t)y * NP_ROW_BYTES, NP_ROW_BYTES);
    }

    size_t n = 0;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    memcpy(png, sig, 8);
    n = 8;

    uint8_t *ihdr = png + n + 8;
    put32(ihdr, NP_PANEL_WIDTH);
    put32(ihdr + 4, NP_PANEL_HEIGHT);
    ihdr[8] = 4;  /* bit depth */
    ihdr[9] = 3;  /* palette */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* filter method */
    ihdr[12] = 0; /* no interlace */
    n += finish_chunk(png + n, "IHDR", 13);

    uint8_t *plte = png + n + 8;
    for (int i = 0; i < 16; i++) {
        const uint8_t *c = i < 8 ? ideal[i] : ideal[4];
        memcpy(plte + i * 3, c, 3);
    }
    n += finish_chunk(png + n, "PLTE", 16 * 3);

    uint8_t *z = png + n + 8;
    size_t zn = 0;
    z[zn++] = 0x78; /* zlib header: deflate, 32 kB window */
    z[zn++] = 0x01; /* no preset dictionary, check bits so that 0x7801 % 31 == 0 */
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += block) {
        size_t len = raw_len - off < block ? raw_len - off : block;
        z[zn++] = off + len == raw_len ? 1 : 0; /* BFINAL, BTYPE 00 (stored) */
        z[zn++] = (uint8_t)len;
        z[zn++] = (uint8_t)(len >> 8);
        z[zn++] = (uint8_t)~len;
        z[zn++] = (uint8_t)(~len >> 8);
        memcpy(z + zn, raw + off, len);
        zn += len;
        for (size_t i = 0; i < len; i++) {
            a = (a + raw[off + i]) % 65521u;
            b = (b + a) % 65521u;
        }
    }
    put32(z + zn, (b << 16) | a);
    zn += 4;
    n += finish_chunk(png + n, "IDAT", zn);
    n += finish_chunk(png + n, "IEND", 0);
    free(raw);
    *out_len = n;
    return png;
}
