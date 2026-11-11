// Procedural test frames in the el133uf1 format (docs/PROTOCOL.md section 4)
// and a dependency-free PNG encoder. Used by the mock server and frame2png.
'use strict';

const zlib = require('node:zlib');

const WIDTH = 1200;
const HEIGHT = 1600;
const HALF_WIDTH = 600;
const ROW_BYTES = 300;
const HALF_BYTES = ROW_BYTES * HEIGHT;
const FRAME_BYTES = 2 * HALF_BYTES;

const INK = { black: 0, white: 1, yellow: 2, red: 3, blue: 5, green: 6 };
const INK_ORDER = [INK.black, INK.white, INK.yellow, INK.red, INK.blue, INK.green];

// RGB by ink code. "panel" is what the inks look like on a Spectra 6 panel
// (values from the Pimoroni Inky library), "ideal" is the pure colour.
const PALETTES = {
  panel: { 0: [0x1e, 0x1d, 0x1f], 1: [0xa1, 0xa4, 0xa5], 2: [0xd0, 0xbe, 0x47], 3: [0x9c, 0x48, 0x4b], 5: [0x3d, 0x3b, 0x5e], 6: [0x3a, 0x5b, 0x46] },
  ideal: { 0: [0, 0, 0], 1: [255, 255, 255], 2: [255, 255, 0], 3: [255, 0, 0], 5: [0, 0, 255], 6: [0, 255, 0] },
};

// 5x7 font, one string of 35 cells per glyph, rows top to bottom.
const FONT = {
  '0': '01110100011001110101110011000101110', '1': '00100011000010000100001000010001110',
  '2': '01110100010000100010001000100011111', '3': '11111000100010000010000011000101110',
  '4': '00010001100101010010111110001000010', '5': '11111100001111000001000011000101110',
  '6': '00110010001000011110100011000101110', '7': '11111000010001000100010000100001000',
  '8': '01110100011000101110100011000101110', '9': '01110100011000101111000010001001100',
  A: '01110100011000110001111111000110001', B: '11110100011000111110100011000111110',
  C: '01110100011000010000100001000101110', D: '11100100101000110001100011001011100',
  E: '11111100001000011110100001000011111', F: '11111100001000011110100001000010000',
  G: '01110100011000010111100011000101111', H: '10001100011000111111100011000110001',
  I: '01110001000010000100001000010001110', J: '00111000100001000010000101001001100',
  K: '10001100101010011000101001001010001', L: '10000100001000010000100001000011111',
  M: '10001110111010110101100011000110001', N: '10001100011100110101100111000110001',
  O: '01110100011000110001100011000101110', P: '11110100011000111110100001000010000',
  Q: '01110100011000110001101011001001101', R: '11110100011000111110101001001010001',
  S: '01111100001000001110000010000111110', T: '11111001000010000100001000010000100',
  U: '10001100011000110001100011000101110', V: '10001100011000110001100010101000100',
  W: '10001100011000110101101011010101010', X: '10001100010101000100010101000110001',
  Y: '10001100010101000100001000010000100', Z: '11111000010001000100010001000011111',
  ' ': '00000000000000000000000000000000000', '/': '00001000010001000100010001000010000',
  '-': '00000000000000011111000000000000000', '.': '00000000000000000000000000110001100',
  ':': '00000011000110000000011000110000000',
};

class Canvas {
  constructor() {
    this.px = new Uint8Array(WIDTH * HEIGHT).fill(INK.white);
  }
  rect(x, y, w, h, ink) {
    const x0 = Math.max(0, x), x1 = Math.min(WIDTH, x + w);
    const y0 = Math.max(0, y), y1 = Math.min(HEIGHT, y + h);
    for (let yy = y0; yy < y1; yy++) this.px.fill(ink, yy * WIDTH + x0, yy * WIDTH + x1);
  }
  // Two inks in a one pixel checkerboard, the way the platform mixes orange.
  dither(x, y, w, h, a, b) {
    for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) this.px[yy * WIDTH + xx] = (xx + yy) & 1 ? a : b;
  }
  text(str, x, y, scale, ink) {
    let pen = x;
    for (const ch of String(str).toUpperCase()) {
      const g = FONT[ch] || FONT[' '];
      for (let r = 0; r < 7; r++) for (let c = 0; c < 5; c++) if (g[r * 5 + c] === '1') this.rect(pen + c * scale, y + r * scale, scale, scale, ink);
      pen += 6 * scale;
    }
  }
  textWidth(str, scale) {
    return String(str).length * 6 * scale - scale;
  }
  centered(str, y, scale, ink) {
    this.text(str, Math.round((WIDTH - this.textWidth(str, scale)) / 2), y, scale, ink);
  }
  // Pack into the el133uf1 layout: left half rows, then right half rows.
  toFrame() {
    const out = Buffer.alloc(FRAME_BYTES);
    for (let half = 0; half < 2; half++) {
      for (let y = 0; y < HEIGHT; y++) {
        const src = y * WIDTH + half * HALF_WIDTH;
        const dst = half * HALF_BYTES + y * ROW_BYTES;
        for (let i = 0; i < ROW_BYTES; i++) out[dst + i] = (this.px[src + 2 * i] << 4) | this.px[src + 2 * i + 1];
      }
    }
    return out;
  }
}

