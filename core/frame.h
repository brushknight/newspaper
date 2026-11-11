/* The el133uf1 frame format, see docs/PROTOCOL.md section 4. */
#ifndef NP_FRAME_H
#define NP_FRAME_H

#define NP_PANEL_WIDTH 1200
#define NP_PANEL_HEIGHT 1600
#define NP_HALF_WIDTH 600
#define NP_ROW_BYTES 300                                      /* one row of one half */
#define NP_FRAME_HALF_BYTES (NP_ROW_BYTES * NP_PANEL_HEIGHT) /* 480,000 */
#define NP_FRAME_BYTES (2 * NP_FRAME_HALF_BYTES)             /* 960,000 */

/* Ink codes, four bits per pixel. 4 and 7 are not used. */
#define NP_INK_BLACK 0
#define NP_INK_WHITE 1
#define NP_INK_YELLOW 2
#define NP_INK_RED 3
#define NP_INK_BLUE 5
#define NP_INK_GREEN 6

#define NP_MODEL_ID "inky-impression-13.3-spectra6"

#endif
