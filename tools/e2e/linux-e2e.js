#!/usr/bin/env node
// End to end check of the Linux program in no-hardware mode against the mock
// platform. No dependencies (Node 22 or newer).
//
//   node tools/e2e/linux-e2e.js --bin build-linux/newspaper [--out DIR] [--real-rest]
//
// The program pairs (the code it draws is the one the platform issued), is
// adopted through tools/mock-server/adopt.js, draws page 1 byte for byte as
// the platform sent it, turns to page 2 on a "Next" key press, stores its
// token in a file with mode 0600, stops cleanly on SIGTERM, and after a
// restart does not redraw a page that is already up. The simulated panel
// reports no rule violations.
//
// By default the simulated panel rests 3 s between refreshes so the check is
// quick. --real-rest keeps the panel's real 180 s and checks that the first
// page waits for it (about four minutes).
'use strict';

const { spawn, execFile } = require('node:child_process');
const { promisify } = require('node:util');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createMock } = require('../mock-server/server.js');
const { testPage } = require('../mock-server/frames.js');

const args = process.argv.slice(2);
const option = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const bin = path.resolve(option('--bin', 'build-linux/newspaper'));
const realRest = args.includes('--real-rest');
const restMs = realRest ? 180000 : 3000;
const work = option('--out', '') ? path.resolve(option('--out', '')) : fs.mkdtempSync(path.join(os.tmpdir(), 'np-linux-e2e-'));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
function check(ok, what) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
}

async function waitFor(fn, what, timeoutMs) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    const v = fn();
    if (v) return v;
    await sleep(100);
  }
  throw new Error(`timed out waiting for ${what}`);
}

function startProgram(base, dataDir, log) {
  const child = spawn(bin, [
    '--no-hardware', '--verbose',
    '--api', base,
    '--data-dir', dataDir,
    '--runtime-dir', path.join(work, 'run'),
    '--frames-dir', path.join(work, 'frames'),
    '--sim-refresh-ms', '1500',
    '--sim-rest-ms', String(restMs),
    '--hardware-id', 'linux-e2e',
    '--network-check', 'no',
  ], { stdio: ['pipe', 'pipe', 'pipe'] });
  child.stdout.on('data', (d) => log.push(d.toString()));
  child.stderr.on('data', (d) => log.push(d.toString()));
  child.exited = new Promise((resolve) => child.on('exit', (code, signal) => resolve({ code, signal })));
  return child;
}

const frameFile = (n) => path.join(work, 'frames', `frame-${String(n).padStart(4, '0')}.el133uf1`);
const frameCount = () => fs.readdirSync(path.join(work, 'frames')).filter((f) => f.endsWith('.el133uf1')).length;

async function main() {
  if (!fs.existsSync(bin)) throw new Error(`${bin} not found; build targets/linux first`);
  fs.rmSync(path.join(work, 'frames'), { recursive: true, force: true });
  fs.rmSync(path.join(work, 'data'), { recursive: true, force: true });
  fs.rmSync(path.join(work, 'run'), { recursive: true, force: true });
  fs.mkdirSync(path.join(work, 'frames'), { recursive: true });
  const dataDir = path.join(work, 'data');

  const mock = createMock({ port: 0, host: '127.0.0.1', publicUrl: '', pages: 3, expires: 900, interval: 1, checkMinutes: 30, pageMinutes: 0, renderDelay: 0, autoAdopt: -1, static: '' });
  await new Promise((resolve) => mock.server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${mock.server.address().port}`;
  const version = () => `mock-${mock.issue.date}-${mock.issue.serial}`;
  const pending = () => [...mock.pairings.values()].filter((p) => !p.adopted).map((p) => p.user_code).pop();
  const log = [];
  let child = startProgram(base, dataDir, log);

  try {
    const code = await waitFor(() => /pair: code (\S+),/.exec(log.join(''))?.[1], 'a pairing code', 20000);
    check(code === pending(), `program asked for a pairing code and got ${code}`);
    await waitFor(() => fs.existsSync(frameFile(1)), 'the pairing screen', 20000);
    check(/refresh started \(pair\)/.test(log.join('')), 'pairing screen drawn on the simulated panel');

    // Asynchronous: the mock platform runs in this process and must keep answering.
    const { stdout: adopt } = await promisify(execFile)(process.execPath, [path.join(__dirname, '../mock-server/adopt.js'), '--url', base, '--name', 'E2E'], { encoding: 'utf8', timeout: 20000 });
    check(adopt.includes(code), `adopted with adopt.js: ${adopt.trim()}`);
    const adoptedAt = Date.now();

    await waitFor(() => fs.existsSync(frameFile(2)), 'page 1', restMs + 30000);
    const page1 = fs.readFileSync(frameFile(2));
    check(Buffer.compare(page1, testPage(1, 3, `VERSION ${version()}`)) === 0, 'page 1 on the panel equals the platform frame byte for byte');
    check(fs.existsSync(path.join(work, 'frames', 'frame-0002.png')) && fs.existsSync(path.join(work, 'frames', 'latest.png')), 'page 1 also written as PNG');
    if (realRest) {
      const waited = Date.now() - adoptedAt;
      check(waited > 150000, `page 1 waited for the 180 s rest after the pairing screen (${Math.round(waited / 1000)} s after adoption)`);
    }

    const state = path.join(dataDir, 'state');
    const mode = fs.statSync(state).mode & 0o777;
    check(mode === 0o600, `token file ${state} has mode ${mode.toString(8)}`);
    check(/^token=.+/m.test(fs.readFileSync(state, 'utf8')), 'token stored');

    await waitFor(() => /refresh done after \d+ ms, showing frame/.test(log.join('')), 'the refresh to end', 10000);
    child.stdin.write('n\n');
    await waitFor(() => fs.existsSync(frameFile(3)), 'page 2 after Next', restMs + 20000);
    check(Buffer.compare(fs.readFileSync(frameFile(3)), testPage(2, 3, `VERSION ${version()}`)) === 0, 'Next draws page 2, byte for byte');
    await waitFor(() => (log.join('').match(/showing frame/g) || []).length >= 2, 'the second refresh to end', 10000);

    child.kill('SIGTERM');
    const exit = await child.exited;
    check(exit.code === 0, `stops cleanly on SIGTERM (exit ${exit.code}${exit.signal ? ` signal ${exit.signal}` : ''})`);

    const before = frameCount();
    child = startProgram(base, dataDir, log);
    await waitFor(() => /data in /.test(log.join('').split('stopping').pop()), 'the restart', 10000);
    await sleep(3000);
    check(frameCount() === before, 'after a restart the page already up is not drawn again');
    child.kill('SIGTERM');
    await child.exited;

    const violations = (log.join('').match(/RULE VIOLATION/g) || []).length;
    check(violations === 0, `simulated panel saw ${violations} rule violations`);
  } finally {
    if (child.exitCode === null) child.kill('SIGKILL');
    mock.server.close();
    fs.writeFileSync(path.join(work, 'program.log'), log.join(''));
    console.log(`log and frames in ${work}`);
  }
  if (failures) {
    console.log(`${failures} check(s) failed`);
    process.exit(1);
  }
  console.log('all checks passed');
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
