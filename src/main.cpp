// Tip-to-play cosplay sign.
//
// When the sign is tipped over past a threshold angle, it plays a random WAV
// file from the microSD card. Hardware:
//
//   QT Py ESP32-S3 (5426)
//   Audio BFF (5769)   -> MAX98357 I2S amp + microSD, flush on the back
//   Charger BFF (5397) -> LiPo charging, hand-wired (5V, GND, monitor -> TX)
//   ADXL343 (4097)     -> accelerometer on the STEMMA QT port (Wire1)
//
// See README.md for wiring and SD-card details.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL343.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <Adafruit_NeoPixel.h>
#include <vector>

// ---------------------------------------------------------------------------
// Pin map: QT Py ESP32-S3 + Audio BFF (5769) + Charger BFF (5397)
// ---------------------------------------------------------------------------
// Audio BFF I2S -> MAX98357
static const int PIN_I2S_BCLK = A3;  // bit clock
static const int PIN_I2S_LRC  = A2;  // word select / LR clock
static const int PIN_I2S_DIN  = A1;  // serial data
// Audio BFF microSD, on the default SPI bus (SCK / MOSI / MISO pads)
static const int PIN_SD_CS    = A0;
// Charger BFF battery-voltage divider, hand-wired to TX (not A2)
static const int PIN_VBAT     = TX;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static const float    AUDIO_GAIN  = 1.0f;   // 0.0 - 1.0
static const float    TIP_TRIGGER = 0.0f;  // cos(~57 deg): tipped far enough to fire
static const float    TIP_REARM   = 0.6f;  // cos(~26 deg): back near upright to re-arm
static const uint32_t BATT_PERIOD = 30000;  // ms between battery prints
static const uint32_t SAMPLE_MS   = 20;     // accelerometer poll period
static const uint8_t  LED_BRIGHTNESS = 40;    // onboard NeoPixel brightness (0-255)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Adafruit_ADXL343 accel = Adafruit_ADXL343(343, &Wire1);

// Onboard NeoPixel (single RGB pixel) used as a status light.
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

AudioGeneratorWAV *wav  = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S    *out  = nullptr;

std::vector<String> sounds;
float    refX = 0, refY = 0, refZ = 1;  // upright gravity reference (unit vector)
bool     armed = true;
uint32_t lastBatt = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Set the onboard NeoPixel to an RGB color. Brightness is scaled globally by
// LED_BRIGHTNESS, so pass full 0-255 components here.
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// Live tip readout: green when upright, fading through yellow to red as the
// sign leans toward the trigger threshold. dot is cos(angle from upright).
static void ledTipReadout(float dot) {
  float frac = (dot - TIP_TRIGGER) / (1.0f - TIP_TRIGGER);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  ledSet((uint8_t)(255 * (1.0f - frac)), (uint8_t)(255 * frac), 0);
}

// Read the current gravity direction as a unit vector. Returns false if the
// accelerometer read fails or the magnitude is implausibly small.
static bool readGravity(float &x, float &y, float &z) {
  sensors_event_t e;
  if (!accel.getEvent(&e)) return false;
  float mag = sqrtf(e.acceleration.x * e.acceleration.x +
                    e.acceleration.y * e.acceleration.y +
                    e.acceleration.z * e.acceleration.z);
  if (mag < 0.1f) return false;
  x = e.acceleration.x / mag;
  y = e.acceleration.y / mag;
  z = e.acceleration.z / mag;
  return true;
}

// Build the list of playable WAV files from the SD card root.
static void scanSounds() {
  sounds.clear();
  File root = SD.open("/");
  if (!root) {
    Serial.println("[sd] cannot open root");
    return;
  }
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!f.isDirectory()) {
      String name = f.name();
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".wav")) {
        if (!name.startsWith("/")) name = "/" + name;
        sounds.push_back(name);
        Serial.printf("[sd] found %s\n", name.c_str());
        // Blink the status LED once per file found so load progress is visible.
        ledSet(0, 255, 0);
        delay(40);
        ledSet(0, 0, 0);
        delay(40);
      }
    }
    f.close();
  }
  root.close();
  Serial.printf("[sd] %u sound file(s)\n", (unsigned)sounds.size());
}

