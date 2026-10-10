// The Oracle - voice assistant node.
// INMP441 mic -> server (VAD, Whisper, Claude, Piper) -> UDA1334A DAC -> amp -> speaker.
// Wi-Fi via WiFiManager captive portal (AP "TheOracle-Setup"); server URL is set in the same portal.
//   Mic : VDD 3V3, GND, SCK GPIO26, WS GPIO25, SD GPIO33, L/R GND
//   DAC : VIN 3V3, GND, BCLK GPIO14, WSEL GPIO27, DIN GPIO23, LOUT->amp IN+, AGND->amp IN-
//   LED : GPIO4 -> 330R -> LED -> GND      Button: BOOT (GPIO0) = push-to-talk; hold at boot 3 s = reset Wi-Fi
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)

#define AP_NAME       "TheOracle-Setup"
#define DEFAULT_SERVER "http://192.168.1.8:8003"
#define DEVICE_ID     "oracle-1"
#define PIN_MIC_SCK 26
#define PIN_MIC_WS  25
#define PIN_MIC_SD  33
#define PIN_DAC_BCLK 14
#define PIN_DAC_WSEL 27
#define PIN_DAC_DIN  23
#define PIN_LED 4
#define PIN_MUTE 21       // -> DAC MUTE pin: LOW = muted on the CJMCU-1334, HIGH = sound; optional
#define PIN_BTN 0
#define MIC_RATE   16000
#define MIC_SHIFT  14
#define PLAY_RATE  22050
#define CHUNK_MS   100
#define QUEUE_DEPTH 10
#define MIC_FRAMES (MIC_RATE * CHUNK_MS / 1000)

struct __attribute__((packed)) Header { char magic[4]; uint32_t seq, uptime_ms, rate, frames, flags; };
struct __attribute__((packed)) Chunk { Header h; int16_t mic[MIC_FRAMES]; };

static char server[96] = DEFAULT_SERVER;
static Preferences prefs;
static QueueHandle_t freeQ, fullQ;
static i2s_chan_handle_t rx = nullptr, tx = nullptr;
static int32_t i2sBuf[MIC_FRAMES * 2];
static volatile uint32_t stSeq = 0, stDrop = 0, stErr = 0, stOk = 0, stBytes = 0;
static volatile bool serverOk = false, playing = false, stopReq = false;
static volatile uint32_t stStallMs = 0, stStalls = 0, stPlayBytes = 0;
static volatile int ledMode = 0;   // 0 auto, 1 on, 2 off, 3 blink (server-driven)
static char playId[64] = "";
static volatile float playVol = 0.7f;
static SemaphoreHandle_t playSem;

// ---------------- I2S ----------------
static void setupI2S() {
  i2s_chan_config_t rc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER); rc.dma_desc_num = 8; rc.dma_frame_num = 512;
  ESP_ERROR_CHECK(i2s_new_channel(&rc, NULL, &rx));
  i2s_std_config_t rs = { .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)PIN_MIC_SCK, .ws = (gpio_num_t)PIN_MIC_WS, .dout = I2S_GPIO_UNUSED, .din = (gpio_num_t)PIN_MIC_SD, .invert_flags = {false, false, false} } };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &rs)); ESP_ERROR_CHECK(i2s_channel_enable(rx));
  i2s_chan_config_t tc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER); tc.dma_desc_num = 16; tc.dma_frame_num = 512; tc.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&tc, &tx, NULL));
  i2s_std_config_t ts = { .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PLAY_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)PIN_DAC_BCLK, .ws = (gpio_num_t)PIN_DAC_WSEL, .dout = (gpio_num_t)PIN_DAC_DIN, .din = I2S_GPIO_UNUSED, .invert_flags = {false, false, false} } };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &ts)); ESP_ERROR_CHECK(i2s_channel_enable(tx));
}
static void dacWrite(const int16_t *mono, int n, float vol) {
  static int16_t st[1024];
  for (int off = 0; off < n; off += 512) { int k = min(512, n - off);
    for (int i = 0; i < k; i++) { int16_t v = (int16_t)(mono[off + i] * vol); st[2 * i] = v; st[2 * i + 1] = v; }
    size_t w; i2s_channel_write(tx, st, k * 4, &w, portMAX_DELAY); }
}

