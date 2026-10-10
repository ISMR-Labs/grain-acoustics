// Rice-bag sensor node: 2x INMP441 (I2S stereo) + 2x piezo (ADC1 DMA) -> HTTP stream
// Board: ESP32 Dev Module (ESP32-WROOM-32U). Arduino-ESP32 core 3.x.
//
// Wiring (see chat):
//   Mic A/B : VDD 3V3, GND, SCK GPIO26, WS GPIO25, SD GPIO33, L/R A->GND, B->3V3
//   Piezo 1 : GPIO34 (ADC1_CH6)   Piezo 2 : GPIO35 (ADC1_CH7)   (0.55 V bias, 0 dB atten)
//   LED     : GPIO4 -> 330R -> LED -> GND
//
// LED: slow blink = connecting Wi-Fi, solid = streaming OK, fast blink = server errors.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "driver/i2s_std.h"
#include "esp_adc/adc_continuous.h"
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

#define PIN_I2S_SCK 26
#define PIN_I2S_WS  25
#define PIN_I2S_SD  33
#define PIN_LED     4
#define PIEZO1_CH   ADC_CHANNEL_6   // GPIO34
#define PIEZO2_CH   ADC_CHANNEL_7   // GPIO35

#define MIC_RATE        16000       // Hz per mic (16-bit samples sent)
#define MIC_SHIFT       14          // 32-bit I2S slot -> int16 (24 valid bits, keep bits 29..14)
#define ADC_TOTAL_RATE  20000       // ESP32 minimum; 2 channels -> 10 kHz per piezo
#define PIEZO_RATE      (ADC_TOTAL_RATE / 2)
#define CHUNK_MS        100         // one HTTP POST per chunk
#define QUEUE_DEPTH     8           // chunks buffered while Wi-Fi hiccups (~10 kB each)

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
static i2s_chan_handle_t rx = nullptr;
static adc_continuous_handle_t adc = nullptr;
static int32_t i2sBuf[MIC_FRAMES * 2];
static uint8_t adcBuf[1024];

static volatile uint32_t stSeq = 0, stDropNoBuf = 0, stPostErr = 0, stPostOk = 0, stBytes = 0;
static volatile uint32_t stAdcBad = 0;
static volatile bool serverOk = false;

// ---------------- I2S (mics) ----------------
static void setupI2S() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);  // I2S1: I2S0 is used by the ADC DMA on ESP32
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 512;
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
}

// ---------------- ADC DMA (piezos) ----------------
static void setupADC() {
  adc_continuous_handle_cfg_t hc = {};
  hc.max_store_buf_size = 8192;
  hc.conv_frame_size = 512;
  hc.flags.flush_pool = 1;
  ESP_ERROR_CHECK(adc_continuous_new_handle(&hc, &adc));

  adc_digi_pattern_config_t pat[2] = {};
  pat[0].atten = ADC_ATTEN_DB_0;  pat[0].channel = PIEZO1_CH; pat[0].unit = ADC_UNIT_1; pat[0].bit_width = ADC_BITWIDTH_12;
  pat[1].atten = ADC_ATTEN_DB_0;  pat[1].channel = PIEZO2_CH; pat[1].unit = ADC_UNIT_1; pat[1].bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t dc = {};
  dc.pattern_num = 2;
  dc.adc_pattern = pat;
  dc.sample_freq_hz = ADC_TOTAL_RATE;
  dc.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  dc.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  ESP_ERROR_CHECK(adc_continuous_config(adc, &dc));
  ESP_ERROR_CHECK(adc_continuous_start(adc));
}

