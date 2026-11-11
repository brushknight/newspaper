#!/usr/bin/env node
// End to end check of the browser emulator against the mock platform, in
// headless Chrome, driven over the DevTools protocol. No dependencies
// (needs Node 22 or newer for the built-in WebSocket).
//
//   node tools/e2e/emulator-e2e.js [--dist targets/wasm/dist] [--out DIR] [--slow]
//
// Default run (panel speed "instant"): pairing screen appears with the code
// the platform issued, the display is adopted, page 1 is drawn and matches
// the platform's frame pixel for pixel, Next draws page 2, a new version is
// picked up... and the panel simulator reports no rule violations.
//
// --slow adds a run at 4x panel speed that checks the refresh really takes
// time with BUSY asserted, and that the first page waits for the panel's
// rest period (about a minute of real time).
//
// Chrome is found through $CHROME, then the usual install locations.
// Extra Chrome flags can be given in $CHROME_ARGS (CI uses --no-sandbox).
'use strict';

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createMock } = require('../mock-server/server.js');
const { testPage, WIDTH, HEIGHT, HALF_BYTES } = require('../mock-server/frames.js');

const args = process.argv.slice(2);
const option = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const dist = path.resolve(option('--dist', path.join(__dirname, '../../targets/wasm/dist')));
const outDir = option('--out', '');
const slow = args.includes('--slow');

function findChrome() {
  const candidates = [
    process.env.CHROME,
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ];
  return candidates.find((c) => c && fs.existsSync(c));
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// FNV-1a over the ink codes; the same function runs in the page.
const HASH_SOURCE = `(function (codes) { let h = 0x811c9dc5; for (let i = 0; i < codes.length; i++) { h ^= codes[i]; h = Math.imul(h, 0x01000193); } return h >>> 0; })`;
const hash = eval(HASH_SOURCE); // eslint-disable-line no-eval

function unpack(frame) {
  const codes = new Uint8Array(WIDTH * HEIGHT);
  for (let half = 0; half < 2; half++) {
    for (let y = 0; y < HEIGHT; y++) {
      let o = y * WIDTH + half * (WIDTH / 2);
      for (let i = 0; i < 300; i++) {
        const b = frame[half * HALF_BYTES + y * 300 + i];
        codes[o++] = b >> 4;
        codes[o++] = b & 15;
      }
    }
  }
  return codes;
}

class Browser {
  static async launch() {
    const chrome = findChrome();
    if (!chrome) throw new Error('Chrome not found; set CHROME to its path');
    const extraArgs = (process.env.CHROME_ARGS || '').split(' ').filter(Boolean); // for example --no-sandbox in CI
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'np-e2e-'));
    const child = spawn(chrome, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--window-size=1320,980', ...extraArgs, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
    const wsUrl = await new Promise((resolve, reject) => {
      let text = '';
      const timer = setTimeout(() => reject(new Error(`Chrome did not start: ${text}`)), 20000);
      child.stderr.on('data', (d) => {
        text += d;
        const m = /DevTools listening on (ws:\/\/\S+)/.exec(text);
        if (m) {
          clearTimeout(timer);
          resolve(m[1]);
        }
      });
      child.on('exit', (code) => reject(new Error(`Chrome exited with ${code}: ${text}`)));
    });
    const b = new Browser();
    b.child = child;
    b.profile = profile;
    b.nextId = 1;
    b.waiting = new Map();
    b.console = [];
    b.ws = new WebSocket(wsUrl);
    await new Promise((resolve, reject) => {
      b.ws.onopen = resolve;
      b.ws.onerror = () => reject(new Error('cannot connect to Chrome'));
    });
    b.ws.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      if (msg.id && b.waiting.has(msg.id)) {
        const { resolve, reject } = b.waiting.get(msg.id);
        b.waiting.delete(msg.id);
        msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
      } else if (msg.method === 'Runtime.consoleAPICalled') {
        b.console.push(msg.params.args.map((a) => a.value).join(' '));
      } else if (msg.method === 'Runtime.exceptionThrown') {
        b.console.push(`EXCEPTION ${msg.params.exceptionDetails.text} ${msg.params.exceptionDetails.exception?.description || ''}`);
      }
    };
    return b;
  }

  send(method, params = {}, sessionId) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.waiting.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params, sessionId }));
    });
  }

  async open(url) {
    const { targetId } = await this.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await this.send('Target.attachToTarget', { targetId, flatten: true });
    await this.send('Runtime.enable', {}, sessionId);
    await this.send('Page.enable', {}, sessionId);
    await this.send('Page.navigate', { url }, sessionId);
    return new Page(this, sessionId);
  }

  close() {
    try {
      this.ws.close();
    } catch {}
    this.child.kill(); // by the PID we started
    try {
      fs.rmSync(this.profile, { recursive: true, force: true });
    } catch {}
  }
}