// ---------------- playback from server ----------------
static WiFiClient *newClient() { if (strncmp(server, "https", 5) == 0) { auto *s = new WiFiClientSecure(); s->setInsecure(); return s; } return new WiFiClient(); }
static void playFromServer(const char *id, float vol) {
  WiFiClient *client = newClient(); HTTPClient http; http.setTimeout(8000);
  String url = String(server) + "/api/sounds/" + id + "/pcm"; http.begin(*client, url);
  int code = http.GET();
  if (code == 200) {
    WiFiClient *s = http.getStreamPtr(); static uint8_t buf[8192]; size_t have = 0; bool primed = false; int remaining = http.getSize(); uint32_t idle = millis();
    while (http.connected() && !stopReq && (remaining > 0 || remaining == -1)) {
      size_t avail = s->available();
      if (!avail) { if (millis() - idle > 8000) break; if (primed) { stStallMs += 2; if (stStallMs % 100 == 0) stStalls++; } delay(2); continue; } idle = millis();
      size_t n = s->read(buf + have, min(avail, sizeof(buf) - have)); have += n; if (remaining > 0) remaining -= n;
      if (!primed && have < sizeof(buf) && remaining != 0 && millis() - idle < 400) continue; primed = true;
      stPlayBytes += n; size_t frames = have / 2; dacWrite((int16_t *)buf, frames, vol); have -= frames * 2; if (have) buf[0] = buf[frames * 2];
      if (remaining == 0) break; }
  } else Serial.printf("[play] GET %s -> %d\n", url.c_str(), code);
  http.end(); delete client;
}
static void playTone(float vol) {   // 3 s of clean 440 Hz generated on the device: if THIS is distorted, the problem is after the ESP32
  static int16_t buf[512]; float ph = 0, step = 2 * PI * 440 / PLAY_RATE;
  for (int n = 0; n < PLAY_RATE * 3 / 512 && !stopReq; n++) { for (int i = 0; i < 512; i++) { buf[i] = (int16_t)(sinf(ph) * 16000); ph += step; if (ph > 2 * PI) ph -= 2 * PI; } dacWrite(buf, 512, vol); }
}
static void playTask(void *) {
  static int16_t silence[256] = {0};
  for (;;) { xSemaphoreTake(playSem, portMAX_DELAY); char id[64]; strncpy(id, playId, sizeof(id)); playId[0] = 0; float vol = playVol; if (!id[0]) continue;
    stopReq = false; playing = true; Serial.printf("[play] %s\n", id); if (!strcmp(id, "*tone*")) playTone(vol); else playFromServer(id, vol); dacWrite(silence, 256, 0); playing = false; stopReq = false; }
}

// ---------------- mic ----------------
static void acqTask(void *) {
  uint32_t seq = 0; size_t got;
  for (int i = 0; i < 4; i++) i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, portMAX_DELAY);
  for (;;) { Chunk *c = nullptr; xQueueReceive(freeQ, &c, 0);
    esp_err_t r = i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, pdMS_TO_TICKS(500)); if (!c) { stDrop++; continue; }
    memcpy(c->h.magic, "AUD1", 4); c->h.seq = seq++; c->h.uptime_ms = millis(); c->h.rate = MIC_RATE; c->h.frames = MIC_FRAMES;
    c->h.flags = (r == ESP_OK ? 1 : 0) | (playing ? 2 : 0) | (digitalRead(PIN_BTN) == LOW ? 4 : 0);
    if (playing) c->h.frames = 0;   // speaker busy: mic is ignored server-side anyway, keep the link free
    else if (r == ESP_OK) for (int i = 0; i < MIC_FRAMES; i++) c->mic[i] = (int16_t)(i2sBuf[2 * i] >> MIC_SHIFT); else memset(c->mic, 0, sizeof(c->mic));
    stSeq = seq; xQueueSend(fullQ, &c, 0); }
}
// server reply: {"play":"id","vol":0.7} | {"stop":true} | {"led":"on|off|blink|auto"} | {}
static void handleCommand(const String &b) {
  int p;
  if ((p = b.indexOf("\"led\":\"")) >= 0) { String m = b.substring(p + 7, b.indexOf('"', p + 7)); ledMode = m == "on" ? 1 : m == "off" ? 2 : m == "blink" ? 3 : 0; }
  if (b.indexOf("\"stop\"") >= 0) { stopReq = true; }
  if ((p = b.indexOf("\"vol\":")) >= 0) playVol = constrain(b.substring(p + 6).toFloat(), 0.0f, 1.0f);
  if ((p = b.indexOf("\"play\":\"")) >= 0) { int e = b.indexOf('"', p + 8); if (e > p) { stopReq = true; b.substring(p + 8, e).toCharArray(playId, sizeof(playId)); xSemaphoreGive(playSem); } }
}
static void senderTask(void *) {
  WiFiClient *client = newClient(); HTTPClient http; http.setReuse(true); http.setTimeout(3000); http.setConnectTimeout(3000);
  for (;;) { Chunk *c; xQueueReceive(fullQ, &c, portMAX_DELAY);
    if (WiFi.status() == WL_CONNECTED) {
      http.begin(*client, String(server) + "/api/ingest"); http.addHeader("Content-Type", "application/octet-stream"); http.addHeader("X-Device", DEVICE_ID); http.addHeader("X-Stats", String("rssi=") + WiFi.RSSI() + ",stall_ms=" + stStallMs + ",drop=" + stDrop + ",heap=" + ESP.getFreeHeap());
      size_t len = sizeof(Header) + c->h.frames * 2; int code = http.POST((uint8_t *)c, len);
      if (code == 200) { stOk++; stBytes += len; serverOk = true; String b = http.getString(); if (b.length() > 2) handleCommand(b); }
      else { stErr++; serverOk = false; client->stop(); }
      http.end();
    } else { stErr++; serverOk = false; }
    xQueueSend(freeQ, &c, 0); }
}

