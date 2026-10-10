// Pure test tone: 440 Hz, one second on, one second off, on BOTH channels. No network needed for the sound.
// DAC wiring: BCLK -> TX0 (GPIO 1), WSEL -> D22 (GPIO 22), DIN -> D2 (GPIO 2). GPIO 4 LED lights while the tone sounds.
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESP_I2S.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)
I2SClass dac;
static int16_t buf[1024];
void setup() {
  pinMode(4, OUTPUT);
  dac.setPins(1, 22, 2, -1);
  dac.begin(I2S_MODE_STD, 22050, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);   // only so OTA keeps working
  ArduinoOTA.setHostname("oracle-1"); ArduinoOTA.setPassword(OTA_PASS); ArduinoOTA.begin();
}
void loop() {
  static float ph = 0; const float step = 2 * PI * 440 / 22050;
  bool on = (millis() / 1000) % 2 == 0; digitalWrite(4, on);
  for (int i = 0; i < 512; i++) { int16_t v = on ? (int16_t)(sinf(ph) * 12000) : 0; ph += step; if (ph > 2 * PI) ph -= 2 * PI; buf[2 * i] = v; buf[2 * i + 1] = v; }
  dac.write((uint8_t *)buf, sizeof(buf)); ArduinoOTA.handle();
}
