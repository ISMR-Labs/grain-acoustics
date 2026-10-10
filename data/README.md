# Data

## Conventions

- One folder per recording session, named `YYYYMMDD_HHMMSS[_name]` in server local time
  (Asia/Colombo).
- `meta.json` is authoritative for rates, formats, duration, chunk count and zero-filled gaps.
- Mic WAVs: int16 PCM, 16 kHz. Sample = INMP441 24-bit value >> 14 (bits 29..14 of the 32-bit
  I2S slot). 0 dBFS = 32768.
- Piezo WAVs: int16, 8 kHz. Sample = raw 12-bit ADC count − 2048, 0 dB attenuation
  (0 to 1.1 V), 0.55 V nominal bias. Resting value is around −330 (bias measured lower than
  nominal). ±2048 is the ADC rail. 1 count ≈ 0.27 mV at the pin, ×11 at the disc because of the
  100 kΩ / 1 MΩ divider.
- Per-channel mono files `mic_A.wav`, `mic_B.wav`, `piezo_1.wav`, `piezo_2.wav` are exports of
  the 2-channel files and carry no extra information.

## Sessions

### `recordings/` (final wiring, 10 Oct 2026)

| Session | Duration | Contents | Notes |
|---|---|---|---|
| `20261010_141142` | 23.1 s | all channels | Handling, taps and speech near the sensors; used for the overview figure |
| `20261010_141329` | 1004.9 s | `meta.json` only here | Quiet room with the node on a table. The four per-channel WAVs (93 MB) are on the GitHub release `data-2026-10-10` |
| `20261010_150038` | 8.2 s | all channels | Quiet check; piezo hum and clipping measured here |

### `sessions-2026-09-25/` (shared I2S bus, Mic B dead)

`20260925_202123_mics.wav`, `20260925_202301_mics.wav`, `20260925_204053_mics.wav`: 2-channel
16 kHz. Channel B is all zeros; channel A is valid. Kept as the "before" case for the bus fix.
Piezo files from this period were not kept.

### `sessions-2026-10-10-prelim/` (bench clips during the bus change)

`20261010_134228_mics.wav`, `20261010_140514_mics.wav`, `20261010_140633_mics.wav`,
`20261010_140859_mics.wav` and `20261010_140859_piezo.wav`: intermediate firmware builds while
Mic B was being brought up. Use with care; wiring and firmware were changing between clips.

## Insect recordings

None yet. The planned protocol is in README section 9.

## Licence

Data: CC BY 4.0.
