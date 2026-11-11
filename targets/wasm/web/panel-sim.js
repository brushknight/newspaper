// Simulator of the EL133UF1 panel (13.3 inch E Ink Spectra 6, 1200 x 1600).
//
// It is driven through the same "pins" as the real panel: RESET, two chip
// selects, D/C, bytes on the SPI bus, and it answers on BUSY. The firmware's
// driver (drivers/el133uf1) talks to it unchanged.
//
// It tries to behave like the panel, not like a canvas:
//   - nothing is visible until the refresh command (DRF); the pixel data
//     only fills the two controllers' memories
//   - a refresh takes 12 to 20 seconds, flashes the whole screen between ink
//     states and then settles; BUSY is low for all of it
//   - DRF during a refresh is refused, and so is a DRF sooner than 180
//     seconds after the previous refresh ended (the maker's minimum interval)
//   - the white ground is a clean paper white; the five inks are the duller
//     measured ink colours, not pure RGB
//
// Time can be scaled with setSpeed (1, 4, or Infinity for "instant": a
// refresh then takes a quarter of a second and the rest period is dropped).
'use strict';

(function (root) {
  const WIDTH = 1200;
  const HEIGHT = 1600;
  const HALF_WIDTH = 600;
  const ROW_BYTES = 300;
  const HALF_BYTES = ROW_BYTES * HEIGHT;

  const MIN_INTERVAL_MS = 180000;
  const REFRESH_MIN_MS = 12000;
  const REFRESH_MAX_MS = 20000;

  const CMD = { PSR: 0x00, POF: 0x02, PON: 0x04, DTM: 0x10, DRF: 0x12, TRES: 0x61 };

  // RGB per ink code. Codes 4 and 7 do not exist on this panel; a frame that
  // uses them is wrong, so they are shown in a colour that stands out.
  const PALETTES = {
    panel: [[0x1e, 0x1d, 0x1f], [0xf4, 0xf4, 0xf0], [0xd0, 0xbe, 0x47], [0x9c, 0x48, 0x4b], [0xff, 0x00, 0xff], [0x3d, 0x3b, 0x5e], [0x3a, 0x5b, 0x46], [0xff, 0x00, 0xff]],
    ideal: [[0, 0, 0], [255, 255, 255], [255, 255, 0], [255, 0, 0], [255, 0, 255], [0, 0, 255], [0, 255, 0], [255, 0, 255]],
  };

  const mix = (a, b, k) => [a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k];
  const smooth = (x) => (x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x));

  class PanelSim {
    /**
     * @param {HTMLCanvasElement} canvas  1200 x 1600 canvas to draw on
     * @param {{log?: function(string, string): void, onChange?: function(): void}} hooks
     */
    constructor(canvas, hooks = {}) {
      this.canvas = canvas;
      canvas.width = WIDTH;
      canvas.height = HEIGHT;
      this.ctx = canvas.getContext('2d');
      this.image = this.ctx.createImageData(WIDTH, HEIGHT);
      this.out32 = new Uint32Array(this.image.data.buffer);
      this.hooks = hooks;

      this.speed = 1;
      this.paletteName = 'panel';

      // bus state
      this.csMask = 0;
      this.dc = 0;
      this.resetLevel = 1;
      this.command = [0, 0];
      this.memory = [new Uint8Array(HALF_BYTES).fill(0x11), new Uint8Array(HALF_BYTES).fill(0x11)];
      this.memoryPos = [0, 0];
      this.registerWrites = 0;

      // panel state
      this.powered = false;
      this.shown = new Uint8Array(WIDTH * HEIGHT).fill(1); // ink code per pixel, what is on the glass
      this.next = new Uint8Array(WIDTH * HEIGHT).fill(1);
      this.refreshing = false;
      this.refreshProgress = 0; // 0..1
      this.refreshDurationMs = 0; // in panel time
      this.shortBusyMs = 0; // panel time left of a short busy period (reset, power on)
      this.restLeftMs = 0; // panel time left until the next refresh is allowed
      this.hasRefreshed = false;
      this.refreshCount = 0;
      this.violations = 0;
      this.lastTick = performance.now();
      this.lastPaint = 0;

      this.paint();
      // setInterval, not requestAnimationFrame: the panel keeps working in a
      // hidden tab and under headless browsers, as the real one would.
      this.timer = setInterval(() => this.tick(), 66);
    }

    // ---- settings ----

    setSpeed(speed) {
      this.advance();
      this.speed = speed;
      if (speed === Infinity) this.restLeftMs = 0;
      this.changed();
    }

    setPalette(name) {
      this.paletteName = PALETTES[name] ? name : 'panel';
      this.paint();
    }

    // ---- what the firmware side may ask ----

    /** Real milliseconds the firmware has to leave between the end of one refresh and the next DRF. */
    minRefreshIntervalMs() {
      this.advance();
      if (!this.hasRefreshed || this.restLeftMs <= 0) return 0;
      // Expressed so that "now - end of last refresh >= this" is true exactly when the panel is ready.
      return Math.ceil(performance.now() - this.refreshEndedAt + this.scaleToReal(this.restLeftMs));
    }

    // ---- bus ----

    busReset(level) {
      this.advance();
      if (this.resetLevel === 1 && level === 0) {
        if (this.refreshing) this.violation('RESET pulled low during a refresh');
        this.command = [0, 0];
        this.memoryPos = [0, 0];
        this.registerWrites = 0;
        this.powered = false;
      }
      if (this.resetLevel === 0 && level === 1) this.shortBusyMs = Math.max(this.shortBusyMs, 100);
      this.resetLevel = level;
    }

    busCs(mask) {
      this.csMask = mask & 3;
    }

    busDc(level) {
      this.dc = level ? 1 : 0;
    }

    busWrite(bytes) {
      if (this.csMask === 0 || bytes.length === 0) return;
      this.advance();
      if (this.dc === 0) {
        for (let i = 0; i < bytes.length; i++) this.onCommand(bytes[i]);
        return;
      }
      for (let c = 0; c < 2; c++) {
        if (!(this.csMask & (1 << c))) continue;
        if (this.command[c] === CMD.DTM) {
          const room = HALF_BYTES - this.memoryPos[c];
          if (bytes.length > room) this.violation(`more than ${HALF_BYTES} pixel bytes sent to controller ${c}`);
          const n = Math.min(room, bytes.length);
          this.memory[c].set(bytes.subarray(0, n), this.memoryPos[c]);
          this.memoryPos[c] += n;
        } else if (this.command[c] === CMD.TRES && bytes.length === 4 && c === 0) {
          const w = (bytes[0] << 8) | bytes[1];
          if (w !== WIDTH) this.log('warn', `TRES sets width ${w}, this panel is ${WIDTH}`);
        }
      }
    }

    /** Level of the BUSY pin: 0 while busy, 1 when ready (active low, as on the panel). */
    busBusyLevel() {
      this.advance();
      return this.refreshing || this.shortBusyMs > 0 ? 0 : 1;
    }

    onCommand(cmd) {
      for (let c = 0; c < 2; c++) if (this.csMask & (1 << c)) this.command[c] = cmd;
      switch (cmd) {
        case CMD.DTM:
          if (this.refreshing) this.violation('pixel data sent during a refresh');
          for (let c = 0; c < 2; c++) if (this.csMask & (1 << c)) this.memoryPos[c] = 0;
          break;
        case CMD.PON:
          if (this.registerWrites < 10) this.log('warn', 'power on without the init sequence');
          this.powered = true;
          this.shortBusyMs = Math.max(this.shortBusyMs, 200);
          break;
        case CMD.DRF:
          this.startRefresh();
          break;
        case CMD.POF:
          if (this.refreshing) {
            this.violation('power off sent during a refresh (a real panel can latch a fault here)');
          } else {
            this.powered = false;
            this.shortBusyMs = Math.max(this.shortBusyMs, 100);
          }
          break;
        default:
          this.registerWrites++;
      }
    }

    startRefresh() {
      if (this.refreshing) {
        this.violation('refresh refused: the panel is already refreshing');
        return;
      }
      if (!this.powered) {
        this.violation('refresh refused: panel power is off (no PON)');
        return;
      }
      if (this.csMask !== 3) this.log('warn', 'DRF sent to one controller only');
      if (this.hasRefreshed && this.restLeftMs > 0) {
        this.violation(`refresh refused: only ${Math.round((MIN_INTERVAL_MS - this.restLeftMs) / 1000)} s since the last refresh, the panel needs ${MIN_INTERVAL_MS / 1000} s`);
        return;
      }
      for (let c = 0; c < 2; c++) {
        if (this.memoryPos[c] !== HALF_BYTES) this.log('warn', `controller ${c} got ${this.memoryPos[c]} of ${HALF_BYTES} pixel bytes; the rest keeps old data`);
      }
      // Unpack the controllers' memories into one ink code per pixel.
      let illegal = 0;
      for (let c = 0; c < 2; c++) {
        const mem = this.memory[c];
        for (let y = 0; y < HEIGHT; y++) {
          let o = y * WIDTH + c * HALF_WIDTH;
          const m = y * ROW_BYTES;
          for (let i = 0; i < ROW_BYTES; i++) {
            const b = mem[m + i];
            const hi = (b >> 4) & 7, lo = b & 7;
            if (hi === 4 || hi === 7 || lo === 4 || lo === 7 || b & 0x88) illegal++;
            this.next[o++] = hi;
            this.next[o++] = lo;
          }
        }
      }
      if (illegal) this.log('warn', `${illegal} bytes contain ink codes this panel does not have`);
      this.refreshing = true;
      this.refreshProgress = 0;
      this.refreshDurationMs = REFRESH_MIN_MS + Math.random() * (REFRESH_MAX_MS - REFRESH_MIN_MS);
      this.refreshCount++;
      this.log('info', `refresh ${this.refreshCount} started, will take ${(this.refreshDurationMs / 1000).toFixed(1)} s of panel time`);
      this.changed();
    }

    // ---- time ----

    /**
     * Panel seconds per real second. "Instant" still takes a moment (a
     * quarter of a second per refresh) so that BUSY goes low and high again
     * the way the driver expects; it drops the rest period altogether.
     */
    timeFactor() {
      return this.speed === Infinity ? 64 : this.speed;
    }

    scaleToReal(panelMs) {
      return panelMs / this.timeFactor();
    }

    /** Move panel time forward to now. */
    advance() {
      const now = performance.now();
      let dt = (now - this.lastTick) * this.timeFactor();
      this.lastTick = now;
      if (dt <= 0) return;
      if (this.shortBusyMs > 0) this.shortBusyMs = Math.max(0, this.shortBusyMs - dt);
      if (this.refreshing) {
        const left = (1 - this.refreshProgress) * this.refreshDurationMs;
        if (dt >= left) {
          dt -= left;
          this.refreshing = false;
          this.refreshProgress = 1;
          this.shown.set(this.next);
          this.hasRefreshed = true;
          this.refreshEndedAt = now;
          this.restLeftMs = this.speed === Infinity ? 0 : MIN_INTERVAL_MS;
          this.paint();
          this.log('info', `refresh ${this.refreshCount} done`);
          this.changed();
        } else {
          this.refreshProgress += dt / this.refreshDurationMs;
          dt = 0;
        }
      }
      if (!this.refreshing && this.restLeftMs > 0) {
        this.restLeftMs = Math.max(0, this.restLeftMs - dt);
        if (this.restLeftMs === 0) this.changed();
      }
    }

    tick() {
      this.advance();
      if (this.refreshing) this.paint();
      if (this.hooks.onTick) this.hooks.onTick();
    }

    /** Snapshot for the page's status line. */
    status() {
      this.advance();
      return {
        busy: this.refreshing || this.shortBusyMs > 0,
        refreshing: this.refreshing,
        refreshProgress: this.refreshProgress,
        refreshLeftRealMs: this.refreshing ? this.scaleToReal((1 - this.refreshProgress) * this.refreshDurationMs) : 0,
        restLeftRealMs: this.scaleToReal(this.restLeftMs),
        restFraction: this.restLeftMs / MIN_INTERVAL_MS,
        powered: this.powered,
        refreshCount: this.refreshCount,
        violations: this.violations,
      };
    }

    // ---- picture ----

    /**
     * Colour of a pixel that was ink `from` and will be ink `to`, at refresh
     * progress t. A rough model of what a Spectra 6 refresh looks like: the
     * old picture inverts, the whole screen flashes dark and light several
     * times with a ghost of the new picture, the colours then separate out
     * of a muddy state, and the picture settles.
     */
    transitionColour(pal, from, to, t) {
      const black = pal[0], white = pal[1];
      const oldC = pal[from], newC = pal[to];
      const inverse = (c) => [white[0] + black[0] - c[0], white[1] + black[1] - c[1], white[2] + black[2] - c[2]];
      if (t < 0.07) return mix(oldC, inverse(oldC), smooth(t / 0.07));
      if (t < 0.45) {
        // Six full screen flashes. The ghost of the new picture grows.
        const p = (t - 0.07) / 0.38;
        const cycle = p * 6;
        const phase = cycle - Math.floor(cycle);
        const dark = Math.floor(cycle) % 2 === 0;
        const a = dark ? black : white, b = dark ? white : black;
        const field = mix(b, a, smooth(phase / 0.25)); // quick swing, then hold
        const ghost = dark ? newC : inverse(newC);
        return mix(field, ghost, 0.08 + 0.22 * p);
      }
      if (t < 0.82) {
        // Colours separate: pigment by pigment, with slow dark and light pulses.
        const p = (t - 0.45) / 0.37;
        const grey = (newC[0] + newC[1] + newC[2]) / 3;
        const mud = mix([grey, grey * 0.93, grey * 0.8], newC, 0.25 + 0.6 * smooth(p));
        const pulse = Math.sin(p * Math.PI * 5);
        return pulse > 0 ? mix(mud, white, 0.28 * pulse * (1 - p)) : mix(mud, black, -0.38 * pulse * (1 - p));
      }
      // Settle, with one last faint kick.
      const p = (t - 0.82) / 0.18;
      const grey = (newC[0] + newC[1] + newC[2]) / 3;
      const nearly = mix([grey, grey * 0.93, grey * 0.8], newC, 0.85);
      const kick = p > 0.3 && p < 0.5 ? 0.12 : 0;
      return mix(mix(nearly, newC, smooth(p)), white, kick);
    }

    paint() {
      const pal = PALETTES[this.paletteName];
      const out = this.out32;
      const pack = (c) => (0xff000000 | (Math.round(c[2]) << 16) | (Math.round(c[1]) << 8) | Math.round(c[0])) >>> 0;
      if (!this.refreshing) {
        const lut = new Uint32Array(8);
        for (let i = 0; i < 8; i++) lut[i] = pack(pal[i]);
        const shown = this.shown;
        for (let i = 0; i < out.length; i++) out[i] = lut[shown[i]];
      } else {
        // One colour per (old ink, new ink) pair, then a table lookup per pixel.
        const lut = new Uint32Array(64);
        for (let from = 0; from < 8; from++) for (let to = 0; to < 8; to++) lut[(from << 3) | to] = pack(this.transitionColour(pal, from, to, this.refreshProgress));
        const shown = this.shown, next = this.next;
        for (let i = 0; i < out.length; i++) out[i] = lut[(shown[i] << 3) | next[i]];
      }
      this.ctx.putImageData(this.image, 0, 0);
    }

    // ---- reporting ----

    log(level, message) {
      if (this.hooks.log) this.hooks.log(level, `panel: ${message}`);
    }

    violation(message) {
      this.violations++;
      this.log('error', message);
      this.changed();
    }

    changed() {
      if (this.hooks.onChange) this.hooks.onChange();
    }
  }

  PanelSim.WIDTH = WIDTH;
  PanelSim.HEIGHT = HEIGHT;
  PanelSim.MIN_INTERVAL_MS = MIN_INTERVAL_MS;
  root.PanelSim = PanelSim;
})(typeof window !== 'undefined' ? window : globalThis);
