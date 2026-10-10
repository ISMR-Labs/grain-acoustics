// Plays "On The Floor" on a loop. The song is streamed as raw PCM (22.05 kHz mono 16-bit) from the Oracle
// server's library and sent to the UDA1334A DAC. Wiring: BCLK -> TX0 (GPIO 1), WSEL -> D22 (GPIO 22), DIN -> D2 (GPIO 2).
// GPIO 1/3 are the USB serial pins, so there is no serial monitor with this wiring; updates go over Wi-Fi (OTA).
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ESP_I2S.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)
#define SONG_URL  "http://192.168.1.8:8003/api/sounds/snd-419f6a5e/pcm"   // On The Floor (22050 Hz mono s16le)
#define VOLUME    0.5f
#define PIN_LED   4
#define PIN_BCLK  1    // TX0
#define PIN_WSEL  22   // D22 (RX0 is driven by the USB-serial chip, so it cannot carry WSEL)
#define PIN_DIN   2    // D2

I2SClass dac;
static uint8_t netbuf[8192];
static int16_t stereo[1024];

void setup() {
  pinMode(PIN_LED, OUTPUT);   // no Serial: GPIO 1/3 are used by the DAC
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { digitalWrite(PIN_LED, (millis() / 300) % 2); delay(50); }
  ArduinoOTA.setHostname("oracle-1"); ArduinoOTA.setPassword(OTA_PASS); ArduinoOTA.begin();
  dac.setPins(PIN_BCLK, PIN_WSEL, PIN_DIN, -1);
  dac.begin(I2S_MODE_STD, 22050, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

void playSong() {
  WiFiClient client; HTTPClient http; http.setTimeout(8000);
  http.begin(client, SONG_URL);
  int code = http.GET();
  if (code != 200) { http.end(); delay(2000); return; }
  WiFiClient *s = http.getStreamPtr(); int remaining = http.getSize(); size_t have = 0; uint32_t idle = millis();
  while (http.connected() && (remaining > 0 || remaining == -1)) {
    ArduinoOTA.handle();
    size_t avail = s->available();
    if (!avail) { if (millis() - idle > 8000) break; delay(2); continue; }
    idle = millis();
    size_t n = s->read(netbuf + have, min(avail, sizeof(netbuf) - have)); have += n; if (remaining > 0) remaining -= n;
    if (have < sizeof(netbuf) && remaining != 0 && millis() - idle < 300) continue;   // small pre-fill against Wi-Fi jitter
    size_t frames = have / 2; int16_t *mono = (int16_t *)netbuf;
    for (size_t off = 0; off < frames; off += 512) {
      size_t k = min((size_t)512, frames - off);
      digitalWrite(PIN_LED, HIGH);
      for (size_t i = 0; i < k; i++) { int16_t v = (int16_t)(mono[off + i] * VOLUME); stereo[2 * i] = 0; stereo[2 * i + 1] = v; }   // RIGHT channel only (ROUT), left silent
      dac.write((uint8_t *)stereo, k * 4);
    }
    have -= frames * 2; if (have) netbuf[0] = netbuf[frames * 2];
    if (remaining == 0) break;
  }
  http.end(); digitalWrite(PIN_LED, LOW);
}

void loop() {
  playSong();
  for (int i = 0; i < 40; i++) { ArduinoOTA.handle(); delay(50); }   // 2 s pause, then again
}