// Drain everything the ADC DMA has right now into the chunk. Returns frames (pairs).
static uint32_t drainADC(Chunk *c) {
  static int32_t carry1 = -1, carry2 = -1;   // unpaired sample left from the previous chunk
  uint32_t n1 = 0, n2 = 0;
  if (carry1 >= 0) { c->piezo[0] = carry1; n1 = 1; carry1 = -1; }
  if (carry2 >= 0) { c->piezo[1] = carry2; n2 = 1; carry2 = -1; }

  for (;;) {
    uint32_t got = 0;
    esp_err_t r = adc_continuous_read(adc, adcBuf, sizeof(adcBuf), &got, 0);
    if (r != ESP_OK || got == 0) break;
    for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
      adc_digi_output_data_t *p = (adc_digi_output_data_t *)&adcBuf[i];
      uint32_t ch = p->type1.channel, v = p->type1.data;
      if (ch == PIEZO1_CH)      { if (n1 < PIEZO_MAX_FRAMES) c->piezo[2 * n1++] = v; }
      else if (ch == PIEZO2_CH) { if (n2 < PIEZO_MAX_FRAMES) c->piezo[2 * n2++ + 1] = v; }
      else stAdcBad++;
    }
  }
  uint32_t frames = n1 < n2 ? n1 : n2;
  if (n1 > frames) carry1 = c->piezo[2 * frames];
  if (n2 > frames) carry2 = c->piezo[2 * frames + 1];
  return frames;
}

// ---------------- Acquisition task ----------------
static void acqTask(void *) {
  uint32_t seq = 0;
  // Let the INMP441s settle and discard their start-up garbage.
  size_t got;
  for (int i = 0; i < 4; i++) i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, portMAX_DELAY);
  { uint32_t g; while (adc_continuous_read(adc, adcBuf, sizeof(adcBuf), &g, 0) == ESP_OK && g) {} }

  for (;;) {
    Chunk *c = nullptr;
    xQueueReceive(freeQ, &c, 0);
    // Always read the I2S so the DMA never overflows; if no free chunk, the data is dropped.
    esp_err_t r = i2s_channel_read(rx, i2sBuf, sizeof(i2sBuf), &got, pdMS_TO_TICKS(500));
    if (!c) {  // no free buffer: discard this chunk's mic data and whatever the ADC has queued
      stDropNoBuf++;
      uint32_t got2;
      while (adc_continuous_read(adc, adcBuf, sizeof(adcBuf), &got2, 0) == ESP_OK && got2) {}
      continue;
    }

    memcpy(c->h.magic, "SNS1", 4);
    c->h.seq = seq++;
    c->h.uptime_ms = millis();
    c->h.mic_rate = MIC_RATE;
    c->h.piezo_rate = PIEZO_RATE;
    c->h.mic_frames = MIC_FRAMES;
    c->h.flags = 0;
    if (r == ESP_OK && got == sizeof(i2sBuf)) {
      for (int i = 0; i < MIC_FRAMES * 2; i++) c->mic[i] = (int16_t)(i2sBuf[i] >> MIC_SHIFT);
      c->h.flags |= 1;
    } else {
      memset(c->mic, 0, sizeof(c->mic));
    }
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

  setupADC();                    // ADC DMA first: it claims I2S0 on the ESP32
  setupI2S();                    // mics on I2S1

  xTaskCreatePinnedToCore(acqTask,    "acq",  4096, nullptr, 20, nullptr, 1);
  xTaskCreatePinnedToCore(senderTask, "send", 8192, nullptr, 5,  nullptr, 0);
}

void loop() {
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
    float kbps = (stBytes - lastBytes) / (float)(now - lastStat);
    lastBytes = stBytes; lastStat = now;
    Serial.printf("[rice-node] wifi=%s rssi=%d ip=%s seq=%u ok=%u err=%u drop=%u adcbad=%u %.1f kB/s heap=%u\n",
                  wifi ? "up" : "down", wifi ? WiFi.RSSI() : 0, WiFi.localIP().toString().c_str(),
                  stSeq, stPostOk, stPostErr, stDropNoBuf, stAdcBad, kbps, (unsigned)ESP.getFreeHeap());
  }
  delay(20);
}
