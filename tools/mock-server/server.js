#!/usr/bin/env node
// Mock of the Morning Newspaper platform for local development and tests.
// Implements docs/PROTOCOL.md with made-up test pattern pages. No dependencies.
//
//   node tools/mock-server/server.js [options]
//
//   --port N             listen port (default 8787)
//   --host ADDR          listen address (default 127.0.0.1; use 0.0.0.0 for a real board on your LAN)
//   --public-url URL     base URL put into verification_uri (default: taken from the request)
//   --pages N            pages in the issue (default 4)
//   --expires S          pairing code lifetime in seconds (default 900)
//   --interval S         pairing poll interval in seconds (default 5)
//   --check-minutes N    config.check_interval_minutes (default 30)
//   --page-minutes N     config.page_interval_minutes (default 0)
//   --render-delay S     answer 503 for frames until S seconds after a version appears (default 0)
//   --auto-adopt S       adopt every pairing code by itself S seconds after it was issued
//   --static DIR         also serve the emulator build from DIR at /emulator/
//
// Helper endpoints (not part of the protocol):
//   GET  /mock/status    pending codes, devices, issue
//   POST /mock/adopt     {"user_code": "K7QF-3MXD", "name": "Kitchen"}; no user_code adopts the newest pending code
//   POST /mock/bump      publish a new version of the issue (the display should redraw)
//   POST /mock/issue     {"pages": 6} or {"present": false}
//   POST /mock/config    {"check_interval_minutes": 1, "page_interval_minutes": 5}
//   POST /mock/remove    remove all devices (their tokens start to answer 401)
//   GET  /app/pair?code= a one button page that stands in for the real pairing page
'use strict';

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { testPage, frameToPng, FRAME_BYTES } = require('./frames.js');

const MODEL = 'inky-impression-13.3-spectra6';
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

function parseArgs(argv) {
  const opts = { port: 8787, host: '127.0.0.1', publicUrl: '', pages: 4, expires: 900, interval: 5, checkMinutes: 30, pageMinutes: 0, renderDelay: 0, autoAdopt: -1, static: '' };
  const names = { '--port': 'port', '--host': 'host', '--public-url': 'publicUrl', '--pages': 'pages', '--expires': 'expires', '--interval': 'interval', '--check-minutes': 'checkMinutes', '--page-minutes': 'pageMinutes', '--render-delay': 'renderDelay', '--auto-adopt': 'autoAdopt', '--static': 'static' };
  for (let i = 0; i < argv.length; i++) {
    const key = names[argv[i]];
    if (!key) {
      console.error(`unknown option ${argv[i]}`);
      process.exit(2);
    }
    const value = argv[++i];
    opts[key] = typeof opts[key] === 'number' ? Number(value) : value;
  }
  return opts;
}