/** A test pattern page: page number, all six inks, half seam markers. */
function testPage(page, pageCount, label) {
  const c = new Canvas();
  const accent = INK_ORDER[2 + ((page - 1) % 4)]; // yellow, red, blue, green in turn
  // Frame and corner marks, to see cropping or mirroring at a glance.
  c.rect(0, 0, WIDTH, 12, INK.black);
  c.rect(0, HEIGHT - 12, WIDTH, 12, INK.black);
  c.rect(0, 0, 12, HEIGHT, INK.black);
  c.rect(WIDTH - 12, 0, 12, HEIGHT, INK.black);
  c.rect(12, 12, 90, 90, INK.red); // top left is red
  c.rect(WIDTH - 102, 12, 90, 90, INK.blue); // top right is blue
  c.rect(12, HEIGHT - 102, 90, 90, INK.green); // bottom left is green
  c.rect(WIDTH - 102, HEIGHT - 102, 90, 90, INK.yellow); // bottom right is yellow

  c.centered('THE MOCK NEWSPAPER', 60, 8, INK.black);
  c.rect(60, 140, WIDTH - 120, 8, INK.black);
  c.centered(`TEST PATTERN  PAGE ${page} / ${pageCount}`, 172, 5, INK.black);

  // The six inks as labelled bars.
  const names = ['BLACK', 'WHITE', 'YELLOW', 'RED', 'BLUE', 'GREEN'];
  const barW = 170, gap = 10, left = (WIDTH - 6 * barW - 5 * gap) / 2;
  for (let i = 0; i < 6; i++) {
    const x = left + i * (barW + gap);
    c.rect(x - 3, 247, barW + 6, 206, INK.black);
    c.rect(x, 250, barW, 200, INK_ORDER[i]);
    c.text(names[i], x + Math.round((barW - c.textWidth(names[i], 4)) / 2), 464, 4, INK.black);
  }

  // Giant page number on a block of this page's accent ink.
  c.rect(300, 540, 600, 560, accent);
  c.rect(330, 570, 540, 500, INK.white);
  const digits = String(page);
  const scale = digits.length > 1 ? 44 : 60;
  c.centered(digits, 610 + (digits.length > 1 ? 56 : 0), scale, INK.black);

  // The seam between the two controllers: red is the last column of the
  // left half, blue the first column of the right half.
  c.rect(HALF_WIDTH - 1, 1120, 1, 120, INK.red);
  c.rect(HALF_WIDTH, 1120, 1, 120, INK.blue);
  c.text('LEFT HALF', 190, 1160, 5, INK.black);
  c.text('RIGHT HALF', 720, 1160, 5, INK.black);

  // Mixed colours by dithering, as the platform does for orange.
  const mixes = [[INK.yellow, INK.red, 'ORANGE'], [INK.white, INK.black, 'GREY'], [INK.blue, INK.red, 'PURPLE'], [INK.yellow, INK.green, 'LIME'], [INK.white, INK.blue, 'SKY']];
  const mixW = 200, mixLeft = (WIDTH - 5 * mixW - 4 * gap) / 2;
  mixes.forEach(([a, b, name], i) => {
    const x = mixLeft + i * (mixW + gap);
    c.dither(x, 1270, mixW, 130, a, b);
    c.text(name, x + Math.round((mixW - c.textWidth(name, 4)) / 2), 1412, 4, INK.black);
  });

  // Fine line pairs: 1, 2 and 4 pixel pitch.
  for (let x = 100; x < 400; x += 2) c.rect(x, 1470, 1, 50, INK.black);
  for (let x = 450; x < 750; x += 4) c.rect(x, 1470, 2, 50, INK.black);
  for (let x = 800; x < 1100; x += 8) c.rect(x, 1470, 4, 50, INK.black);
  c.centered(label || '', 1540, 4, INK.black);
  return c.toFrame();
}

// ---- PNG ----

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const head = Buffer.alloc(8);
  head.writeUInt32BE(data.length, 0);
  head.write(type, 4, 'ascii');
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])), 0);
  return Buffer.concat([head, data, crc]);
}

/** Convert an el133uf1 frame to a 1200x1600 palette PNG. */
function frameToPng(frame, paletteName = 'ideal') {
  if (frame.length !== FRAME_BYTES) throw new Error(`frame is ${frame.length} bytes, expected ${FRAME_BYTES}`);
  const palette = PALETTES[paletteName] || PALETTES.ideal;
  const plte = Buffer.alloc(16 * 3, 0x80); // unused codes show as mid grey
  for (const [code, rgb] of Object.entries(palette)) plte.set(rgb, Number(code) * 3);
  // 4 bits per pixel, so a PNG row is the left half row followed by the right half row.
  const raw = Buffer.alloc(HEIGHT * (1 + 2 * ROW_BYTES));
  for (let y = 0; y < HEIGHT; y++) {
    const o = y * (1 + 2 * ROW_BYTES);
    raw[o] = 0; // filter: none
    frame.copy(raw, o + 1, y * ROW_BYTES, (y + 1) * ROW_BYTES);
    frame.copy(raw, o + 1 + ROW_BYTES, HALF_BYTES + y * ROW_BYTES, HALF_BYTES + (y + 1) * ROW_BYTES);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(WIDTH, 0);
  ihdr.writeUInt32BE(HEIGHT, 4);
  ihdr[8] = 4; // bit depth
  ihdr[9] = 3; // palette
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('PLTE', plte),
    chunk('IDAT', zlib.deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

module.exports = { WIDTH, HEIGHT, FRAME_BYTES, HALF_BYTES, INK, PALETTES, testPage, frameToPng };
