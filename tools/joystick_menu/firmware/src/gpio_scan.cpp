// C5VRX-5 by Twotoz and the C5VRX contributors: GPIO survey for the ESP32-S3
// Super Mini: finds which pad the switch ladder is really wired to, in case
// the silkscreen doesn't match the GPIO numbers.
//
// 1. Survey: each pin is read digitally with the internal pull-down and then
//    the internal pull-up (~45k). A floating pin follows the pull; a pin with
//    an external pull-up reads 1 both ways. Pins that aren't floating also get
//    an analog reading with no pull. (Analog reads switch the internal pulls
//    off, which is why the pull test is digital.)
// 2. Watch: pins that aren't floating are watched in mV; floating ones are
//    watched digitally with the pull-up on, which still catches UP/DOWN/LEFT
//    pulling the pin low if the external 10k is open. Press each direction.
//
// Serial commands: s re-run the survey and watch again, g stream frames for
// ../gpio_gui.py, a<gpio> stream one ADC pin fast for ../ladder_check.py
// (e.g. a11), x stop streaming.
//
// Stream frame, one line per pass over every pin:
//   F <gpio>:<pull-down level>:<pull-up level>:<mV with no pull, -1 if digital only> ...
// Single-pin stream, every 5 ms, no pull:
//   A <raw 0-4095> <mV>
//
// Skipped: 19/20 (USB D-/D+, touching them kills the serial port), 26-37
// (flash/PSRAM), 48 (onboard LED).

#include <Arduino.h>