class Page {
  constructor(browser, sessionId) {
    this.browser = browser;
    this.sessionId = sessionId;
  }
  async eval(expression) {
    const r = await this.browser.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }, this.sessionId);
    if (r.exceptionDetails) throw new Error(`page error: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
    return r.result.value;
  }
  // Real time polling. Headless virtual time would stall the emulator's timers.
  async waitFor(expression, what, timeoutMs) {
    const start = Date.now();
    for (;;) {
      let value;
      try {
        value = await this.eval(expression);
      } catch {
        value = false; // page still loading
      }
      if (value) return (Date.now() - start) / 1000;
      if (Date.now() - start > timeoutMs) throw new Error(`timed out after ${timeoutMs / 1000} s waiting for: ${what}`);
      await sleep(100);
    }
  }
  async screenshot(name) {
    if (!outDir) return;
    fs.mkdirSync(outDir, { recursive: true });
    const { data } = await this.browser.send('Page.captureScreenshot', { format: 'png' }, this.sessionId);
    const file = path.join(outDir, name);
    fs.writeFileSync(file, Buffer.from(data, 'base64'));
    console.log(`    screenshot ${file}`);
  }
}

const results = [];
function check(ok, what) {
  results.push(ok);
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`);
}

const shownHash = `${HASH_SOURCE}(newspaperEmulator.panel.shown)`;
const shownIs = (page, pages, label) => hash(unpack(testPage(page, pages, label)));