function createMock(opts) {
  const pairings = new Map(); // device_code -> pairing
  const devices = new Map(); // token -> device
  const issue = { present: true, date: new Date().toISOString().slice(0, 10), number: 1, serial: 1, pages: opts.pages, publishedAt: Date.now() };
  const config = { check_interval_minutes: opts.checkMinutes, page_interval_minutes: opts.pageMinutes, timezone: 'Europe/Madrid', quiet_hours: null };
  const frameCache = new Map();

  const version = () => `mock-${issue.date}-${issue.serial}`;
  const normaliseCode = (code) => String(code || '').toUpperCase().replace(/[^A-Z0-9]/g, '');

  function newUserCode() {
    const bytes = crypto.randomBytes(8);
    let s = '';
    for (let i = 0; i < 8; i++) s += CODE_ALPHABET[bytes[i] % CODE_ALPHABET.length];
    return `${s.slice(0, 4)}-${s.slice(4)}`;
  }

  function adopt(userCode, name) {
    const wanted = normaliseCode(userCode);
    let found = null;
    for (const p of pairings.values()) {
      if (p.adopted || Date.now() >= p.expiresAt) continue;
      if (wanted ? normaliseCode(p.user_code) === wanted : !found || p.createdAt > found.createdAt) found = p;
    }
    if (!found) return null;
    found.adopted = true;
    found.name = name || 'Mock display';
    found.device_id = crypto.randomUUID();
    return found;
  }

  function frameFor(page) {
    const key = `${version()}/${page}`;
    if (!frameCache.has(key)) {
      if (frameCache.size > 32) frameCache.clear();
      frameCache.set(key, testPage(page, issue.pages, `VERSION ${version()}`));
    }
    return frameCache.get(key);
  }

  function json(res, status, body, extra = {}) {
    const data = Buffer.from(JSON.stringify(body));
    res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': data.length, ...extra });
    res.end(data);
  }

  function readBody(req) {
    return new Promise((resolve) => {
      const parts = [];
      let size = 0;
      req.on('data', (d) => {
        size += d.length;
        if (size < 65536) parts.push(d);
      });
      req.on('end', () => {
        try {
          resolve(JSON.parse(Buffer.concat(parts).toString('utf8') || '{}'));
        } catch {
          resolve(null);
        }
      });
    });
  }

  function deviceFor(req) {
    const m = /^Bearer (.+)$/.exec(req.headers.authorization || '');
    const device = m && devices.get(m[1]);
    if (device) {
      device.last_seen = new Date().toISOString();
      device.firmware = req.headers['x-device-firmware'] || device.firmware;
      device.rssi = req.headers['x-device-rssi'] || device.rssi;
    }
    return device || null;
  }

  const STATIC_TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml' };

  function serveStatic(res, rel) {
    const root = path.resolve(opts.static);
    const file = path.resolve(root, `.${path.posix.normalize(`/${rel || 'index.html'}`)}`);
    if (!file.startsWith(root + path.sep) && file !== root) return json(res, 404, { error: 'not found' });
    fs.readFile(file, (err, data) => {
      if (err) return json(res, 404, { error: 'not found' });
      res.writeHead(200, { 'Content-Type': STATIC_TYPES[path.extname(file)] || 'application/octet-stream', 'Content-Length': data.length, 'Cache-Control': 'no-store' });
      res.end(data);
    });
  }

  async function handle(req, res) {
    // The emulator may be served from another origin, so allow everything a display sends.
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Authorization, Content-Type, Accept, X-Device-Firmware, X-Device-Battery, X-Device-Rssi');
    res.setHeader('Access-Control-Expose-Headers', 'ETag, Retry-After');
    res.setHeader('Access-Control-Max-Age', '600');
    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      return res.end();
    }

    const url = new URL(req.url, 'http://mock');
    const route = `${req.method} ${url.pathname}`;
    const base = opts.publicUrl || `http://${req.headers.host}`;

    if (route === 'POST /api/v1/devices/pair/start') {
      const body = await readBody(req);
      if (!body || body.model !== MODEL) return json(res, 400, { error: `model must be ${MODEL}` });
      const p = {
        device_code: crypto.randomBytes(48).toString('base64url'),
        user_code: newUserCode(),
        hardware_id: String(body.hardware_id || ''),
        firmware: String(body.firmware || ''),
        emulator: body.emulator === true,
        createdAt: Date.now(),
        expiresAt: Date.now() + opts.expires * 1000,
        adopted: false,
        handedOver: false,
      };
      pairings.set(p.device_code, p);
      if (opts.autoAdopt >= 0) setTimeout(() => adopt(p.user_code, 'Auto adopted'), opts.autoAdopt * 1000).unref();
      console.log(`pair: code ${p.user_code} for ${p.hardware_id}${p.emulator ? ' (emulator)' : ''}`);
      return json(res, 200, {
        device_code: p.device_code,
        user_code: p.user_code,
        verification_uri: `${base}/app/pair`,
        verification_uri_complete: `${base}/app/pair?code=${p.user_code}`,
        expires_in: opts.expires,
        interval: opts.interval,
      });
    }

    if (route === 'POST /api/v1/devices/pair/poll') {
      const body = await readBody(req);
      const p = body && pairings.get(String(body.device_code || ''));
      if (!p) return json(res, 404, { error: 'unknown device_code' });
      if (p.handedOver || Date.now() >= p.expiresAt) return json(res, 410, { status: 'expired' });
      if (!p.adopted) return json(res, 200, { status: 'pending' });
      p.handedOver = true;
      const token = `newspaper_dev_${crypto.randomBytes(24).toString('base64url')}`;
      devices.set(token, { id: p.device_id, name: p.name, model: MODEL, hardware_id: p.hardware_id, emulator: p.emulator, firmware: p.firmware });
      console.log(`pair: ${p.user_code} adopted as "${p.name}"`);
      return json(res, 200, { status: 'adopted', device_id: p.device_id, name: p.name, token });
    }

    if (route === 'POST /api/v1/devices/adopt' || route === 'POST /mock/adopt') {
      const body = (await readBody(req)) || {};
      if (route === 'POST /api/v1/devices/adopt' && !body.user_code) return json(res, 400, { error: 'user_code is required' });
      const p = adopt(body.user_code, body.name);
      if (!p) return json(res, 404, { error: 'no such pending code' });
      return json(res, 200, { device_id: p.device_id, name: p.name, user_code: p.user_code });
    }

    if (route === 'GET /api/v1/device/state') {
      const device = deviceFor(req);
      if (!device) return json(res, 401, { error: 'unknown device token' });
      const pages = [];
      for (let i = 1; i <= issue.pages; i++) pages.push({ index: i, id: i === 1 ? 'front' : `page-${i}`, title: i === 1 ? 'Front Page' : `Test Page ${i}` });
      return json(res, 200, {
        device: { id: device.id, name: device.name, model: device.model },
        server_time: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
        config,
        issue: issue.present ? { date: issue.date, number: issue.number, version: version(), pages } : null,
      });
    }

    if (route === 'GET /api/v1/device/frame') {
      const device = deviceFor(req);
      if (!device) return json(res, 401, { error: 'unknown device token' });
      const page = Number(url.searchParams.get('page'));
      if (!issue.present || !Number.isInteger(page) || page < 1 || page > issue.pages) return json(res, 404, { error: 'no such page' });
      const wait = issue.publishedAt + opts.renderDelay * 1000 - Date.now();
      if (wait > 0) return json(res, 503, { error: 'frame is not rendered yet' }, { 'Retry-After': String(Math.ceil(wait / 1000)) });
      const frame = frameFor(page);
      const png = url.searchParams.get('format') === 'png';
      const data = png ? frameToPng(frame, url.searchParams.get('palette') || 'ideal') : frame;
      const etag = `"${version()}-${page}${png ? '-png' : ''}"`;
      if (req.headers['if-none-match'] === etag) {
        res.writeHead(304, { ETag: etag });
        return res.end();
      }
      res.writeHead(200, { 'Content-Type': png ? 'image/png' : 'application/octet-stream', 'Content-Length': data.length, ETag: etag, 'Cache-Control': 'private, max-age=60' });
      return res.end(data);
    }

    if (route === 'GET /mock/status') {
      return json(res, 200, {
        version: version(),
        issue,
        config,
        pending: [...pairings.values()].filter((p) => !p.handedOver && Date.now() < p.expiresAt).map((p) => ({ user_code: p.user_code, hardware_id: p.hardware_id, emulator: p.emulator, adopted: p.adopted })),
        devices: [...devices.values()],
      });
    }
    if (route === 'POST /mock/bump') {
      issue.serial++;
      issue.publishedAt = Date.now();
      console.log(`issue: version is now ${version()}`);
      return json(res, 200, { version: version() });
    }
    if (route === 'POST /mock/issue') {
      const body = (await readBody(req)) || {};
      if (typeof body.present === 'boolean') issue.present = body.present;
      if (Number.isInteger(body.pages) && body.pages >= 1 && body.pages <= 99) issue.pages = body.pages;
      if (typeof body.date === 'string') issue.date = body.date;
      issue.serial++;
      issue.publishedAt = Date.now();
      return json(res, 200, { version: version(), issue });
    }
    if (route === 'POST /mock/config') {
      const body = (await readBody(req)) || {};
      for (const key of ['check_interval_minutes', 'page_interval_minutes']) if (Number.isInteger(body[key]) && body[key] >= 0) config[key] = body[key];
      return json(res, 200, { config });
    }
    if (route === 'POST /mock/remove') {
      const n = devices.size;
      devices.clear();
      return json(res, 200, { removed: n });
    }

    if (route === 'GET /app/pair') {
      const code = normaliseCode(url.searchParams.get('code'));
      const html = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mock pairing</title>
<body style="font: 18px system-ui; max-width: 28rem; margin: 3rem auto; padding: 0 1rem">
<h1>Mock pairing page</h1>
<p>This stands in for the real pairing page of the platform.</p>
<form onsubmit="event.preventDefault(); fetch('/api/v1/devices/adopt', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({user_code: c.value, name: n.value})}).then(r => r.json()).then(j => out.textContent = j.error ? 'Error: ' + j.error : 'Adopted as ' + j.name + '. The display will show the paper shortly.')">
<p><label>Code<br><input id="c" value="${code}" style="font: inherit; padding: .4rem; width: 100%"></label></p>
<p><label>Name<br><input id="n" value="Kitchen" style="font: inherit; padding: .4rem; width: 100%"></label></p>
<p><button style="font: inherit; padding: .5rem 1rem">Add this display</button></p>
</form><p id="out"></p></body>`;
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      return res.end(html);
    }

    if (opts.static && req.method === 'GET' && url.pathname === '/emulator') {
      res.writeHead(302, { Location: '/emulator/' });
      return res.end();
    }
    if (opts.static && req.method === 'GET' && url.pathname.startsWith('/emulator/')) {
      return serveStatic(res, url.pathname.slice('/emulator/'.length));
    }
    if (route === 'GET /') {
      return json(res, 200, { name: 'Morning Newspaper mock platform', protocol: 1, frame_bytes: FRAME_BYTES, help: 'see tools/mock-server/server.js', emulator: opts.static ? '/emulator/' : null });
    }
    return json(res, 404, { error: 'not found' });
  }

  const server = http.createServer((req, res) => {
    handle(req, res).catch((err) => {
      console.error(err);
      if (!res.headersSent) json(res, 500, { error: 'mock server error' });
    });
  });
  return { server, adopt, issue, config, devices, pairings };
}

if (require.main === module) {
  const opts = parseArgs(process.argv.slice(2));
  const { server } = createMock(opts);
  server.listen(opts.port, opts.host, () => {
    const base = `http://${opts.host === '0.0.0.0' ? 'localhost' : opts.host}:${opts.port}`;
    console.log(`mock platform on ${base} (pid ${process.pid})`);
    if (opts.static) console.log(`emulator: ${base}/emulator/?api=${base}`);
    console.log(`adopt a display: node tools/mock-server/adopt.js --url ${base} [CODE]`);
  });
}

module.exports = { createMock, parseArgs };