static const int kAnalogPins[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
static const int kDigitalPins[] = {0, 21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47};
static const int kAnalogCount = sizeof(kAnalogPins) / sizeof(kAnalogPins[0]);
static const int kDigitalCount = sizeof(kDigitalPins) / sizeof(kDigitalPins[0]);

static const int kChangeMv = 250;  // movement that counts as a change in watch mode
static const int kStableReads = 3;

static int candidate[kAnalogCount];
static int candidateCount[kAnalogCount];
static int digitalLast[kDigitalCount];

static int readMv(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(pin);
  return sum / 8;
}

static bool floating[kAnalogCount];

static int digitalWithPull(int pin, int mode) {
  pinMode(pin, mode);
  delay(3);
  return digitalRead(pin);
}

static bool streaming = false;
static int fastPin = -1;

static void sendFast() {
  uint32_t raw = 0, mv = 0;
  for (int i = 0; i < 8; i++) {
    raw += analogRead(fastPin);
    mv += analogReadMilliVolts(fastPin);
  }
  Serial.printf("A %u %u\n", raw / 8, mv / 8);
}

static int quickPull(int pin, int mode) {
  pinMode(pin, mode);
  delayMicroseconds(300);
  return digitalRead(pin);
}

static void sendFrame() {
  String line = "F";
  char entry[24];
  for (int pin : kAnalogPins) {
    int down = quickPull(pin, INPUT_PULLDOWN);
    int up = quickPull(pin, INPUT_PULLUP);
    pinMode(pin, INPUT);
    delayMicroseconds(300);
    snprintf(entry, sizeof(entry), " %d:%d:%d:%d", pin, down, up, readMv(pin));
    line += entry;
  }
  for (int pin : kDigitalPins) {
    int down = quickPull(pin, INPUT_PULLDOWN);
    int up = quickPull(pin, INPUT_PULLUP);
    snprintf(entry, sizeof(entry), " %d:%d:%d:-1", pin, down, up);
    line += entry;
  }
  Serial.println(line);
}

static const char *describe(int down, int up) {
  if (!down && up) return "floating";
  if (down && up) return "tied high";
  if (!down && !up) return "tied low";
  return "?";
}

static void survey() {
  Serial.println("\n=== Survey: digital with pull-down / pull-up, then mV with no pull ===");
  for (int i = 0; i < kAnalogCount; i++) {
    int pin = kAnalogPins[i];
    int down = digitalWithPull(pin, INPUT_PULLDOWN);
    int up = digitalWithPull(pin, INPUT_PULLUP);
    floating[i] = !down && up;
    if (floating[i]) {
      Serial.printf("GPIO%-2d  %d / %d  floating\n", pin, down, up);
      continue;
    }
    pinMode(pin, INPUT);
    delay(3);
    Serial.printf("GPIO%-2d  %d / %d  %-9s  %4d mV  <--\n", pin, down, up, describe(down, up),
                  readMv(pin));
  }
  for (int i = 0; i < kDigitalCount; i++) {
    int pin = kDigitalPins[i];
    int down = digitalWithPull(pin, INPUT_PULLDOWN);
    int up = digitalWithPull(pin, INPUT_PULLUP);
    Serial.printf("GPIO%-2d  %d / %d  %s%s\n", pin, down, up, describe(down, up),
                  (!down && up) ? "" : "  <--");
  }
}

static int analogLast[kAnalogCount];

static void startWatch() {
  for (int i = 0; i < kAnalogCount; i++) {
    int pin = kAnalogPins[i];
    pinMode(pin, floating[i] ? INPUT_PULLUP : INPUT);
  }
  for (int pin : kDigitalPins) pinMode(pin, INPUT_PULLUP);
  delay(5);
  for (int i = 0; i < kAnalogCount; i++) {
    int pin = kAnalogPins[i];
    analogLast[i] = floating[i] ? digitalRead(pin) : readMv(pin);
    candidate[i] = analogLast[i];
    candidateCount[i] = 0;
  }
  for (int i = 0; i < kDigitalCount; i++) digitalLast[i] = digitalRead(kDigitalPins[i]);
  Serial.println("\n=== Watching. Press each switch direction. ===");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  for (int pin : kAnalogPins) analogSetPinAttenuation(pin, ADC_11db);
  delay(1500);  // give the USB CDC port time to enumerate
  Serial.println("\nS3 Super Mini GPIO scan. Command: s = survey again");
  survey();
  startWatch();
}

void loop() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 's': streaming = false; fastPin = -1; survey(); startWatch(); break;
      case 'g': streaming = true; fastPin = -1; break;
      case 'a': {
        int pin = Serial.parseInt();
        bool ok = false;
        for (int p : kAnalogPins) ok |= p == pin;
        if (ok) {
          streaming = false;
          fastPin = pin;
          pinMode(pin, INPUT);
        }
        break;
      }
      case 'x': streaming = false; fastPin = -1; survey(); startWatch(); break;
    }
  }

  if (streaming) {
    sendFrame();
    return;
  }
  if (fastPin >= 0) {
    sendFast();
    delay(5);
    return;
  }

  for (int i = 0; i < kAnalogCount; i++) {
    int pin = kAnalogPins[i];
    if (floating[i]) {
      int v = digitalRead(pin);
      if (v == candidate[i]) {
        candidateCount[i]++;
      } else {
        candidate[i] = v;
        candidateCount[i] = 1;
      }
      if (candidateCount[i] == kStableReads && v != analogLast[i]) {
        Serial.printf("GPIO%-2d  %d -> %d  (floating pin, pulled up)\n", pin, analogLast[i], v);
        analogLast[i] = v;
      }
      continue;
    }
    int mv = readMv(pin);
    if (abs(mv - candidate[i]) > kChangeMv / 2) {
      candidate[i] = mv;
      candidateCount[i] = 1;
      continue;
    }
    if (++candidateCount[i] == kStableReads && abs(mv - analogLast[i]) > kChangeMv) {
      Serial.printf("GPIO%-2d  %4d -> %4d mV\n", pin, analogLast[i], mv);
      analogLast[i] = mv;
    }
  }
  for (int i = 0; i < kDigitalCount; i++) {
    int v = digitalRead(kDigitalPins[i]);
    if (v != digitalLast[i]) {
      Serial.printf("GPIO%-2d  %d -> %d\n", kDigitalPins[i], digitalLast[i], v);
      digitalLast[i] = v;
    }
  }
  delay(5);
}