// ---------------- Wi-Fi setup portal ----------------
static void setupWiFi() {
  prefs.begin("oracle", false);
  String saved = prefs.getString("server", DEFAULT_SERVER); saved.toCharArray(server, sizeof(server));
  // hold BOOT for 3 s after power-up to forget Wi-Fi and reopen the portal
  uint32_t t0 = millis(); while (digitalRead(PIN_BTN) == LOW && millis() - t0 < 3000) { digitalWrite(PIN_LED, (millis() / 100) % 2); delay(10); }
  WiFiManager wm; wm.setDebugOutput(false);
  if (digitalRead(PIN_BTN) == LOW) { Serial.println("[wifi] reset requested"); wm.resetSettings(); }
  WiFiManagerParameter pServer("server", "Oracle server URL", server, sizeof(server) - 1);
  wm.addParameter(&pServer);
  wm.setSaveParamsCallback([&]() { prefs.putString("server", pServer.getValue()); });
  wm.setConfigPortalTimeout(300); wm.setConnectTimeout(20); wm.setTitle("The Oracle");
  WiFi.setSleep(false);
  Serial.printf("[wifi] connecting (portal AP: %s)\n", AP_NAME); digitalWrite(PIN_LED, HIGH);
  if (!wm.autoConnect(AP_NAME)) { Serial.println("[wifi] no connection, rebooting"); delay(1000); ESP.restart(); }
  prefs.putString("server", pServer.getValue()); String s = prefs.getString("server", DEFAULT_SERVER); s.toCharArray(server, sizeof(server));
  WiFi.setAutoReconnect(true); WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.printf("[wifi] connected %s rssi=%d server=%s\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), server);
  ArduinoOTA.setHostname(DEVICE_ID); ArduinoOTA.setPassword(OTA_PASS); ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200); pinMode(PIN_LED, OUTPUT); pinMode(PIN_MUTE, INPUT); /* mute control disabled */ pinMode(PIN_BTN, INPUT_PULLUP); delay(200);
  Serial.println("\n[oracle] boot");
  freeQ = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk *)); fullQ = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk *));
  for (int i = 0; i < QUEUE_DEPTH; i++) { Chunk *c = (Chunk *)malloc(sizeof(Chunk)); if (!c) break; xQueueSend(freeQ, &c, 0); }
  playSem = xSemaphoreCreateBinary();
  setupWiFi();
  setupI2S();
  xTaskCreatePinnedToCore(acqTask, "acq", 4096, nullptr, 20, nullptr, 1);
  xTaskCreatePinnedToCore(senderTask, "send", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(playTask, "play", 8192, nullptr, 10, nullptr, 1);
}

void loop() {
  static uint32_t lastStat = 0, lastBytes = 0, downSince = 0;
  ArduinoOTA.handle();
  uint32_t now = millis(); bool wifi = WiFi.status() == WL_CONNECTED;
  bool led;
  if (!wifi) led = (now / 500) % 2; else if (!serverOk) led = (now / 100) % 2;
  else led = ledMode == 1 ? true : ledMode == 2 ? false : ledMode == 3 ? (now / 250) % 2 : (playing || digitalRead(PIN_BTN) == LOW);
  digitalWrite(PIN_LED, led);
  if (wifi) downSince = 0; else if (!downSince) downSince = now; else if (now - downSince > 15000) { WiFi.reconnect(); downSince = now; }
  if (now - lastStat >= 2000) { float kbps = (stBytes - lastBytes) / (float)(now - lastStat); lastBytes = stBytes; lastStat = now;
    Serial.printf("[oracle] wifi=%s rssi=%d seq=%u ok=%u err=%u drop=%u playing=%d %.1f kB/s heap=%u\n", wifi ? "up" : "down", wifi ? WiFi.RSSI() : 0, stSeq, stOk, stErr, stDrop, (int)playing, kbps, (unsigned)ESP.getFreeHeap()); }
  delay(20);
}
