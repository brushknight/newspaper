#!/usr/bin/env node
// Turn a raw el133uf1 frame (960,000 bytes) into a PNG for a quick look.
//   node tools/frame2png.js frame.el133uf1 out.png [panel|ideal]
'use strict';

const fs = require('node:fs');
const { frameToPng } = require('./mock-server/frames.js');

const [input, output, palette] = process.argv.slice(2);
if (!input || !output) {
  console.error('usage: frame2png.js <frame file> <out.png> [panel|ideal]');
  process.exit(2);
}
fs.writeFileSync(output, frameToPng(fs.readFileSync(input), palette || 'ideal'));
