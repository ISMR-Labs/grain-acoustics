// Noise isolation test: DAC outputs pure silence, Wi-Fi on for 20 s / off for 20 s, LED shows Wi-Fi state.
// Static that comes and goes with the LED = Wi-Fi RF/power noise reaching the amp. Static all the time = wiring/amp.
#include <WiFi.h>
#include <ESP_I2S.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)
I2SClass dac;
void setup() {
  Serial.begin(115200); pinMode(4, OUTPUT);
  dac.setPins(14, 27, 23, -1);
  dac.begin(I2S_MODE_STD, 22050, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  static int16_t z[512] = {0}; for (int i = 0; i < 20; i++) dac.write((uint8_t *)z, sizeof(z));
}
void loop() {
  Serial.println("[test] Wi-Fi ON (LED on) for 20 s"); digitalWrite(4, HIGH);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis(); while (millis() - t < 20000) { static int16_t z[512] = {0}; dac.write((uint8_t *)z, sizeof(z)); }
  Serial.println("[test] Wi-Fi OFF (LED off) for 20 s"); digitalWrite(4, LOW);
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  t = millis(); while (millis() - t < 20000) { static int16_t z[512] = {0}; dac.write((uint8_t *)z, sizeof(z)); }
}
