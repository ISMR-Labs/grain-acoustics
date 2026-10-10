// Audio node: INMP441 mic -> HTTP stream to server; server -> sounds played on UDA1334A DAC -> amp -> speaker.
// Board: ESP32 Dev Module, Arduino-ESP32 core 3.x.
//   Mic : VDD 3V3, GND, SCK GPIO26, WS GPIO25, SD GPIO33, L/R GND
//   DAC : VIN 3V3, GND, BCLK GPIO14, WSEL GPIO27, DIN GPIO23, LOUT->amp IN+, AGND->amp IN-
//   LED : GPIO4 (slow blink = wifi, solid = streaming, fast = server error)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)
#define SERVER     "http://192.168.1.8:8002"          // LAN
//#define SERVER   "https://audio.dilanp.duckdns.org"   // from anywhere
#define DEVICE_ID  "audio-node-1"

#define PIN_MIC_SCK 26
#define PIN_MIC_WS  25
#define PIN_MIC_SD  33
#define PIN_DAC_BCLK 14
#define PIN_DAC_WSEL 27
#define PIN_DAC_DIN  23
#define PIN_LED 4
#define PIN_MUTE 21       // -> DAC MUTE pin (high = muted); optional

#define MIC_RATE   16000
#define MIC_SHIFT  14
#define PLAY_RATE  22050
#define CHUNK_MS   100
#define QUEUE_DEPTH 10
#define MIC_FRAMES (MIC_RATE * CHUNK_MS / 1000)   // 1600

struct __attribute__((packed)) Header { char magic[4]; uint32_t seq, uptime_ms, rate, frames, flags; };
struct __attribute__((packed)) Chunk { Header h; int16_t mic[MIC_FRAMES]; };

static QueueHandle_t freeQ, fullQ;
static i2s_chan_handle_t rx = nullptr, tx = nullptr;
static int32_t i2sBuf[MIC_FRAMES * 2];
static volatile uint32_t stSeq = 0, stDrop = 0, stErr = 0, stOk = 0, stBytes = 0;
static volatile bool serverOk = false, playing = false, stopReq = false;
static char playId[48] = "";           // pending sound id ("" = none, "*melody*" = built-in tune)
static volatile float playVol = 0.6f;
static SemaphoreHandle_t playSem;

// ---------------- I2S ----------------
static void setupI2S() {
  // Mic on I2S1 (stereo capture, keep left slot)
  i2s_chan_config_t rc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  rc.dma_desc_num = 8; rc.dma_frame_num = 512;
  ESP_ERROR_CHECK(i2s_new_channel(&rc, NULL, &rx));
  i2s_std_config_t rs = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)PIN_MIC_SCK, .ws = (gpio_num_t)PIN_MIC_WS,
                  .dout = I2S_GPIO_UNUSED, .din = (gpio_num_t)PIN_MIC_SD, .invert_flags = {false, false, false} },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &rs));
  ESP_ERROR_CHECK(i2s_channel_enable(rx));

  // DAC on I2S0 (16-bit stereo out)
  i2s_chan_config_t tc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  tc.dma_desc_num = 16; tc.dma_frame_num = 512; tc.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&tc, &tx, NULL));
  i2s_std_config_t ts = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PLAY_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)PIN_DAC_BCLK, .ws = (gpio_num_t)PIN_DAC_WSEL,
                  .dout = (gpio_num_t)PIN_DAC_DIN, .din = I2S_GPIO_UNUSED, .invert_flags = {false, false, false} },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &ts));
  ESP_ERROR_CHECK(i2s_channel_enable(tx));
}

// write mono int16 samples to the DAC (duplicated to both channels, volume applied)
static void dacWrite(const int16_t *mono, int n, float vol) {
  static int16_t st[1024];
  for (int off = 0; off < n; off += 512) {
    int k = min(512, n - off);
    for (int i = 0; i < k; i++) { int16_t v = (int16_t)(mono[off + i] * vol); st[2 * i] = v; st[2 * i + 1] = v; }
    size_t w; i2s_channel_write(tx, st, k * 4, &w, portMAX_DELAY);
  }
}

