# Companion builds

Two more ESP32 builds share this repository because they reuse the same toolchain, the same
chunk-over-HTTP transport and the same server skeleton. They are not part of the measurement chain.

## Audio node

Hardware: ESP32 dev board, INMP441, UDA1334A I2S DAC (CJMCU-1334 breakout), mono class-D
amplifier with IN+/IN− only, 30 W speaker, status LED.

| ESP32 | Connects to |
|---|---|
| 3V3 | mic VDD; DAC VIN (5 V with a decoupling capacitor was quieter) |
| GND | mic GND, mic L/R, DAC GND, amp input ground |
| GPIO 26 / 25 / 33 | mic SCK / WS / SD |
| GPIO 14 / 27 / 23 | DAC BCLK / WSEL / DIN |
| GPIO 21 | DAC MUTE (optional) |
| DAC LOUT / AGND | amp IN+ / IN− (amp on its own supply, grounds tied) |
| GPIO 4 | LED |

Firmware `firmware/audio_node/audio_node.ino`, server `audio-server/` (port 8002). The mic
streams up as AUD1 chunks; the reply JSON carries play/stop/volume commands; the node streams
`GET /api/sounds/<id>/pcm` to the DAC with an 8 kB pre-fill. Uploads are converted with ffmpeg
and level-normalised; presets (beep, alarm, siren, chime, noise) are generated on first start.
`firmware/play_music/play_music.ino` is the standalone Ode-to-Joy test that first proved the DAC.

## The Oracle

An Alexa-style assistant on the audio-node hardware.

- **Device** `firmware/oracle_node/oracle_node.ino`: WiFiManager captive portal (AP
  `TheOracle-Setup`) that also asks for the server URL (stored in Preferences); hold BOOT 3 s
  at power-up to forget Wi-Fi; BOOT is push-to-talk; stall stats in `X-Stats`; OTA.
- **Server** `oracle-server/` (port 8003): per-device state machine (idle, listening, thinking,
  speaking, follow-up). Pipeline: WebRTC VAD (20 ms frames, 300 ms pre-roll, 700 ms end) →
  faster-whisper (`small`, int8, CPU) → fuzzy wake-word match (≥ 0.78) → either a music command
  (fuzzy match on the sound library) or Claude (`claude-opus-5`, low effort, tools: time,
  play/stop/list music, optional Home Assistant) → Piper TTS (`en_US-lessac-medium`) → play on
  the device. 6 s follow-up window after an answer. Console with orb state, live mic,
  conversation log, typed ask/say, music library upload, tone test.
- **Config** `oracle-server/.env.example`: `ANTHROPIC_API_KEY` (required for answers),
  `WAKE_WORDS`, `WHISPER_MODEL`, `PIPER_VOICE`, `HA_URL`/`HA_TOKEN`, `ASSISTANT_TZ`.
- **Test** `tools/simulate_oracle.py "Oracle, what time is it?"` feeds the server's own voice
  back as the mic input.

### Status

Pipeline verified end to end with the simulator. The physical DAC chain on the test board is
currently silent: the ESP32 outputs data (the GPIO 2 LED used as a DIN probe flickers) but the
DAC produces nothing even with the known-good test sketch, through earphones, with MUTE in either
state, on 3.3 V or 5 V. Suspects: DAC power/ground wiring or a failed DAC. The Oracle firmware's
DAC pins must be updated to the test board's current wiring (BCLK 1, WSEL 22, DIN 2) before
re-flashing. GPIO 3 (RX0) cannot be used for I2S while USB is attached because the USB-serial
chip drives it.

Bench sketches: `dac_check` (known-good music + OTA), `tone_test` (440 Hz on/off),
`noise_test`, `play_floor` (streams a library track).
