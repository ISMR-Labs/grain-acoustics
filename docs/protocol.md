# Wire protocol and API

## SNS1 chunk (sensor node → server)

One HTTP `POST /api/ingest` per 100 ms, `Content-Type: application/octet-stream`, keep-alive.
Optional header `X-Token` when the server has `INGEST_TOKEN` set.

Header, little-endian, 32 bytes:

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | char[4] | magic | `"SNS1"` |
| 4 | u32 | seq | chunk counter since boot |
| 8 | u32 | uptime_ms | device uptime at chunk end |
| 12 | u32 | mic_rate | 16000 |
| 16 | u32 | piezo_rate | 8000 |
| 20 | u32 | mic_frames | per channel (1600) |
| 24 | u32 | piezo_frames | per channel (about 800, varies with timer jitter) |
| 28 | u32 | flags | bit0 mic ok, bit1 piezo ok |

Payload: `int16[mic_frames × 2]` interleaved `[A, B]`, then `uint16[piezo_frames × 2]`
interleaved `[p1, p2]` holding raw 12-bit ADC counts.

Python struct for the header: `"<4sIIIIIII"`.

The server replies `200` with a small JSON object. For the sensor node it is `{}`; the companion
builds carry commands in it (below).

## AUD1 chunk (audio node and The Oracle → server)

Header `"<4sIIIII"`: magic `"AUD1"`, seq, uptime_ms, rate (16000), frames, flags
(bit0 mic ok, bit1 playing, bit2 button held). Payload int16 mono. While the device is playing it
sends header-only chunks (frames = 0) so the server still gets a heartbeat and can send commands.

Reply JSON may contain: `{"play": "<sound id>", "vol": 0.6}`, `{"stop": true}`,
`{"melody": true}`, `{"led": "on|off|blink|auto"}`. The device fetches
`GET /api/sounds/<id>/pcm` (22.05 kHz mono s16le) and streams it to the DAC.

Devices add `X-Device: <id>` and `X-Stats: rssi=..,stall_ms=..,drop=..,heap=..` headers.

## Sensors server REST + WebSocket

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/ingest` | SNS1 chunk |
| GET | `/api/status` | device, rate, lost chunks, uptime, recording state |
| POST | `/api/record/start` `{name}` | start a recording |
| POST | `/api/record/stop` | stop; writes per-channel files |
| GET | `/api/recordings` | list, with files and sizes |
| GET | `/api/recordings/{id}/{file}` | download `mics.wav`, `piezo.wav`, `mic_A.wav`, `mic_B.wav`, `piezo_1.wav`, `piezo_2.wav`, `meta.json` |
| DELETE | `/api/recordings/{id}` | delete |
| WS | `/ws` | binary SNS1 chunks for live view, JSON status every second |

## Recording folder

```
recordings/<YYYYMMDD_HHMMSS>[_name]/
  mics.wav      2 ch, 16 kHz, int16   [A, B]
  piezo.wav     2 ch, 8 kHz, int16    raw ADC − 2048  [p1, p2]
  mic_A.wav mic_B.wav piezo_1.wav piezo_2.wav   mono exports
  meta.json     started, ended, duration_s, rates, formats, chunks, gap_chunks_zero_filled, complete
```

Gaps (missing `seq` values) are zero-filled so the files stay on a continuous time axis; the
count is in `meta.json`.
