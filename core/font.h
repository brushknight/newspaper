/* Compiled-in 1 bit per pixel bitmap fonts for the setup screens. */
#ifndef NP_FONT_H
#define NP_FONT_H

#include <stdint.h>

typedef struct {
    uint16_t code;    /* ASCII code */
    uint16_t w, h;    /* bitmap size in pixels */
    int16_t xoff;     /* bitmap left edge relative to the pen position */
    int16_t yoff;     /* bitmap top edge relative to the top of the line */
    uint16_t advance; /* pen advance in pixels */
    uint32_t offset;  /* first byte in the bitmap array; rows are padded to bytes, MSB first */
} np_glyph_t;

typedef struct {
    const np_glyph_t *glyphs; /* sorted by code */
    uint16_t count;
    const uint8_t *bitmap;
    uint16_t line_height;
    uint16_t ascent;
} np_font_t;

extern const np_font_t np_font_nameplate; /* blackletter, only the letters of the product name */
extern const np_font_t np_font_text;      /* 40 px serif, ASCII */
extern const np_font_t np_font_heading;   /* 68 px bold serif, ASCII */
extern const np_font_t np_font_code;      /* 170 px bold mono, A-Z 0-9 and dash */

const np_glyph_t *np_font_glyph(const np_font_t *font, char c);
int np_font_text_width(const np_font_t *font, const char *text);

#endif