// ---------------- built-in melody (Ode to Joy) ----------------
struct Note { uint8_t midi; uint8_t len; };
static const Note tune[] = {
  {64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{64,3},{62,1},{62,4},
  {64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{62,3},{60,1},{60,4},
  {62,2},{62,2},{64,2},{60,2},{62,2},{64,1},{65,1},{64,2},{60,2},{62,2},{64,1},{65,1},{64,2},{62,2},{60,2},{62,2},{55,4},
  {64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{62,3},{60,1},{60,4},
};
static void playMelody(float vol) {
  static int16_t buf[512];
  for (auto &n : tune) {
    if (stopReq) return;
    float hz = n.midi ? 440.0f * powf(2.0f, (n.midi - 69) / 12.0f) : 0, phase = 0, step = 2 * PI * hz / PLAY_RATE;
    int total = (int)(n.len * 0.20f * PLAY_RATE);
    for (int done = 0; done < total && !stopReq; ) {
      int k = min(512, total - done);
      for (int i = 0; i < k; i++, done++) {
        float t = (float)done / PLAY_RATE, env = min(1.0f, t * 60.0f) * expf(-t * 1.5f);
        float rel = (float)(total - done) / PLAY_RATE; if (rel < 0.02f) env *= rel / 0.02f;
        float s = hz > 0 ? sinf(phase) + 0.4f * sinf(2 * phase) + 0.15f * sinf(3 * phase) : 0;
        phase += step; if (phase > 2 * PI) phase -= 2 * PI;
        buf[i] = (int16_t)(s * env * 12000.0f);
      }
      dacWrite(buf, k, vol);
    }
  }
}

// ---------------- stream a sound from the server ----------------
static void playFromServer(const char *id, float vol) {
  WiFiClient *client; const bool tls = strncmp(SERVER, "https", 5) == 0;
  if (tls) { auto *s = new WiFiClientSecure(); s->setInsecure(); client = s; } else client = new WiFiClient();
  HTTPClient http; http.setTimeout(8000);
  String url = String(SERVER) + "/api/sounds/" + id + "/pcm";
  http.begin(*client, url);
  int code = http.GET();
  if (code == 200) {
    WiFiClient *s = http.getStreamPtr();
    static uint8_t buf[8192]; size_t have = 0; bool primed = false;
    int remaining = http.getSize();  // -1 if chunked
    uint32_t idle = millis();
    while (http.connected() && !stopReq && (remaining > 0 || remaining == -1)) {
      size_t avail = s->available();
      if (!avail) { if (millis() - idle > 8000) break; delay(2); continue; }
      idle = millis();
      size_t n = s->read(buf + have, min(avail, sizeof(buf) - have)); have += n;
      if (remaining > 0) remaining -= n;
      if (!primed && have < sizeof(buf) && remaining != 0 && millis() - idle < 400) continue; primed = true;
      size_t frames = have / 2;
      dacWrite((int16_t *)buf, frames, vol);
      have -= frames * 2; if (have) buf[0] = buf[frames * 2];
      if (remaining == 0) break;
    }
  } else Serial.printf("[play] GET %s -> %d\n", url.c_str(), code);
  http.end(); delete client;
}

static void playTask(void *) {
  static int16_t silence[256] = {0};
  for (;;) {
    xSemaphoreTake(playSem, portMAX_DELAY);
    char id[48]; strncpy(id, playId, sizeof(id)); playId[0] = 0;
    float vol = playVol;
    if (!id[0]) continue;
    stopReq = false; playing = true; digitalWrite(PIN_MUTE, LOW);
    Serial.printf("[play] %s vol=%.2f\n", id, vol);
    if (!strcmp(id, "*melody*")) playMelody(vol); else playFromServer(id, vol);
    dacWrite(silence, 256, 0);
    digitalWrite(PIN_MUTE, HIGH); playing = false; stopReq = false;
    Serial.println("[play] done");
  }
}

// ---------------- mic acquisition ----------------
static void acqTask(void *) {
  uint32_t seq = 0; size_t got;
  for (int i = 0; i < 4; i++) i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, portMAX_DELAY);
  for (;;) {
    Chunk *c = nullptr; xQueueReceive(freeQ, &c, 0);
    esp_err_t r = i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, pdMS_TO_TICKS(500));
    if (!c) { stDrop++; continue; }
    memcpy(c->h.magic, "AUD1", 4);
    c->h.seq = seq++; c->h.uptime_ms = millis(); c->h.rate = MIC_RATE; c->h.frames = MIC_FRAMES;
    c->h.flags = (r == ESP_OK ? 1 : 0) | (playing ? 2 : 0);
    if (playing) c->h.frames = 0;   // speaker busy: mic is ignored server-side anyway, keep the link free
    else if (r == ESP_OK) for (int i = 0; i < MIC_FRAMES; i++) c->mic[i] = (int16_t)(i2sBuf[2 * i] >> MIC_SHIFT);   // left slot
    else memset(c->mic, 0, sizeof(c->mic));
    stSeq = seq; xQueueSend(fullQ, &c, 0);
  }
}

// parse the server's reply to a chunk: {"play":"id","vol":0.6} | {"melody":true} | {"stop":true} | {}
static void handleCommand(const String &body) {
  int p;
  if (body.indexOf("\"stop\"") >= 0) { stopReq = true; return; }
  if ((p = body.indexOf("\"vol\":")) >= 0) playVol = constrain(body.substring(p + 6).toFloat(), 0.0f, 1.0f);
  if (body.indexOf("\"melody\"") >= 0) { stopReq = true; strncpy(playId, "*melody*", sizeof(playId)); xSemaphoreGive(playSem); return; }
  if ((p = body.indexOf("\"play\":\"")) >= 0) {
    int e = body.indexOf('"', p + 8);
    if (e > p) { stopReq = true; body.substring(p + 8, e).toCharArray(playId, sizeof(playId)); xSemaphoreGive(playSem); }
  }
}

static void senderTask(void *) {
  const bool tls = strncmp(SERVER, "https", 5) == 0;
  WiFiClient *client; if (tls) { auto *s = new WiFiClientSecure(); s->setInsecure(); client = s; } else client = new WiFiClient();
  HTTPClient http; http.setReuse(true); http.setTimeout(3000); http.setConnectTimeout(3000);
  for (;;) {
    Chunk *c; xQueueReceive(fullQ, &c, portMAX_DELAY);
    if (WiFi.status() == WL_CONNECTED) {
      http.begin(*client, String(SERVER) + "/api/ingest");
      http.addHeader("Content-Type", "application/octet-stream");
      http.addHeader("X-Device", DEVICE_ID);
      size_t len = sizeof(Header) + c->h.frames * 2; int code = http.POST((uint8_t *)c, len);
      if (code == 200) { stOk++; stBytes += len; serverOk = true; String b = http.getString(); if (b.length() > 2) handleCommand(b); }
      else { stErr++; serverOk = false; client->stop(); }
      http.end();
    } else { stErr++; serverOk = false; }
    xQueueSend(freeQ, &c, 0);
  }
}

void setup() {
  Serial.begin(115200); pinMode(PIN_LED, OUTPUT); pinMode(PIN_MUTE, OUTPUT); digitalWrite(PIN_MUTE, HIGH); delay(200);
  Serial.println("\n[audio-node] boot");
  freeQ = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk *)); fullQ = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk *));
  for (int i = 0; i < QUEUE_DEPTH; i++) { Chunk *c = (Chunk *)malloc(sizeof(Chunk)); if (!c) break; xQueueSend(freeQ, &c, 0); }
  playSem = xSemaphoreCreateBinary();

  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) Serial.printf("[wifi] disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
    else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) { Serial.printf("[wifi] got ip %s\n", WiFi.localIP().toString().c_str()); WiFi.setTxPower(WIFI_POWER_19_5dBm); }
  });
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) Serial.printf("[wifi]   %-24s ch%-2d %4d dBm\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  WiFi.scanDelete();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  setupI2S();
  xTaskCreatePinnedToCore(acqTask, "acq", 4096, nullptr, 20, nullptr, 1);
  xTaskCreatePinnedToCore(senderTask, "send", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(playTask, "play", 8192, nullptr, 10, nullptr, 1);
}

void loop() {
  static uint32_t lastStat = 0, lastBytes = 0, downSince = 0;
  uint32_t now = millis(); bool wifi = WiFi.status() == WL_CONNECTED;
  if (!wifi) digitalWrite(PIN_LED, (now / 500) % 2); else if (!serverOk) digitalWrite(PIN_LED, (now / 100) % 2); else digitalWrite(PIN_LED, HIGH);
  if (wifi) downSince = 0; else if (!downSince) downSince = now;
  else if (now - downSince > 10000) { Serial.println("[wifi] forcing reconnect"); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); downSince = now; }
  if (now - lastStat >= 2000) {
    float kbps = (stBytes - lastBytes) / (float)(now - lastStat); lastBytes = stBytes; lastStat = now;
    Serial.printf("[audio-node] wifi=%s rssi=%d seq=%u ok=%u err=%u drop=%u playing=%d %.1f kB/s heap=%u\n",
                  wifi ? "up" : "down", wifi ? WiFi.RSSI() : 0, stSeq, stOk, stErr, stDrop, (int)playing, kbps, (unsigned)ESP.getFreeHeap());
  }
  delay(20);
}
