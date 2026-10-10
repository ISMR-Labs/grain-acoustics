# Experiment and build log

Dates are 2026, Asia/Colombo.

## 25 Sep: project start, first node

- Goal set: detect small insects inside sealed 5 kg rice bags in a warehouse.
- Sensor choice: 2 × INMP441 (airborne) + 2 × bare piezo discs (contact). Amplifier designs
  (LM324 charge amp, NJM386 monitor amp) discussed and parked; bare discs chosen for a first pass.
- Wiring v1: both mics on one I2S bus (A left slot, B right slot), piezos on ADC1 via DMA at
  10 kHz, 0.55 V bias, 10 MΩ bleed, BAT54S clamps.
- Firmware v1 and the sensors server written; node streams 100 ms chunks at ~104 kB/s.
- Wi-Fi association failed at −83 dBm with the module's trace antenna; external antenna fitted.
- First recordings (`data/sessions-2026-09-25/`): Mic A fine, Mic B all zeros.
- Enclosures designed: controller box v9, mic pod v2, piezo pad v3. Printed in PLA+.

## 26 Sep: diagnosing Mic B

- Swapped L/R ties, swapped modules, swapped pins. Mic B never produced data on the shared SD.
- With Mic B connected, Mic A showed 74 % zero samples and 640 ms periodic bit garbage.
  Disconnecting Mic B's SD made Mic A clean.
- Piezo recordings showed 50 Hz hum clipping (62–67 dB above the floor) and 2/4 kHz tones.
  Measured bias 0.26 V, later ~1745 counts; bleed resistor changed from 10 MΩ to 1 MΩ.

## 27 Sep: companion builds

- Second ESP32 with INMP441 + UDA1334A DAC + mono amp + 30 W speaker. Ode to Joy test plays.
- Audio node platform (mic up, sound library down) and The Oracle voice assistant written and
  deployed. Oracle pipeline verified end to end with the simulator (3.5 s per utterance on the
  server CPU). Music command "play ... on the floor" added. Physical DAC chain later went silent
  during noise troubleshooting and is still under investigation (see companion-builds.md).

## 10 Oct: Mic B on its own bus, piezos re-timed, per-channel export

- Mic B moved to I2S0 (SCK 14, WS 27, SD 32). Second `i2s_new_channel` first failed with
  "allocate DMA buffer failed"; fixed with 4 × 256 DMA per channel, queue depth 6, reading each
  mic in two 50 ms halves.
- DMA ADC could no longer run (needs I2S0). Piezos now sampled by a 1 MHz hardware timer ISR that
  notifies a priority-22 task doing two one-shot reads; 7987–8013 Hz achieved. `adc_oneshot_read_isr`
  does not exist in the installed IDF, hence the task.
- Mic B alive: level above 100 Hz matches Mic A within 1 dB (−20 dBFS speech test).
- Server: per-channel exports (`mic_A.wav` etc.) at stop, lazy export for old recordings,
  kbps in the status ticker, gap counter reset on device reboot.
- Recordings: `20261010_141142` (23 s, handling + speech), `20261010_141329` (17 min quiet
  room), `20261010_150038` (8 s check). Piezos show 30 dB of 50 Hz hum on the quiet check and
  clip 7–15 % of samples.
- Question raised: piezo sensitivity and whether hot glue to a plastic plate is an acceptable
  mount. Answer recorded in README section 9: use a thin stiff metal plate or probe with a thin
  hard bond; hot glue and plastic damp the band of interest; a charge amplifier is the real fix.

## Open items

- Shielded piezo leads, 470 kΩ load trial, LM324 charge amplifier.
- Server-side high-pass for display/export.
- Baseline and seeded-bag protocol (README section 9).
