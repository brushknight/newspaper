// Self test of the mock platform against docs/PROTOCOL.md.
//   node --test tools/mock-server/test.js
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { createMock } = require('./server.js');
const { FRAME_BYTES } = require('./frames.js');

async function withMock(overrides, fn) {
  const opts = { port: 0, host: '127.0.0.1', publicUrl: '', pages: 3, expires: 900, interval: 5, checkMinutes: 30, pageMinutes: 0, renderDelay: 0, autoAdopt: -1, static: '', ...overrides };
  const mock = createMock(opts);
  await new Promise((resolve) => mock.server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${mock.server.address().port}`;
  const post = (path, body, headers = {}) => fetch(base + path, { method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) });
  try {
    await fn({ mock, base, post });
  } finally {
    mock.server.close();
  }
}

const START = { model: 'inky-impression-13.3-spectra6', hardware_id: 'test-1', firmware: '0.0.0' };

test('pairing: start, pending, adopt, token once, then expired', async () => {
  await withMock({}, async ({ base, post }) => {
    const start = await (await post('/api/v1/devices/pair/start', START)).json();
    assert.match(start.user_code, /^[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{4}-[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{4}$/);
    assert.ok(start.device_code.length >= 64);
    assert.equal(start.verification_uri, `${base}/app/pair`);
    assert.equal(start.verification_uri_complete, `${base}/app/pair?code=${start.user_code}`);
    assert.equal(start.expires_in, 900);
    assert.equal(start.interval, 5);

    let poll = await post('/api/v1/devices/pair/poll', { device_code: start.device_code });
    assert.equal(poll.status, 200);
    assert.deepEqual(await poll.json(), { status: 'pending' });

    assert.equal((await post('/api/v1/devices/pair/poll', { device_code: 'nope' })).status, 404);

    // The code is accepted without the dash and in lower case.
    const adopt = await post('/api/v1/devices/adopt', { user_code: start.user_code.replace('-', '').toLowerCase(), name: 'Kitchen' });
    assert.equal(adopt.status, 200);

    poll = await post('/api/v1/devices/pair/poll', { device_code: start.device_code });
    const adopted = await poll.json();
    assert.equal(adopted.status, 'adopted');
    assert.equal(adopted.name, 'Kitchen');
    assert.match(adopted.token, /^newspaper_dev_/);

    poll = await post('/api/v1/devices/pair/poll', { device_code: start.device_code });
    assert.equal(poll.status, 410);
    assert.deepEqual(await poll.json(), { status: 'expired' });
  });
});

test('pairing: a code that ran out answers 410', async () => {
  await withMock({ expires: 0 }, async ({ post }) => {
    const start = await (await post('/api/v1/devices/pair/start', START)).json();
    assert.equal((await post('/api/v1/devices/pair/poll', { device_code: start.device_code })).status, 410);
  });
});

test('device API: state, frames, 401 after removal', async () => {
  await withMock({}, async ({ mock, base, post }) => {
    assert.equal((await fetch(`${base}/api/v1/device/state`)).status, 401);

    const start = await (await post('/api/v1/devices/pair/start', START)).json();
    await post('/mock/adopt', {});
    const { token } = await (await post('/api/v1/devices/pair/poll', { device_code: start.device_code })).json();
    const auth = { Authorization: `Bearer ${token}` };

    const state = await (await fetch(`${base}/api/v1/device/state`, { headers: auth })).json();
    assert.equal(state.device.model, START.model);
    assert.equal(state.config.check_interval_minutes, 30);
    assert.equal(state.issue.pages.length, 3);
    assert.equal(state.issue.pages[0].index, 1);

    const frame = await fetch(`${base}/api/v1/device/frame?page=2&version=${encodeURIComponent(state.issue.version)}`, { headers: auth });
    assert.equal(frame.status, 200);
    assert.equal(frame.headers.get('content-type'), 'application/octet-stream');
    assert.ok(frame.headers.get('etag'));
    const bytes = new Uint8Array(await frame.arrayBuffer());
    assert.equal(bytes.length, FRAME_BYTES);
    const legal = new Set([0, 1, 2, 3, 5, 6]);
    const used = new Set();
    for (const b of bytes) {
      used.add(b >> 4);
      used.add(b & 15);
    }
    for (const code of used) assert.ok(legal.has(code), `illegal ink code ${code}`);
    assert.equal(used.size, 6, 'the test pattern uses all six inks');

    const png = await fetch(`${base}/api/v1/device/frame?page=2&format=png`, { headers: auth });
    assert.equal(png.headers.get('content-type'), 'image/png');
    const sig = new Uint8Array(await png.arrayBuffer()).subarray(0, 8);
    assert.deepEqual([...sig], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

    assert.equal((await fetch(`${base}/api/v1/device/frame?page=4`, { headers: auth })).status, 404);

    const before = state.issue.version;
    await post('/mock/bump', {});
    const after = (await (await fetch(`${base}/api/v1/device/state`, { headers: auth })).json()).issue.version;
    assert.notEqual(after, before);

    await post('/mock/issue', { present: false });
    assert.equal((await (await fetch(`${base}/api/v1/device/state`, { headers: auth })).json()).issue, null);

    mock.devices.clear();
    assert.equal((await fetch(`${base}/api/v1/device/state`, { headers: auth })).status, 401);
  });
});

test('frames answer 503 with Retry-After while "rendering"', async () => {
  await withMock({ renderDelay: 30 }, async ({ base, post }) => {
    const start = await (await post('/api/v1/devices/pair/start', START)).json();
    await post('/mock/adopt', { user_code: start.user_code });
    const { token } = await (await post('/api/v1/devices/pair/poll', { device_code: start.device_code })).json();
    const res = await fetch(`${base}/api/v1/device/frame?page=1`, { headers: { Authorization: `Bearer ${token}` } });
    assert.equal(res.status, 503);
    assert.ok(Number(res.headers.get('retry-after')) > 0);
  });
});
