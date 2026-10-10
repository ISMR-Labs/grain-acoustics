# Bill of materials: sensor node

| Qty | Part | Spec / notes | Alternatives |
|---|---|---|---|
| 1 | ESP32-WROOM-32U dev board | 38-pin, CP2102 USB-serial, u.FL antenna | ESP32-WROOM-32 (PCB antenna) if signal is strong; ESP32-S3 would allow two I2S plus DMA ADC |
| 1 | 2.4 GHz antenna with u.FL pigtail | 3 dBi | |
| 2 | INMP441 breakout | I2S MEMS mic, 3.3 V | ICS-43434, SPH0645 (different slot timing, firmware change) |
| 2 | Piezo disc 27 mm | brass, 2-wire | 35 mm disc for more low-frequency output; 20 mm for a probe tip |
| 2 | 100 kΩ 1/4 W | series protection | |
| 2 | 1 MΩ 1/4 W | load/bleed to bias | 470 kΩ (less hum), 4.7 MΩ (more LF, more hum) |
| 1 | 100 kΩ + 1 | 22 kΩ | bias divider 3V3 → 0.55 V | trimmer 10 kΩ for adjustable bias |
| 2 | BAT54S | dual Schottky clamp | 1N4148 pair |
| 1 | 100 nF ceramic | bias node decoupling | |
| 1 | LED 3 mm + 330 Ω | status | |
| 4 | 1 m braided 4-core cable, 4 mm | sensor leads | shielded 2-core for the piezos (recommended) |
| 1 | PLA+ filament, ~60 g | enclosures | |
| 4 | M3 × 8 screws | controller box lid | |
| 8 | M2 × 6 screws | mic pods | |
| 8 | M2.5 × 6 screws | piezo pads | |
| 1 | USB-C cable + 5 V 1 A supply | power | |

## Upgrade path: piezo charge amplifier (not yet built)

Per channel: LM324 (one of four op-amps), 1 nF feedback capacitor, 1 GΩ or 100 MΩ feedback
resistor (or 10 MΩ with reduced LF response), 1 MΩ input bias, output biased to 0.55 V,
300 Hz to 5 kHz band-pass on the output. Gain about 1000 relative to the bare disc into the
ADC. Powered from 3V3. Fits inside the controller box on a small proto board.
