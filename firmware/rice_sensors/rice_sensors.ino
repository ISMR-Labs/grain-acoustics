// Rice-bag sensor node: 2x INMP441 (I2S stereo) + 2x piezo (ADC1 DMA) -> HTTP stream
// Board: ESP32 Dev Module (ESP32-WROOM-32U). Arduino-ESP32 core 3.x.
//
// Wiring (see chat):
//   Mic A : VDD 3V3, GND, SCK GPIO26, WS GPIO25, SD GPIO33, L/R -> GND   (I2S1)
//   Mic B : VDD 3V3, GND, SCK GPIO14, WS GPIO27, SD GPIO32, L/R -> GND   (I2S0, its own bus)
//   Piezo 1 : GPIO34 (ADC1_CH6)   Piezo 2 : GPIO35 (ADC1_CH7)   (0.55 V bias, 0 dB atten, timer-sampled)
//   LED     : GPIO4 -> 330R -> LED -> GND
//
// LED: slow blink = connecting Wi-Fi, solid = streaming OK, fast blink = server errors.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include <ArduinoOTA.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ---------------- Configuration ----------------
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS (copy secrets.h.example)

// Use the LAN URL when the ESP32 is on the same network as the server (fastest).
// Use the HTTPS URL from anywhere else (goes through the reverse proxy).
#define SERVER_URL  "http://192.168.1.8:8001/api/ingest"
//#define SERVER_URL  "https://sensors.dilanp.duckdns.org/api/ingest"
#define INGEST_TOKEN ""          // leave "" unless INGEST_TOKEN is set on the server
#define DEVICE_ID   "rice-node-1"

#define PIN_I2S_SCK 26      // Mic A
#define PIN_I2S_WS  25
#define PIN_I2S_SD  33
#define PIN_I2SB_SCK 14     // Mic B (separate bus)
#define PIN_I2SB_WS  27
#define PIN_I2SB_SD  32
#define PIN_LED     4
#define PIEZO1_CH   ADC_CHANNEL_6   // GPIO34
#define PIEZO2_CH   ADC_CHANNEL_7   // GPIO35

#define MIC_RATE        16000       // Hz per mic (16-bit samples sent)
#define MIC_SHIFT       14          // 32-bit I2S slot -> int16 (24 valid bits, keep bits 29..14)
#define PIEZO_RATE      8000        // Hz per piezo, hardware-timer paced
#define CHUNK_MS        100         // one HTTP POST per chunk
#define QUEUE_DEPTH     6           // chunks buffered while Wi-Fi hiccups (~10 kB each)

// ---------------- Wire format ----------------
#define MIC_FRAMES        (MIC_RATE * CHUNK_MS / 1000)          // 1600
#define PIEZO_MAX_FRAMES  (PIEZO_RATE * CHUNK_MS / 1000 + 256)  // headroom for jitter

struct __attribute__((packed)) Header {
  char     magic[4];      // "SNS1"
  uint32_t seq;
  uint32_t uptime_ms;
  uint32_t mic_rate;
  uint32_t piezo_rate;
  uint32_t mic_frames;    // per channel
  uint32_t piezo_frames;  // per channel
  uint32_t flags;         // bit0 mic ok, bit1 piezo ok
};
struct __attribute__((packed)) Chunk {
  Header   h;
  int16_t  mic[MIC_FRAMES * 2];            // interleaved L(mic A), R(mic B)
  uint16_t piezo[PIEZO_MAX_FRAMES * 2];    // interleaved p1, p2 (raw 12-bit ADC)
};
static inline size_t chunkBytes(const Chunk *c) {
  return sizeof(Header) + sizeof(c->mic) + (size_t)c->h.piezo_frames * 2 * sizeof(uint16_t);
}

// ---------------- Globals ----------------
static QueueHandle_t freeQ, fullQ;
static i2s_chan_handle_t rx = nullptr, rxB = nullptr;
static adc_oneshot_unit_handle_t adc = nullptr;
#define HALF (MIC_FRAMES / 2)
static int32_t i2sBuf[HALF * 2], i2sBufB[HALF * 2];
#define PZ_RING 4096
static volatile uint16_t pzRing[2][PZ_RING];
static volatile uint32_t pzW = 0; static uint32_t pzR = 0;
static hw_timer_t *pzTimer = nullptr; static TaskHandle_t pzTask = nullptr;

static volatile uint32_t stSeq = 0, stDropNoBuf = 0, stPostErr = 0, stPostOk = 0, stBytes = 0;
static volatile uint32_t stAdcBad = 0;
static volatile bool serverOk = false;

