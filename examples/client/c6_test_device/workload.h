/*

 workload simulated controller workload for the C6 test device

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing
 - 2026/11/20 : Remove the switch input (runs on a bare board)

*/
// Test device (application side): puts a production-like load on the C6
//
// - Scan task (1 ms period, woken by esp_timer):
//     24-bit SPI transfer on the production board's pins (read 74HC165 x2 + write 74HC595), state-transition decoding of 4 encoders,
//     and burst generation.
//     MIDI for the burst and encoders is sent from this task (same structure as production).
// - loop() load: busy-wait for a given time on every loop() iteration (stand-in for LED effects etc.). CC ch16 #125 = value x 100 us
// - Encoder simulation: CC ch16 #126 = rotation speed per encoder (clicks/s)
// Used without the board, so MISO has an internal pull-down to read 0 (there is no real encoder input).
// Independent of the communication layer (mimuz-link). Sending just calls the function passed to begin().
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <esp_timer.h>

namespace wl {

static const int PIN_SCK = 19, PIN_MISO = 20, PIN_MOSI = 18, PIN_LATCH = 21;  // production board (D8/D9/D10/D3)
static const uint32_t SPI_HZ = 2000000;

typedef void (*SendFn)(uint8_t st, uint8_t d1, uint8_t d2);
static SendFn sendFn = nullptr;
static TaskHandle_t scanTaskH = nullptr;
static esp_timer_handle_t tickTimer;

static volatile uint32_t loopLoadUs = 0;
static volatile uint32_t scanCount = 0, scanMaxUs = 0, scanLateMax = 0;
static volatile uint16_t burstCount = 500, burstSeq = 0, burstPeriodMs = 0, burstPhase = 0;
static volatile bool burstLoop = false;   // true: send burst as TM_LOOP (0xA8) (for two-hop latency measurement)
static volatile bool burstNotes = false;  // true: send burst as Note On / Note Off pairs (to check Note Off retransmission)
static uint8_t ledByte = 0;

// Encoder: state-transition table (impossible transitions are 0)
static const int8_t ENC_TBL[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
static uint8_t encPrev[4] = {0, 0, 0, 0};
static int8_t encAcc[4] = {0, 0, 0, 0};

// Encoder simulation
static volatile uint8_t encSimRate = 0;  // clicks/s (per encoder)
static uint32_t encSimAcc = 0;
static uint8_t encSimPhase = 0;
static const uint8_t QSEQ[4] = {0, 1, 3, 2};  // Gray code (1 click = 4 steps)
static volatile uint32_t encSent = 0;
static volatile bool extLed = false;  // for lighting the LED from outside (e.g. a received Note On)

static void IRAM_ATTR onTick(void *) {
  BaseType_t w = pdFALSE;
  if (scanTaskH) vTaskNotifyGiveFromISR(scanTaskH, &w);
}

static void scanTask(void *) {
  uint64_t expect = esp_timer_get_time();
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint64_t t0 = esp_timer_get_time();
    expect += 1000;
    uint32_t late = (uint32_t)(t0 > expect ? t0 - expect : 0);
    if (late > 100000) { expect = t0; late = 0; }  // discard large offsets (e.g. right after startup)
    if (late > scanLateMax) scanLateMax = late;

    // --- SPI 24 bit: LATCH Low->High (165 captures / 595 latches) -> 3-byte transfer (LED byte last)
    digitalWrite(PIN_LATCH, LOW);
    digitalWrite(PIN_LATCH, HIGH);
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    uint8_t encBits = SPI.transfer(0);
    uint8_t swBits = SPI.transfer(0);
    SPI.transfer(ledByte);
    SPI.endTransaction();
    (void)swBits;

    // --- Encoder simulation (if enabled, used instead of the value read over SPI)
    uint8_t rate = encSimRate;
    if (rate) {
      encSimAcc += (uint32_t)rate * 4;  // added every 1 ms; 1000 = one step
      if (encSimAcc >= 1000) {
        encSimAcc -= 1000;
        encSimPhase = (encSimPhase + 1) & 3;
      }
      uint8_t ab = QSEQ[encSimPhase];
      encBits = ab | (ab << 2) | (ab << 4) | (ab << 6);  // turn all 4 at the same speed
    }

    // --- 4 encoders
    for (int i = 0; i < 4; i++) {
      uint8_t ab = (encBits >> (i * 2)) & 3;
      int8_t d = ENC_TBL[(encPrev[i] << 2) | ab];
      encPrev[i] = ab;
      encAcc[i] += d;
      if (encAcc[i] >= 4 || encAcc[i] <= -4) {
        if (sendFn) sendFn(0xB0, 20 + i, encAcc[i] > 0 ? 65 : 63);  // relative CC
        encSent++;
        encAcc[i] = 0;
      }
    }

    digitalWrite(LED_BUILTIN, extLed ? LOW : HIGH);  // lit by a Note On from the PC

    // --- burst (period in 1 ms steps)
    if (burstPeriodMs) {
      if (++burstPhase >= burstPeriodMs) {
        burstPhase = 0;
        if (burstNotes) {  // even = Note On, odd = Note Off of the same note (notes 36-83 in turn)
          uint8_t note = 36 + ((burstSeq / 2) % 48);
          if (sendFn) sendFn((burstSeq & 1) ? 0x80 : 0x90, note, (burstSeq & 1) ? 0 : 100);
          if (++burstSeq >= burstCount * 2) burstPeriodMs = 0;
        } else {
          if (sendFn) sendFn(burstLoop ? 0xA8 : 0xAE, burstSeq & 0x7F, (burstSeq >> 7) & 0x7F);
          if (++burstSeq >= burstCount) burstPeriodMs = 0;
        }
      }
    }

    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if (dt > scanMaxUs) scanMaxUs = dt;
    scanCount++;
  }
}

static void begin(SendFn fn, UBaseType_t prio) {
  sendFn = fn;
  pinMode(PIN_LATCH, OUTPUT);
  digitalWrite(PIN_LATCH, HIGH);
  pinMode(LED_BUILTIN, OUTPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  pinMode(PIN_MISO, INPUT_PULLDOWN);  // no board: read 0
  xTaskCreate(scanTask, "scan", 4096, NULL, prio, &scanTaskH);
  esp_timer_create_args_t a = {};
  a.callback = onTick;
  a.name = "scan";
  a.dispatch_method = ESP_TIMER_TASK;
  esp_timer_create(&a, &tickTimer);
  esp_timer_start_periodic(tickTimer, 1000);
}

// Returns true if the CC ch16 message was handled
static bool handleCtrl(uint8_t cc, uint8_t val) {
  if (cc == 118) { burstCount = val ? val * 10 : 500; return true; }
  if (cc == 116 && val) { burstNotes = false; burstLoop = true; burstSeq = 0; burstPhase = 0; burstPeriodMs = val; return true; }
  if (cc == 119 && val) { burstNotes = false; burstLoop = false; burstSeq = 0; burstPhase = 0; burstPeriodMs = val; return true; }
  if (cc == 117 && val) { burstNotes = true; burstSeq = 0; burstPhase = 0; burstPeriodMs = val; return true; }  // "count" pairs of Note On/Off
  if (cc == 125) { loopLoadUs = (uint32_t)val * 100; return true; }
  if (cc == 126) { encSimRate = val; encSimAcc = 0; return true; }
  return false;
}

// Call from loop(): busy-wait for the given time (other tasks still run, but loop() itself does not advance meanwhile)
static void loopLoad() {
  uint32_t us = loopLoadUs;
  if (!us) return;
  uint32_t t = micros();
  while ((uint32_t)(micros() - t) < us) {
  }
}

}  // namespace wl