static void stopSound() {
  if (wav)  { wav->stop(); delete wav;  wav = nullptr; }
  if (file) { delete file; file = nullptr; }
}

// Start playing a randomly chosen clip. Does nothing if none are loaded.
static void startRandomSound() {
  if (sounds.empty()) {
    Serial.println("[play] no sounds on card");
    return;
  }
  int idx = (int)random((long)sounds.size());
  Serial.printf("[play] %s\n", sounds[idx].c_str());
  file = new AudioFileSourceSD(sounds[idx].c_str());
  wav  = new AudioGeneratorWAV();
  if (!wav->begin(file, out)) {
    Serial.println("[play] failed to start");
    stopSound();
  } else {
    ledSet(255, 0, 255); // magenta while a clip is playing
  }
}

static void printBattery() {
  // The Charger BFF divider halves the pack voltage, so double the reading.
  uint32_t mv = analogReadMilliVolts(PIN_VBAT) * 2;
  Serial.printf("[batt] %.2f V\n", mv / 1000.0f);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTip-to-play sign starting");

  // Onboard NeoPixel as a status light. The QT Py gates pixel power on a
  // dedicated pin that must be driven high before the pixel will light.
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  ledSet(0, 0, 0);

  analogReadResolution(12);

  // Accelerometer on the STEMMA QT port (Wire1 on this board).
  Wire1.setPins(SDA1, SCL1);
  Wire1.begin();
  if (!accel.begin()) {
    Serial.println("[accel] ADXL343 not found - check the STEMMA QT cable");
  } else {
    accel.setRange(ADXL343_RANGE_4_G);
    Serial.println("[accel] ready");
  }

  // microSD over the default SPI bus.
  ledSet(0, 0, 255); // blue: reading the card
  SPI.begin(SCK, MISO, MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[sd] card init failed");
    ledSet(255, 0, 0); // red: no card / init failed
    delay(800);
  } else {
    scanSounds();
    // Hold a summary color: green if clips loaded, amber if the card is fine
    // but empty. Overwritten by the live tip readout once the loop starts.
    ledSet(sounds.empty() ? 255 : 0, sounds.empty() ? 120 : 255, 0);
    delay(500);
  }

  // I2S output to the MAX98357 amp on the Audio BFF.
  out = new AudioOutputI2S();
  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  out->SetGain(AUDIO_GAIN);

  // Capture the current orientation as "upright". Hold the sign upright at
  // boot; tip detection is measured relative to this reference, so the mounting
  // angle does not matter.
  float sx = 0, sy = 0, sz = 0;
  int n = 0;
  for (int i = 0; i < 20; i++) {
    float x, y, z;
    if (readGravity(x, y, z)) { sx += x; sy += y; sz += z; n++; }
    delay(20);
  }
  if (n > 0) { refX = sx / n; refY = sy / n; refZ = sz / n; }
  Serial.printf("[tip] upright reference = (%.2f, %.2f, %.2f)\n", refX, refY, refZ);

  randomSeed(esp_random());
}

void loop() {
  // If a clip is playing, keep feeding it and ignore new tips until it ends.
  if (wav && wav->isRunning()) {
    if (!wav->loop()) stopSound();
    return;
  }

  float x, y, z;
  if (readGravity(x, y, z)) {
    // Dot product of two unit vectors == cosine of the angle from upright.
    float dot = x * refX + y * refY + z * refZ;
    Serial.print("[tip] angle: ");
    Serial.println(dot);
    ledTipReadout(dot); // live orientation readout (green upright -> red tipped)
    if (armed && dot < TIP_TRIGGER) {
      armed = false;
      startRandomSound();
    } else if (!armed && dot > TIP_REARM) {
      armed = true;
      Serial.println("[tip] re-armed");
    }
  }

  uint32_t now = millis();
  if (now - lastBatt > BATT_PERIOD) {
    lastBatt = now;
    printBattery();
  }

  delay(SAMPLE_MS);
}