// ---------------- I2S (mics) ----------------
static void setupI2S() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);  // I2S1: I2S0 is used by the ADC DMA on ESP32
  cc.dma_desc_num = 4;
  cc.dma_frame_num = 256;
  ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &rx));

  i2s_std_config_t sc = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_I2S_SCK,
      .ws   = (gpio_num_t)PIN_I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)PIN_I2S_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &sc));
  ESP_ERROR_CHECK(i2s_channel_enable(rx));

  // Mic B on its own bus (I2S0), same format; we keep the left slot
  i2s_chan_config_t ccB = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ccB.dma_desc_num = 4; ccB.dma_frame_num = 256;
  ESP_ERROR_CHECK(i2s_new_channel(&ccB, NULL, &rxB));
  i2s_std_config_t scB = sc;
  scB.gpio_cfg.bclk = (gpio_num_t)PIN_I2SB_SCK; scB.gpio_cfg.ws = (gpio_num_t)PIN_I2SB_WS; scB.gpio_cfg.din = (gpio_num_t)PIN_I2SB_SD;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rxB, &scB));
  ESP_ERROR_CHECK(i2s_channel_enable(rxB));
}

// ---------------- Piezos: hardware timer -> task -> two ADC reads, PIEZO_RATE times a second ----------------
static void IRAM_ATTR onPzTimer() { BaseType_t hp = pdFALSE; vTaskNotifyGiveFromISR(pzTask, &hp); if (hp) portYIELD_FROM_ISR(); }
static void pzSampleTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int v1 = 0, v2 = 0;
    adc_oneshot_read(adc, PIEZO1_CH, &v1); adc_oneshot_read(adc, PIEZO2_CH, &v2);
    uint32_t i = pzW % PZ_RING; pzRing[0][i] = v1; pzRing[1][i] = v2; pzW++;
  }
}
static void setupADC() {
  adc_oneshot_unit_init_cfg_t uc = {}; uc.unit_id = ADC_UNIT_1;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&uc, &adc));
  adc_oneshot_chan_cfg_t cc = {}; cc.atten = ADC_ATTEN_DB_0; cc.bitwidth = ADC_BITWIDTH_12;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, PIEZO1_CH, &cc));
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, PIEZO2_CH, &cc));
  xTaskCreatePinnedToCore(pzSampleTask, "piezo", 3072, nullptr, 22, &pzTask, 1);
  pzTimer = timerBegin(1000000);
  timerAttachInterrupt(pzTimer, &onPzTimer);
  timerAlarm(pzTimer, 1000000 / PIEZO_RATE, true, 0);
}

// Copy everything sampled since the last call into the chunk. Returns frames (pairs).
static uint32_t drainADC(Chunk *c) {
  uint32_t w = pzW; uint32_t n = w - pzR;
  if (n > PZ_RING) { stAdcBad += n - PZ_RING; pzR = w - PZ_RING; n = PZ_RING; }   // ring overrun
  if (n > PIEZO_MAX_FRAMES) n = PIEZO_MAX_FRAMES;
  for (uint32_t k = 0; k < n; k++) { uint32_t i = (pzR + k) % PZ_RING; c->piezo[2 * k] = pzRing[0][i]; c->piezo[2 * k + 1] = pzRing[1][i]; }
  pzR += n;
  return n;
}

// ---------------- Acquisition task ----------------
static void acqTask(void *) {
  uint32_t seq = 0;
  // Let the INMP441s settle and discard their start-up garbage.
  size_t got;
  for (int i = 0; i < 8; i++) { i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, portMAX_DELAY); i2s_channel_read(rxB, i2sBufB, sizeof(i2sBufB), &got, portMAX_DELAY); }
  pzR = pzW;

  for (;;) {
    Chunk *c = nullptr;
    xQueueReceive(freeQ, &c, 0);
    bool okA = true, okB = true;
    for (int h = 0; h < 2; h++) {   // two 50 ms halves: A then B, so neither DMA ring has to hold a whole chunk
      esp_err_t r  = i2s_channel_read(rx,  i2sBuf,  sizeof(i2sBuf),  &got, pdMS_TO_TICKS(300)); size_t gotB = 0;
      esp_err_t rB = i2s_channel_read(rxB, i2sBufB, sizeof(i2sBufB), &gotB, pdMS_TO_TICKS(300));
      okA &= (r == ESP_OK && got == sizeof(i2sBuf)); okB &= (rB == ESP_OK && gotB == sizeof(i2sBufB));
      if (c) for (int i = 0; i < HALF; i++) {
        c->mic[2 * (h * HALF + i)]     = okA ? (int16_t)(i2sBuf[2 * i]  >> MIC_SHIFT) : 0;   // Mic A
        c->mic[2 * (h * HALF + i) + 1] = okB ? (int16_t)(i2sBufB[2 * i] >> MIC_SHIFT) : 0;   // Mic B
      }
    }
    if (!c) { stDropNoBuf++; pzR = pzW; continue; }   // no free buffer: discard this chunk

    memcpy(c->h.magic, "SNS1", 4);
    c->h.seq = seq++;
    c->h.uptime_ms = millis();
    c->h.mic_rate = MIC_RATE;
    c->h.piezo_rate = PIEZO_RATE;
    c->h.mic_frames = MIC_FRAMES;
    c->h.flags = (okA || okB) ? 1 : 0;
    c->h.piezo_frames = drainADC(c);
    if (c->h.piezo_frames) c->h.flags |= 2;
    stSeq = seq;
    xQueueSend(fullQ, &c, 0);
  }
}

