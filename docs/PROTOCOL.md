# Device protocol, version 1

How a display (a real ESP32 board or the browser emulator) talks to the Morning Newspaper platform. All paths are under the platform's API base, for example `https://newspaper.brushknight.com`. All bodies are JSON unless stated. Times are RFC 3339 UTC.

A display is a dumb reader. It never lays out a page. The platform sends it finished frames in the panel's own pixel format.

## 1. Pairing (adoption)

A new display has no credentials. It asks for a pairing code, shows it, and waits for a signed-in person (or their assistant over MCP) to adopt it. This is the OAuth device flow in spirit, ending in a plain device token.

### 1.1 Start

`POST /api/v1/devices/pair/start` (no auth)

```json
{ "model": "inky-impression-13.3-spectra6", "hardware_id": "esp32-a1b2c3d4e5f6", "firmware": "0.1.0" }
```

`model` is one of the model ids in section 4. `hardware_id` is stable per device (chip MAC based) and is only a label. Answer `200`:

```json
{
  "device_code": "64+ random url-safe characters, secret, never shown",
  "user_code": "K7QF-3MXD",
  "verification_uri": "https://newspaper.brushknight.com/app/pair",
  "verification_uri_complete": "https://newspaper.brushknight.com/app/pair?code=K7QF-3MXD",
  "expires_in": 900,
  "interval": 5
}
```

`user_code` is 8 characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no 0, O, 1, I), shown as two groups of four. The display shows a QR code of `verification_uri_complete` and the `user_code` as text under it.

### 1.2 Poll

`POST /api/v1/devices/pair/poll` (no auth), body `{ "device_code": "..." }`, no more often than `interval` seconds.

- `200 { "status": "pending" }` while nobody has adopted it.
- `200 { "status": "adopted", "device_id": "uuid", "name": "Kitchen", "token": "newspaper_dev_..." }` exactly once. The token is shown only in this answer. The display stores it and stops polling.
- `410 { "status": "expired" }` when the code ran out or the token was already handed over. The display starts again from 1.1.
- `404` for an unknown `device_code`.

### 1.3 Adopt (the person's side, not called by the display)

- Web: `POST /api/v1/devices/adopt` with the user's session, body `{ "user_code": "K7QF-3MXD", "name": "Kitchen" }`. The page `/app/pair?code=...` does this.
- MCP: tool `adopt_device` with `code` and optional `name`. Tools `list_devices`, `rename_device`, `remove_device` manage them.

`user_code` is accepted with or without the dash and in any letter case.

## 2. Device API

Every call carries `Authorization: Bearer newspaper_dev_...`. A `401` means the display was removed in the cloud: it forgets its token and goes back to pairing.

Optional request headers on every call, used for the device list in the web app: `X-Device-Firmware: 0.1.0`, `X-Device-Battery: 87` (percent, omit on mains), `X-Device-Rssi: -61`.

### 2.1 State

`GET /api/v1/device/state`

```json
{
  "device": { "id": "uuid", "name": "Kitchen", "model": "inky-impression-13.3-spectra6" },
  "server_time": "2026-11-11T06:40:00Z",
  "config": {
    "check_interval_minutes": 30,
    "page_interval_minutes": 10,
    "timezone": "Europe/Madrid",
    "quiet_hours": null
  },
  "issue": {
    "date": "2026-11-11",
    "number": 12,
    "version": "opaque string, changes whenever any page changes",
    "pages": [ { "index": 1, "id": "front", "title": "Front Page" } ]
  }
}
```

`issue` is `null` when the account has no published issue. The display compares `issue.version` with the one it last drew to decide whether anything changed. `check_interval_minutes` is how often to call this; `page_interval_minutes` is how often to turn to the next page by itself (0 means stay on the page).

### 2.2 Frame

`GET /api/v1/device/frame?page=1` (page is 1-based; add `&version=...` from state so caches are exact)

Answers `200` with `Content-Type: application/octet-stream`, an `ETag`, and a body in the frame format of the device's model (section 4). `404` when there is no such page, `503` with `Retry-After` when the frame is not rendered yet. The display must stream the body to the panel in chunks; it must not need the whole frame in memory.

`GET /api/v1/device/frame?page=1&format=png` returns the same picture as a PNG at the panel's size, for emulators and debugging.

## 3. Behaviour expected of a display

1. No Wi-Fi credentials: provisioning (target specific).
2. No token: pair (section 1), showing the QR code and user code.
3. With a token: call state. If there is an issue and its version differs from the one drawn, fetch the frame of the current page and draw it.
4. Buttons: Next, Back, Home (page 1). A press fetches and draws that page. Presses during a refresh are ignored.
5. Timer: every `page_interval_minutes` turn to the next page (wrapping to 1); every `check_interval_minutes` call state.
6. Never start a refresh sooner than the panel's minimum refresh interval after the last one, and never interrupt one.
7. Between activities, sleep as deeply as the target allows.

## 4. Models and frame formats

### `inky-impression-13.3-spectra6`

Pimoroni Inky Impression 13.3" and equivalents (Waveshare 13.3inch e-Paper HAT+ (E), GoodDisplay GDEP133C02). Panel EL133UF1: 1200 x 1600 pixels portrait, six inks, two controllers each driving one 600 x 1600 half.

Frame format `el133uf1`: exactly 960,000 bytes.

- Bytes `0 .. 479,999`: the left half (columns 0 to 599), for the first controller.
- Bytes `480,000 .. 959,999`: the right half (columns 600 to 1199), for the second controller.
- Each half is 1600 rows of 300 bytes, top row first. Each byte holds two pixels, left pixel in the high 4 bits.
- Pixel codes: `0` black, `1` white, `2` yellow, `3` red, `5` blue, `6` green. Other values are not used.

The firmware's panel driver owns any rotation or mirroring needed between this logical layout and the order the controllers want; the platform always sends this layout.

Panel limits the firmware must respect: a full refresh takes about 12 to 20 seconds and cannot be interrupted; at least 180 seconds between refreshes; no partial refresh.

### `emulator`

The browser emulator registers with model `inky-impression-13.3-spectra6` too, so it receives byte-identical frames. It adds `"emulator": true` to the pairing start body, which the platform only records and shows.