async function main() {
  if (!fs.existsSync(path.join(dist, 'newspaper.wasm'))) throw new Error(`no emulator build in ${dist}; run targets/wasm/build.sh first`);
  const mock = createMock({ port: 0, host: '127.0.0.1', publicUrl: '', pages: 3, expires: 900, interval: 1, checkMinutes: 1, pageMinutes: 0, renderDelay: 0, autoAdopt: -1, static: dist });
  await new Promise((resolve) => mock.server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${mock.server.address().port}`;
  const version = () => `mock-${mock.issue.date}-${mock.issue.serial}`;
  const browser = await Browser.launch();
  const pendingCode = () => [...mock.pairings.values()].filter((p) => !p.adopted).map((p) => p.user_code).pop();

  try {
    console.log('instant panel speed');
    const page = await browser.open(`${base}/emulator/?api=self&speed=instant&device=e2e&reset=1`);
    await page.waitFor(`window.newspaperEmulator && newspaperEmulator.state.state === 'pairing' && newspaperEmulator.state.screen === 'pair'`, 'pairing screen drawn', 30000);
    const shownCode = await page.eval('newspaperEmulator.state.user_code');
    check(shownCode && shownCode === pendingCode(), `pairing screen is up with the code the platform issued (${shownCode})`);
    const ink = await page.eval(`(() => { const s = newspaperEmulator.panel.shown; const n = [0,0,0,0,0,0,0,0]; for (let i = 0; i < s.length; i++) n[s[i]]++; return n; })()`);
    check(ink[0] > 50000 && ink[1] > 1000000 && ink[2] > 1000 && ink[3] > 1000 && ink[4] + ink[5] + ink[6] + ink[7] === 0, `pairing screen has black, white and accent ink only (black ${ink[0]}, yellow ${ink[2]}, red ${ink[3]})`);
    check([...mock.pairings.values()].pop().emulator === true, 'pairing request carried "emulator": true');
    await page.screenshot('1-pairing.png');

    mock.adopt(shownCode, 'E2E');
    await page.waitFor(`newspaperEmulator.state.screen === 'frame' && newspaperEmulator.state.page === 1`, 'page 1 drawn', 30000);
    check((await page.eval(shownHash)) === shownIs(1, 3, `VERSION ${version()}`), 'page 1 on the panel equals the platform frame, pixel for pixel');
    await page.screenshot('2-page1.png');

    await page.eval(`newspaperEmulator.press('next')`);
    await page.waitFor(`newspaperEmulator.state.page === 2`, 'page 2 drawn', 30000);
    check((await page.eval(shownHash)) === shownIs(2, 3, `VERSION ${version()}`), 'Next draws page 2');
    await page.eval(`newspaperEmulator.press('back'); newspaperEmulator.press('back')`);
    await page.waitFor(`newspaperEmulator.state.page === 3`, 'page 3 drawn', 30000);
    check((await page.eval(shownHash)) === shownIs(3, 3, `VERSION ${version()}`), 'Back twice wraps to page 3');
    await page.eval(`newspaperEmulator.set({colors: 'ideal'})`);
    await page.screenshot('3-page3-ideal-colours.png');

    const mockStartCount = async () => mock.pairings.size;
    const startsBefore = mock.pairings.size + 1; // the 401 below causes one more
    mock.devices.clear(); // removed in the cloud: the next call answers 401
    await page.eval(`newspaperEmulator.press('home')`);
    await page.waitFor(`newspaperEmulator.state.state === 'pairing' && newspaperEmulator.state.screen === 'pair'`, 'back to pairing after 401', 30000);
    const secondCode = await page.eval('newspaperEmulator.state.user_code');
    check(secondCode && secondCode !== shownCode, `401 leads back to pairing with a new code (${secondCode})`);
    check((await page.eval('newspaperEmulator.panel.violations')) === 0, 'panel simulator saw no rule violations');
    const errors = browser.console.filter((l) => /EXCEPTION|\[error\]/.test(l));
    check(errors.length === 0, `no errors in the page console${errors.length ? `: ${errors[0]}` : ''}`);

    console.log('Wi-Fi provisioning stand-in, embedded layout');
    const p3 = await browser.open(`${base}/emulator/?api=self&speed=instant&device=e2e-wifi&reset=1&wifi=0`);
    await p3.waitFor(`window.newspaperEmulator && newspaperEmulator.state.state === 'provisioning' && newspaperEmulator.state.screen === 'prov'`, 'provisioning screen drawn', 30000);
    check((await mockStartCount()) === startsBefore, 'no pairing request is made before Wi-Fi is set up');
    await p3.screenshot('7-provisioning.png');
    await p3.eval(`document.getElementById('portal-save').click()`);
    await p3.waitFor(`newspaperEmulator.state.state === 'pairing' && newspaperEmulator.state.screen === 'pair'`, 'pairing after Wi-Fi setup', 30000);
    check(true, 'after the setup page is submitted the display moves on to pairing');
    const p4 = await browser.open(`${base}/emulator/?api=self&speed=instant&device=e2e-embed&reset=1&embed=1`);
    await p4.waitFor(`window.newspaperEmulator && newspaperEmulator.state.screen === 'pair'`, 'embedded emulator pairing screen', 30000);
    check((await p4.eval(`getComputedStyle(document.querySelector('.side')).display`)) === 'none', 'embed=1 hides everything but the panel and its buttons');
    await p4.screenshot('8-embed.png');

    if (slow) {
      console.log('4x panel speed (refresh 3 to 5 s, rest period 45 s)');
      const p2 = await browser.open(`${base}/emulator/?api=self&speed=4&device=e2e-slow&reset=1`);
      await p2.waitFor(`window.newspaperEmulator && newspaperEmulator.panel.status().refreshing`, 'first refresh running', 30000);
      await sleep(1200);
      const mid = await p2.eval(`(() => { const s = newspaperEmulator.panel.status(); return { refreshing: s.refreshing, busy: newspaperEmulator.panel.busBusyLevel() === 0, shownBlank: newspaperEmulator.panel.shown.every((v) => v === 1) }; })()`);
      check(mid.refreshing && mid.busy, 'a second into the refresh the panel is still refreshing and BUSY is asserted');
      check(mid.shownBlank, 'the new picture is not on the glass until the refresh ends');
      await p2.screenshot('4-mid-refresh.png');
      const took = 1.2 + (await p2.waitFor(`newspaperEmulator.state.screen === 'pair' && !newspaperEmulator.panel.status().refreshing`, 'pairing refresh done', 30000));
      check(took >= 2.5 && took <= 7, `refresh took ${took.toFixed(1)} s at 4x (12 to 20 s of panel time)`);
      const code = await p2.eval('newspaperEmulator.state.user_code');
      mock.adopt(code, 'E2E slow');
      await sleep(5000);
      const waiting = await p2.eval(`({ screen: newspaperEmulator.state.screen, state: newspaperEmulator.state.state, rest: newspaperEmulator.panel.status().restLeftRealMs })`);
      check(waiting.state === 'running' && waiting.screen === 'pair' && waiting.rest > 20000, `adopted, but the page waits for the panel's rest period (${Math.round(waiting.rest / 1000)} s left)`);
      await p2.screenshot('5-resting.png');
      const waited = 5 + (await p2.waitFor(`newspaperEmulator.state.screen === 'frame'`, 'page 1 drawn after the rest period', 90000));
      check(waited >= 40, `page 1 was drawn ${waited.toFixed(0)} s after adoption, not before the 45 s rest was over`);
      check((await p2.eval('newspaperEmulator.panel.violations')) === 0, 'panel simulator saw no rule violations');
      await p2.screenshot('6-page1-panel-colours.png');
    }
  } catch (err) {
    console.log('  page console, last lines:');
    for (const line of browser.console.slice(-25)) console.log(`    ${line}`);
    throw err;
  } finally {
    browser.close();
    mock.server.close();
  }
  const failed = results.filter((r) => !r).length;
  console.log(failed ? `${failed} of ${results.length} checks FAILED` : `all ${results.length} checks passed`);
  process.exit(failed ? 1 : 0);
}

main().catch((err) => {
  console.error(`FAIL ${err.message}`);
  process.exit(1);
});