// ---------------- Sender task ----------------
static void senderTask(void *) {
  const bool tls = strncmp(SERVER_URL, "https", 5) == 0;
  WiFiClient *client;
  if (tls) { auto *s = new WiFiClientSecure(); s->setInsecure(); client = s; }
  else client = new WiFiClient();
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(3000);
  http.setConnectTimeout(3000);

  for (;;) {
    Chunk *c;
    xQueueReceive(fullQ, &c, portMAX_DELAY);
    if (WiFi.status() == WL_CONNECTED) {
      http.begin(*client, SERVER_URL);
      http.addHeader("Content-Type", "application/octet-stream");
      http.addHeader("X-Device", DEVICE_ID);
      if (strlen(INGEST_TOKEN)) http.addHeader("X-Token", INGEST_TOKEN);
      size_t len = chunkBytes(c);
      int code = http.POST((uint8_t *)c, len);
      http.end();
      if (code == 200 || code == 204) { stPostOk++; stBytes += len; serverOk = true; }
      else { stPostErr++; serverOk = false; client->stop(); }
    } else {
      stPostErr++; serverOk = false;
    }
    xQueueSend(freeQ, &c, 0);
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  delay(200);
  Serial.println("\n[rice-node] boot");

  freeQ = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk *));
  fullQ = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk *));
  for (int i = 0; i < QUEUE_DEPTH; i++) {
    Chunk *c = (Chunk *)malloc(sizeof(Chunk));
    if (!c) { Serial.printf("[rice-node] only %d chunk buffers fit in RAM\n", i); break; }
    xQueueSend(freeQ, &c, 0);
  }
  Serial.printf("[rice-node] chunk=%u bytes, %u ms, mic %u Hz x2, piezo %u Hz x2\n",
                (unsigned)sizeof(Chunk), CHUNK_MS, MIC_RATE, PIEZO_RATE);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // modem sleep kills throughput
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) Serial.printf("[wifi] disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
    else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) Serial.printf("[wifi] got ip %s\n", WiFi.localIP().toString().c_str());
    else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) Serial.println("[wifi] associated");
  });
  Serial.println("[wifi] scanning 2.4 GHz networks...");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++)
    Serial.printf("[wifi]   %-24s ch%-2d %4d dBm %s\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
  WiFi.scanDelete();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);   // max TX power: the router is far / signal is weak

  setupI2S();                    // Mic A on I2S1, Mic B on I2S0
  setupADC();                    // piezos: timer-paced ADC reads
  ArduinoOTA.setHostname("rice-node-1"); ArduinoOTA.setPassword(OTA_PASS); ArduinoOTA.begin();

  xTaskCreatePinnedToCore(acqTask,    "acq",  4096, nullptr, 20, nullptr, 1);
  xTaskCreatePinnedToCore(senderTask, "send", 8192, nullptr, 5,  nullptr, 0);
}

void loop() {
  ArduinoOTA.handle();
  static uint32_t lastStat = 0, lastBytes = 0;
  uint32_t now = millis();
  bool wifi = WiFi.status() == WL_CONNECTED;

  // LED pattern
  if (!wifi)          digitalWrite(PIN_LED, (now / 500) % 2);
  else if (!serverOk) digitalWrite(PIN_LED, (now / 100) % 2);
  else                digitalWrite(PIN_LED, HIGH);

  static uint32_t downSince = 0;
  if (wifi) downSince = 0;
  else if (!downSince) downSince = now;
  else if (now - downSince > 10000) { Serial.println("[wifi] forcing reconnect"); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); downSince = now; }

  if (now - lastStat >= 2000) {
    static uint32_t lastPz = 0; uint32_t pzNow = pzW; float pzHz = (pzNow - lastPz) * 1000.0f / (now - lastStat); lastPz = pzNow;
    float kbps = (stBytes - lastBytes) / (float)(now - lastStat);
    lastBytes = stBytes; lastStat = now;
        Serial.printf("[rice-node] wifi=%s rssi=%d ip=%s seq=%u ok=%u err=%u drop=%u pz_overrun=%u piezo=%.0fHz %.1f kB/s heap=%u\n",
                  wifi ? "up" : "down", wifi ? WiFi.RSSI() : 0, WiFi.localIP().toString().c_str(),
                  stSeq, stPostOk, stPostErr, stDropNoBuf, stAdcBad, pzHz, kbps, (unsigned)ESP.getFreeHeap());
  }
  delay(20);
}
