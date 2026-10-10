# Annotated references

## Why insects in grain can be heard

**Mankin et al. 2011, American Entomologist 57(1): 30–44.** A century-long review of insect
acoustic detection. Establishes that feeding and movement by hidden insects produce trains of
short broadband impulses, that the usable band in grain is roughly 1 to 8 kHz because grain
attenuates higher frequencies, and that background noise rather than sensor sensitivity is
usually the limiting factor. Motivates the two-channel design and the warehouse baseline stage.

**Mankin, Hagstrum, Guo, Eliopoulos, Njoroge 2021, Insects 12(3): 259.** Open-access review of
automated acoustic systems for stored-product insects, including low-cost microcontroller
sensors. Confirms that low-cost systems are viable when the sensor is well coupled and the
analysis uses impulse-train features rather than overall level.

**Hagstrum & Flinn 1993, J. Econ. Entomol. 86(4): 1271–1278.** Detection rates for several
stored-grain beetles across temperatures with probe sensors in grain. Shows adult activity is
strongly temperature dependent, so trials must log temperature.

**Shuman, Coffelt, Vick, Mankin 1993, J. Econ. Entomol. 86(3): 933–938.** Quantitative
detection of larvae feeding inside kernels; demonstrates that counts of acoustic events scale
with the number of larvae. Basis for the planned "events per minute" detector.

**Potamitis, Ganchev, Kontodimas 2009, J. Econ. Entomol. 102(4): 1681–1690.** Automatic
bioacoustic detection of *Sitophilus oryzae* (our main target) and the red palm weevil using
pattern recognition on recorded impulses. Reference for feature choice.

**Eliopoulos, Potamitis, Kontodimas, Givropoulou 2015, J. Econ. Entomol. 108(6): 2808–2814.**
Adult beetles detected inside a wheat mass from acoustic emissions with piezo probes; reports
detection distances within the grain, which sets expectations for how far from the pad an insect
can be.

**Fleurat-Lessard, Tomasini, Kostine, Fuzeau 2006, 9th IWCSPP.** Noise-spectrum processing
and classification of insect stage activity in grain bulks.

## Contact sensors and biotremology

**Gjonov, Lapeva-Gjonova, Pramatarova 2025, Biodiversity Data Journal 13: e143481.** A DIY piezo
contact microphone in a 3D-printed box, used to record substrate vibration from ants, beetles and
planthoppers. Open access with build drawings. This is the direct model for our piezo pad; the
difference is that they feed a digital recorder with a proper preamplifier, where we feed the
ESP32 ADC directly, which is why our noise floor is far higher.

**Hill & Wessel 2016, Current Biology 26(5): R187–R191.** Primer on biotremology: vibrational
signals travel in solids at 50 to 5000 Hz, which is why a contact sensor coupled to the bag or
grain is expected to outperform an airborne mic for the smallest insects.

## Components

- TDK InvenSense INMP441 datasheet: 61 dB SNR, −26 dBFS sensitivity, 60 Hz to 15 kHz flat band,
  24-bit I2S, L/R pin selects the stereo slot.
- Espressif ESP32 Technical Reference Manual: I2S (two peripherals), SAR ADC (ADC1 usable with
  Wi-Fi on, ADC2 is not), GPIO 34/35 input-only.
- ESP-IDF 5 driver docs: `i2s_std` and `adc_oneshot`; `adc_continuous` on the classic ESP32
  uses I2S0 DMA, which is why it cannot coexist with a second I2S RX channel.
- NXP UDA1334ATS datasheet (companion builds): PLL from BCLK, 3.3 V logic, MUTE pin.

## Software

FastAPI, uvicorn, websockets; numpy, scipy, matplotlib for analysis; faster-whisper, Piper,
webrtcvad and the Anthropic Python SDK for The Oracle; Arduino-ESP32 3.3.x; arduino-cli; OpenSCAD
through the lab's Forge print service; Nginx Proxy Manager; Docker Compose.
