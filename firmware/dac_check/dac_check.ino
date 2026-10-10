// DAC check: the exact known-good music path (ESP_I2S library) + Wi-Fi + OTA, nothing else.
// Plays Ode to Joy forever on BCLK 14 / WSEL 27 / DIN 23 at 22050 Hz. If this is silent, the DAC or its wiring is at fault.
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESP_I2S.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)
I2SClass dac;
struct Note { uint8_t midi; uint8_t len; };
const Note tune[] = {{64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{64,3},{62,1},{62,4},{0,4}};
static float midiHz(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }
void setup() {
  Serial.begin(115200); pinMode(4, OUTPUT); digitalWrite(4, HIGH);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(100);
  ArduinoOTA.setHostname("oracle-1"); ArduinoOTA.setPassword(OTA_PASS); ArduinoOTA.begin();
  Serial.printf("[dac_check] wifi=%s ip=%s\n", WiFi.status() == WL_CONNECTED ? "up" : "down", WiFi.localIP().toString().c_str());
  dac.setPins(14, 27, 23, -1);
  dac.begin(I2S_MODE_STD, 22050, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}
void playNote(float hz, float seconds) {
  const int total = (int)(seconds * 22050); static int16_t buf[512]; float phase = 0, step = 2 * PI * hz / 22050;
  for (int done = 0; done < total; ) {
    int frames = min(256, total - done);
    for (int i = 0; i < frames; i++, done++) {
      float tt = (float)done / 22050, env = min(1.0f, tt * 60.0f) * expf(-tt * 1.5f);
      float s = hz > 0 ? (sinf(phase) + 0.4f * sinf(2 * phase)) : 0; phase += step; if (phase > 2 * PI) phase -= 2 * PI;
      int16_t v = (int16_t)(s * env * 0.35f * 20000.0f); buf[2 * i] = v; buf[2 * i + 1] = v;
    }
    dac.write((uint8_t *)buf, frames * 4); ArduinoOTA.handle();
  }
}
void loop() { for (auto &n : tune) playNote(n.midi ? midiHz(n.midi) : 0, n.len * 0.2f); }
