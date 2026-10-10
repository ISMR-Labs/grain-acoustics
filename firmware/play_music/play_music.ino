// Plays a synthesized melody through a UDA1334A I2S DAC (CJMCU-1334) -> mono amp -> speaker.
// DAC wiring: BCLK GPIO14, WSEL GPIO27, DIN GPIO23, VIN 3V3, GND GND. LOUT -> amp IN+, AGND -> amp IN-.
#include <ESP_I2S.h>

#define PIN_BCLK 14
#define PIN_WSEL 27
#define PIN_DIN  23
#define RATE     22050
#define VOLUME   0.35f      // 0..1, keep low: the amp does the work

I2SClass dac;

// Ode to Joy (Beethoven, public domain). {MIDI note, length in 1/8 notes}. 0 = rest.
struct Note { uint8_t midi; uint8_t len; };
const Note tune[] = {
  {64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{64,3},{62,1},{62,4},
  {64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{62,3},{60,1},{60,4},
  {62,2},{62,2},{64,2},{60,2},{62,2},{64,1},{65,1},{64,2},{60,2},{62,2},{64,1},{65,1},{64,2},{62,2},{60,2},{62,2},{55,4},
  {64,2},{64,2},{65,2},{67,2},{67,2},{65,2},{64,2},{62,2},{60,2},{60,2},{62,2},{64,2},{62,3},{60,1},{60,4},{0,4},
};
const float EIGHTH_S = 0.20f;   // tempo: one eighth note = 200 ms

static float midiHz(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

void setup() {
  Serial.begin(115200);
  dac.setPins(PIN_BCLK, PIN_WSEL, PIN_DIN, -1);
  if (!dac.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S init failed"); while (1) delay(1000);
  }
  Serial.println("playing...");
}

void playNote(float hz, float seconds) {
  const int total = (int)(seconds * RATE);
  static int16_t buf[512];              // 256 stereo frames
  float phase = 0, step = 2 * PI * hz / RATE;
  for (int done = 0; done < total; ) {
    int frames = min(256, total - done);
    for (int i = 0; i < frames; i++, done++) {
      float t = (float)done / RATE;
      // simple pluck/organ envelope: quick attack, gentle decay, short release
      float env = min(1.0f, t * 60.0f) * expf(-t * 1.5f);
      float rel = (float)(total - done) / RATE; if (rel < 0.02f) env *= rel / 0.02f;
      float s = hz > 0 ? (sinf(phase) + 0.4f * sinf(2 * phase) + 0.15f * sinf(3 * phase)) : 0;
      phase += step; if (phase > 2 * PI) phase -= 2 * PI;
      int16_t v = (int16_t)(s * env * VOLUME * 20000.0f);
      buf[2 * i] = v; buf[2 * i + 1] = v;
    }
    dac.write((uint8_t *)buf, frames * 4);
  }
}

void loop() {
  for (auto &n : tune) playNote(n.midi ? midiHz(n.midi) : 0, n.len * EIGHTH_S);
  playNote(0, 1.0f);   // pause, then repeat
}
