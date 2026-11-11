// Page logic of the display emulator: reads the settings from the query
// string, wires the page to the panel simulator, and implements the "host"
// object that the WebAssembly HAL (targets/wasm/main.c) calls into.
//
// Query parameters (all optional):
//   api=URL        platform base URL (default https://newspaper.brushknight.com;
//                  "self" means the origin this page is served from)
//   speed=1|4|instant      panel time scale (default 1, real time)
//   colors=panel|ideal     ink colours as on the panel, or pure colours
//   device=NAME    storage namespace, so several virtual displays can coexist
//   reset=1        forget the stored token first (a factory reset)
//   wifi=0         start without Wi-Fi credentials, to see the provisioning screen
//   embed=1        only the framed panel and its buttons, for use in an iframe
//   log=0          hide the log
//
// When embedded, the page talks to its parent with postMessage:
//   out: {source: "newspaper-emulator", type: "state", state, user_code,
//         verification_uri_complete, screen, page, pages, refreshing}
//   in:  {target: "newspaper-emulator", type: "button", button: "next" | "back" | "home"}
//        {target: "newspaper-emulator", type: "set", speed: "4", colors: "ideal"}
'use strict';

(function () {
  const params = new URLSearchParams(location.search);
  const $ = (id) => document.getElementById(id);

  // ---- settings ----

  function resolveApi() {
    const raw = params.get('api');
    if (!raw) return 'https://newspaper.brushknight.com';
    if (raw === 'self') return location.origin;
    try {
      return new URL(raw, location.href).href.replace(/\/+$/, '');
    } catch {
      return raw;
    }
  }

  const settings = {
    api: resolveApi(),
    speed: ['1', '4', 'instant'].includes(params.get('speed')) ? params.get('speed') : '1',
    colors: params.get('colors') === 'ideal' ? 'ideal' : 'panel',
    device: (params.get('device') || 'default').replace(/[^\w.-]/g, '').slice(0, 40) || 'default',
  };
  if (params.get('embed') === '1') document.body.classList.add('embed');
  if (params.get('log') === '0') document.body.classList.add('no-log');

  // ---- storage: localStorage with a memory fallback (third party iframes may be denied storage) ----

  const prefix = `newspaper-emu:${settings.device}:`;
  const memory = new Map();
  let persistent = true;
  try {
    localStorage.setItem(`${prefix}probe`, '1');
    localStorage.removeItem(`${prefix}probe`);
  } catch {
    persistent = false;
  }
  const store = {
    get: (key) => (persistent ? localStorage.getItem(prefix + key) : memory.has(key) ? memory.get(key) : null),
    set: (key, value) => {
      if (value === null) persistent ? localStorage.removeItem(prefix + key) : memory.delete(key);
      else persistent ? localStorage.setItem(prefix + key, value) : memory.set(key, value);
    },
    clear: () => {
      if (!persistent) return memory.clear();
      for (let i = localStorage.length - 1; i >= 0; i--) {
        const k = localStorage.key(i);
        if (k && k.startsWith(prefix)) localStorage.removeItem(k);
      }
    },
  };

  if (params.get('reset') === '1') store.clear();
  // The simulated panel starts blank on every page load, unlike real e-paper
  // which keeps its picture. Tell the core so: nothing is on screen, and the
  // panel has not been refreshed recently.
  store.set('screen', null);
  store.set('rt_refresh', null);

  let hardwareId = store.get('emu_hwid');
  if (!hardwareId) {
    const bytes = crypto.getRandomValues(new Uint8Array(6));
    hardwareId = `emu-${[...bytes].map((b) => b.toString(16).padStart(2, '0')).join('')}`;
    store.set('emu_hwid', hardwareId);
  }

  // ---- log ----

  const LEVELS = ['debug', 'info', 'warn', 'error'];
  const logEl = $('log');
  function log(level, message) {
    const name = typeof level === 'number' ? LEVELS[level] || 'info' : level;
    const line = document.createElement('div');
    line.className = name;
    line.textContent = `${new Date().toLocaleTimeString([], { hour12: false })} ${message}`;
    const stick = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 8;
    logEl.appendChild(line);
    while (logEl.childElementCount > 400) logEl.firstElementChild.remove();
    if (stick) logEl.scrollTop = logEl.scrollHeight;
    (name === 'error' ? console.error : console.log)(`[${name}] ${message}`);
  }
  if (!persistent) log('warn', 'browser storage is not available here; the pairing will not survive a reload');

  // ---- panel ----

  const panel = new PanelSim($('panel'), { log, onTick: renderPanelStatus, onChange: renderPanelStatus });
  const speedValue = (s) => (s === 'instant' ? Infinity : Number(s));
  panel.setSpeed(speedValue(settings.speed));
  panel.setPalette(settings.colors);

  function renderPanelStatus() {
    const s = panel.status();
    const led = $('led');
    let text;
    let fraction = 0;
    if (s.refreshing) {
      led.className = 'led busy';
      text = `BUSY: refreshing, about ${Math.ceil(s.refreshLeftRealMs / 1000)} s left`;
      fraction = s.refreshProgress;
    } else if (s.restLeftRealMs > 0) {
      led.className = 'led rest';
      text = `Resting: next refresh allowed in ${Math.ceil(s.restLeftRealMs / 1000)} s`;
      fraction = 1 - s.restFraction;
    } else {
      led.className = 'led';
      text = s.busy ? 'BUSY' : 'Ready';
    }
    $('panel-status').textContent = text;
    $('bar').style.width = `${Math.round(fraction * 100)}%`;
    $('bar').style.background = s.refreshing ? 'var(--accent)' : 'var(--muted)';
    $('violations').textContent = s.violations ? `${s.violations} panel rule violation${s.violations > 1 ? 's' : ''}` : '';
  }

  // ---- buttons ----

  const BUTTONS = { next: 1, back: 2, home: 3 };
  const queue = [];
  let wakeIdle = null;
  function press(name) {
    if (!BUTTONS[name]) return;
    queue.push(BUTTONS[name]);
    if (wakeIdle) wakeIdle();
  }
  $('btn-next').addEventListener('click', () => press('next'));
  $('btn-back').addEventListener('click', () => press('back'));
  $('btn-home').addEventListener('click', () => press('home'));
  window.addEventListener('keydown', (e) => {
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
    if (e.key === 'ArrowRight') press('next');
    else if (e.key === 'ArrowLeft') press('back');
    else if (e.key === 'Home' || e.key === 'h' || e.key === 'H') press('home');
  });

  // ---- controls ----

  $('api').textContent = settings.api;
  $('speed').value = settings.speed;
  $('colors').value = settings.colors;
  function applySettings(next) {
    if (next.speed && ['1', '4', 'instant'].includes(String(next.speed))) {
      settings.speed = String(next.speed);
      $('speed').value = settings.speed;
      panel.setSpeed(speedValue(settings.speed));
      if (wakeIdle) wakeIdle(); // the core's wait for the rest period may be shorter now
    }
    if (next.colors === 'panel' || next.colors === 'ideal') {
      settings.colors = next.colors;
      $('colors').value = settings.colors;
      panel.setPalette(settings.colors);
    }
  }
  $('speed').addEventListener('change', (e) => applySettings({ speed: e.target.value }));
  $('colors').addEventListener('change', (e) => applySettings({ colors: e.target.value }));
  $('forget').addEventListener('click', () => {
    store.clear();
    location.reload();
  });
  $('toggle-log').addEventListener('click', (e) => {
    const hidden = document.body.classList.toggle('no-log');
    e.target.textContent = hidden ? 'Show log' : 'Hide log';
  });
  if (document.body.classList.contains('no-log')) $('toggle-log').textContent = 'Show log';

  // ---- Wi-Fi provisioning stand-in ----

  let wifiReady = params.get('wifi') !== '0' || store.get('emu_wifi') === '1';
  let provisioned = false;
  $('portal-save').addEventListener('click', () => {
    provisioned = true;
    wifiReady = true;
    store.set('emu_wifi', '1');
    $('portal').classList.remove('on');
    log('info', `wifi: "${$('portal-ssid').value}" saved by the setup page stand-in`);
  });

  // ---- state shown on the page and sent to an embedding page ----

  const STATE_NAMES = ['boot', 'provisioning', 'pair_start', 'pairing', 'pairing_paused', 'running'];
  let lastReport = '';
  const current = { state: 'boot' };
  function report(state, refreshing, userCode, uri, screen, page, pages) {
    const name = STATE_NAMES[state] || 'unknown';
    const key = [name, refreshing, userCode, screen, page, pages].join('|');
    if (key === lastReport) return;
    lastReport = key;
    Object.assign(current, { state: name, refreshing, user_code: userCode, verification_uri_complete: uri, screen, page, pages });

    let text = { boot: 'Starting', provisioning: 'Waiting for Wi-Fi setup', pair_start: 'Asking for a pairing code', pairing: 'Waiting to be added to an account', pairing_paused: 'Pairing paused, press a button', running: 'Paired' }[name];
    if (name === 'running') {
      if (screen === 'frame') text = `Showing page ${page} of ${pages}`;
      else if (pages > 0) text = 'Paired, waiting to draw the first page';
      else text = 'Paired, no issue published yet';
    }
    $('state').textContent = text;
    const pairing = name === 'pairing' && userCode;
    $('code-label').hidden = $('code-value').hidden = !pairing;
    if (pairing) {
      $('code').textContent = userCode;
      $('pair-link').href = uri;
    }
    if (window.parent !== window) window.parent.postMessage({ source: 'newspaper-emulator', type: 'state', ...current }, '*');
  }

  window.addEventListener('message', (e) => {
    const m = e.data;
    if (!m || m.target !== 'newspaper-emulator') return;
    if (m.type === 'button') press(String(m.button));
    if (m.type === 'set') applySettings(m);
  });

  // ---- http ----

  const transfers = new Map();
  let nextHandle = 1;

  const host = {
    panel,
    config: { api: settings.api, hardwareId },
    log,
    report,

    sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),

    // Wait for the time to pass or for a button, whichever is first.
    idle: (ms) =>
      new Promise((resolve) => {
        if (queue.length) return resolve();
        const timer = setTimeout(done, Math.min(ms, 0x7fffffff));
        function done() {
          clearTimeout(timer);
          wakeIdle = null;
          resolve();
        }
        wakeIdle = done;
      }),

    buttonPoll: () => queue.shift() || 0,

    storageGet: (key) => store.get(key),
    storageSet: (key, value) => store.set(key, value),

    wifiHasCredentials: () => wifiReady,
    provisioningStart: () => $('portal').classList.add('on'),
    provisioningDone: () => provisioned,

    async httpOpen(method, url, headerLines, body) {
      const headers = {};
      for (const line of headerLines.split('\n')) {
        const i = line.indexOf(': ');
        if (i > 0) headers[line.slice(0, i)] = line.slice(i + 2);
      }
      try {
        const response = await fetch(url, { method, headers, body: body === null ? undefined : body, cache: 'no-store', credentials: 'omit' });
        const handle = nextHandle++;
        transfers.set(handle, { response, reader: response.body ? response.body.getReader() : null, pending: null, offset: 0 });
        return handle;
      } catch (err) {
        log('warn', `http: ${method} ${url} failed: ${err.message} (network, or the platform does not allow this origin with CORS)`);
        return -1;
      }
    },
    httpStatus: (handle) => transfers.get(handle).response.status,
    httpRetryAfter(handle) {
      const value = parseInt(transfers.get(handle).response.headers.get('Retry-After') || '', 10);
      return Number.isFinite(value) ? value : -1;
    },
    // Up to `cap` bytes of the body; an empty array at the end; null on error.
    async httpRead(handle, cap) {
      const t = transfers.get(handle);
      try {
        if (!t.reader) return new Uint8Array(0);
        while (!t.pending || t.offset >= t.pending.length) {
          const { done, value } = await t.reader.read();
          if (done) return new Uint8Array(0);
          t.pending = value;
          t.offset = 0;
        }
        const piece = t.pending.subarray(t.offset, t.offset + cap);
        t.offset += piece.length;
        return piece;
      } catch (err) {
        log('warn', `http: body read failed: ${err.message}`);
        return null;
      }
    },
    httpClose(handle) {
      const t = transfers.get(handle);
      transfers.delete(handle);
      if (t && t.reader) t.reader.cancel().catch(() => {});
    },
  };

  // For tests and for poking around in the console.
  window.newspaperEmulator = { press, panel, settings, state: current, set: applySettings };

  renderPanelStatus();
  createNewspaper({ host }).catch((err) => log('error', `emulator failed to start: ${err.message}`));
})();
