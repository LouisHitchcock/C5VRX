// C5VRX-5 by Twotoz and the C5VRX contributors: joystick bench firmware.
//
// Reads an SKRHABE010 5-way switch on a resistor ladder with an ESP32-S3
// Super Mini and reports it over USB serial; ../menu_mock.py drives the menu
// mock from it. The ADC pin is LADDER_PIN (platformio.ini). See ../README.md.
//
// Ladder: 10k pull-up from 3V3 to the ADC node, each switch pulls the node to
// GND through its own resistor. Expected node voltages:
//   UP 4.7k 1.06V, DOWN 6.8k 1.34V, LEFT 15.1k 1.98V, RIGHT 22k 2.27V,
//   MID 47k 2.72V, released 3.30V.
//
// Prints press/release events, tracks the min/max mV seen per button and
// reports once all five have been pressed. Serial commands:
//   r  toggle raw mV streaming (for checking margins)
//   s  print the per-button stats
//   c  clear the stats

#include <Arduino.h>

#ifndef LADDER_PIN
#define LADDER_PIN 11
#endif

static const int kAdcPin = LADDER_PIN;
static const int kLedPin = 48;  // onboard WS2812
static const int kSamples = 16;
static const int kSampleIntervalMs = 5;
static const int kStableReads = 4;  // ~20 ms debounce

enum Button { UP, DOWN, LEFT, RIGHT, MID, NONE, kButtonCount };

struct Level {
  const char *name;
  int expectedMv;
  int upperMv;  // readings below this (and above the previous) map here
  uint8_t r, g, b;
};

// Bounds are midpoints between neighbouring levels. The S3 ADC at 11 dB tops
// out around 3.1 V, so released reads as full scale (an ADC2 pin reports
// 4968 mV there, an artefact of the conversion clamping) rather than 3300; the
// MID/NONE bound sits between 2720 and that.
static const Level kLevels[kButtonCount] = {
    {"UP", 1055, 1195, 0, 0, 40},
    {"DOWN", 1336, 1660, 0, 40, 0},
    {"LEFT", 1985, 2125, 40, 0, 0},
    {"RIGHT", 2269, 2495, 40, 20, 0},
    {"MID", 2721, 2900, 40, 0, 40},
    {"NONE", 3300, 99999, 0, 0, 0},
};

struct Stats {
  uint32_t presses;
  int minMv;
  int maxMv;
};

static Stats stats[kButtonCount];
static Button current = NONE;
static Button candidate = NONE;
static int candidateCount = 0;
static bool rawMode = false;
static bool allReported = false;

static int readMv() {
  uint32_t sum = 0;
  for (int i = 0; i < kSamples; i++) sum += analogReadMilliVolts(kAdcPin);
  return sum / kSamples;
}

static Button classify(int mv) {
  for (int i = 0; i < kButtonCount; i++)
    if (mv < kLevels[i].upperMv) return (Button)i;
  return NONE;
}

static void clearStats() {
  for (auto &s : stats) s = {0, 99999, 0};
  allReported = false;
}

static void printStats() {
  Serial.println("button  presses  min mV  max mV  expected  margin");
  for (int i = 0; i < kButtonCount; i++) {
    const Stats &s = stats[i];
    if (s.maxMv == 0) {
      Serial.printf("%-6s  %7u       -       -  %8d  not seen\n", kLevels[i].name, s.presses,
                    kLevels[i].expectedMv);
      continue;
    }
    int lower = i == 0 ? 0 : kLevels[i - 1].upperMv;
    int margin = min(s.minMv - lower, kLevels[i].upperMv - s.maxMv);
    Serial.printf("%-6s  %7u  %6d  %6d  %8d  %5d mV\n", kLevels[i].name, s.presses, s.minMv,
                  s.maxMv, kLevels[i].expectedMv, margin);
  }
}

static void setLed(Button b) {
  neopixelWrite(kLedPin, kLevels[b].r, kLevels[b].g, kLevels[b].b);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(kAdcPin, ADC_11db);
  setLed(NONE);
  clearStats();

  delay(1500);  // give the USB CDC port time to enumerate
  Serial.printf("\nSKRHABE010 ladder test, ADC on GPIO%d\n", kAdcPin);
  Serial.println("Commands: r = raw stream, s = stats, c = clear");
  Serial.printf("Idle reading: %d mV\n", readMv());
}

void loop() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'r': rawMode = !rawMode; break;
      case 's': printStats(); break;
      case 'c': clearStats(); Serial.println("Stats cleared"); break;
    }
  }

  int mv = readMv();
  Button b = classify(mv);

  if (rawMode) Serial.printf("%4d mV  %s\n", mv, kLevels[b].name);

  if (b == candidate) {
    if (candidateCount < kStableReads) candidateCount++;
  } else {
    candidate = b;
    candidateCount = 1;
  }

  if (candidateCount >= kStableReads && candidate != current) {
    if (current != NONE) Serial.printf("release %s\n", kLevels[current].name);
    current = candidate;
    if (current != NONE) {
      stats[current].presses++;
      Serial.printf("press   %-5s %4d mV (expected %d)\n", kLevels[current].name, mv,
                    kLevels[current].expectedMv);
    }
    setLed(current);
  }

  // Stats only count settled readings so edge transitions don't widen them.
  if (candidateCount >= kStableReads) {
    Stats &s = stats[current];
    s.minMv = min(s.minMv, mv);
    s.maxMv = max(s.maxMv, mv);
  }

  if (!allReported) {
    bool all = true;
    for (int i = 0; i < NONE; i++) all &= stats[i].presses > 0;
    if (all) {
      allReported = true;
      Serial.println("\nAll five directions detected:");
      printStats();
    }
  }

  delay(kSampleIntervalMs);
}
