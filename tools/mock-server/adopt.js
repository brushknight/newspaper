#!/usr/bin/env node
// Adopt a display that is waiting on the mock platform.
//   node tools/mock-server/adopt.js [--url http://127.0.0.1:8787] [--name Kitchen] [CODE]
// Without CODE the newest pending pairing code is adopted.
'use strict';

async function main() {
  let url = 'http://127.0.0.1:8787';
  let name = 'Kitchen';
  let code = '';
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--url') url = args[++i];
    else if (args[i] === '--name') name = args[++i];
    else code = args[i];
  }
  const res = await fetch(`${url.replace(/\/$/, '')}/mock/adopt`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user_code: code || undefined, name }),
  });
  const body = await res.json();
  if (!res.ok) {
    console.error(`not adopted: ${body.error || res.status}`);
    process.exit(1);
  }
  console.log(`adopted ${body.user_code} as "${body.name}" (${body.device_id})`);
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
