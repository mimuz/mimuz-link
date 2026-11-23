/*

 basic mimuz-link client example: one button and one LED

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/23 : Start Writing

*/
// The smallest mimuz-link client. Runs on a bare board (no external wiring):
//   - pressing the button sends Note On (middle C, channel 1), releasing it sends Note Off
//   - Note On / Note Off from the PC turn the LED on / off
//   - lines starting with "@" on the USB serial are config commands (web console, examples/pc/mlink_config.py)
//
// mimuz-link handles pairing with the bridge, the radio link and BLE-MIDI when the bridge cannot be reached.
// The sketch only reads the button, drives the LED and passes serial input on.
//
// Board-dependent settings are collected in "Board settings" below; each can be overridden at build time
// (e.g. --build-property "compiler.cpp.extra_flags=-DBASIC_BUTTON_PIN=3").
//
// Build (from examples/client), XIAO ESP32-C6:
//   arduino-cli compile --libraries ../../libs --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app basic
#include <mimuz_link_client.h>

// ---------------- Board settings ----------------

// Button: the BOOT button by default (GPIO9 on ESP32-C3/C6/H2, GPIO0 on the others), pressed = LOW
#ifndef BASIC_BUTTON_PIN
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
#define BASIC_BUTTON_PIN 9
#else
#define BASIC_BUTTON_PIN 0
#endif
#endif

// LED: the board's built-in LED; -1 = no LED
#ifndef BASIC_LED_PIN
#ifdef LED_BUILTIN
#define BASIC_LED_PIN LED_BUILTIN
#else
#define BASIC_LED_PIN -1
#endif
#endif

// Level that turns the LED on (the XIAO boards' user LED is on at LOW)
#ifndef BASIC_LED_ON
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C6) || defined(ARDUINO_XIAO_ESP32S3)
#define BASIC_LED_ON LOW
#else
#define BASIC_LED_ON HIGH
#endif
#endif

// ---------------- Application ----------------

static const uint8_t NOTE = 60;      // middle C
static const uint8_t VELOCITY = 100;
static const uint8_t CHANNEL = 0;    // 0 = MIDI channel 1
static const uint32_t DEBOUNCE_MS = 10;

static void led(bool on) {
  if (BASIC_LED_PIN >= 0) digitalWrite(BASIC_LED_PIN, on ? BASIC_LED_ON : !BASIC_LED_ON);
}

// Received MIDI (called from the mimuz-link receive task, not from loop())
static void onMidi(const uint8_t m[3], uint32_t rxUs) {
  (void)rxUs;
  uint8_t type = m[0] & 0xF0;
  if (type == 0x90 && m[2] > 0) led(true);                          // Note On
  else if (type == 0x80 || (type == 0x90 && m[2] == 0)) led(false);  // Note Off (or Note On with velocity 0)
}

void setup() {
  Serial.begin(115200);
  pinMode(BASIC_BUTTON_PIN, INPUT_PULLUP);
  if (BASIC_LED_PIN >= 0) pinMode(BASIC_LED_PIN, OUTPUT);
  led(false);

  mlink::Config cfg;  // defaults: device name "mi:muz:conESP32C6-<unique ID>", pairs with a bridge on first start
  mlink::onMidi(onMidi);
  mlink::Transport tr = mlink::begin(cfg);
  Serial.printf("basic: ready (%s)\n", tr == mlink::TR_BLE ? "BLE-MIDI" : "ESP-NOW");
}

void loop() {
  // Button, with a simple debounce: the level has to stay the same for DEBOUNCE_MS
  static bool pressed = false;
  static bool lastRead = false;
  static uint32_t changedMs = 0;
  bool now = digitalRead(BASIC_BUTTON_PIN) == LOW;
  uint32_t ms = millis();
  if (now != lastRead) {
    lastRead = now;
    changedMs = ms;
  } else if (now != pressed && ms - changedMs >= DEBOUNCE_MS) {
    pressed = now;
    if (pressed) mlink::sendMidi(0x90 | CHANNEL, NOTE, VELOCITY);
    else mlink::sendMidi(0x80 | CHANNEL, NOTE, 0);
  }

  // Config commands from the USB serial (lines starting with "@")
  while (Serial.available()) mlink::serialInput((char)Serial.read());

  mlink::loop();  // keeps the link alive, follows the bridge, searches again when the link is lost
  delay(1);
}
