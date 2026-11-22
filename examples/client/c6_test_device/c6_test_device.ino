/*

 c6_test_device mimuz-link test device firmware (XIAO ESP32-C6)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing
 - 2026/11/20 : Remove the switch input (runs on a bare board)
 - 2026/11/22 : Build without the scan task with -DTEST_WORKLOAD=0 (for comparing with other libraries)
 - 2026/11/22 : -DTEST_TASK_PRIO=n sets the priorities of the mimuz-link send and receive tasks (for comparison)

*/
// Test device (application): bare XIAO ESP32-C6 (no external wiring)
//
// Communication is left to mimuz-link (libs/mimuz-link). This sketch contains only the device-specific parts:
//   - LED and a production-like workload (workload.h)
//   - handling of the test MIDI messages (mimuz_link_testproto.h; counterpart of latency_test.py on the PC)
//   - a stand-in for a sound module (lights the LED on Note On from the PC, counts sounding notes)
//   - test serial commands
//
// Serial      : USB CDC 115200 (requires board setting CDCOnBoot=Enabled)
//   p / P     : radio round-trip time measurement, 10,000 / 100,000 times (ESP-NOW only)
//   s         : show statistics   c: clear statistics
//   r0..r3    : switch PHY rate (also told to the bridge)  0=1M 1=6M 2=24M 3=MCS4
//   b / e / u : reboot (b = force BLE, e = auto, u = erase registration and pair again)
//
// Build (from examples/client): arduino-cli compile --libraries ../../libs --libraries ../../test --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app c6_test_device
#include <mimuz_link_client.h>
#include <mimuz_link_testproto.h>
#include "workload.h"

#define LED_PIN LED_BUILTIN  // XIAO C6 user LED (Low = on)
#define LED_ON LOW
#define LED_OFF HIGH

static inline uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }

// ---- Two-hop (C6 -> bridge -> C6) latency measurement ----
#define LOOP_MAX 1300
static uint32_t loopSent[LOOP_MAX];  // time TM_LOOP was sent (put into the send queue)
static LatHist hLoop;
static volatile uint32_t loopRx;

// ---- Records for receive-side (PC -> C6) measurement ----
#define RXLOG_MAX 1300
static uint16_t rxLogSeq[RXLOG_MAX];
static uint32_t rxLogT[RXLOG_MAX];
static volatile uint16_t rxLogN = 0;
static volatile bool rxDumpReq = false;

// ---- Sound module stand-in ----
static uint8_t noteBits[16];  // sounding notes (channels are not distinguished)
static volatile uint16_t noteOnRx, noteOffRx;
static uint8_t noteHeldCount() {
  uint8_t c = 0;
  for (int i = 0; i < 16; i++) c += __builtin_popcount(noteBits[i]);
  return c;
}

// Send MIDI. From the 1 ms scan task it does not wait; from elsewhere it waits briefly for room in the queue
static void appSend(uint8_t st, uint8_t d1, uint8_t d2) {
  if (st == TM_LOOP) {
    uint16_t sq = d1 | (d2 << 7);
    if (sq < LOOP_MAX) loopSent[sq] = nowUs();
  }
  bool fromScan = xTaskGetCurrentTaskHandle() == wl::scanTaskH;
  mlink::sendMidi(st, d1, d2, 0, fromScan ? 0 : 100);
}

// Called inside the radio receive callback. Measures the latency of our own TM_LOOP coming back
static bool onMidiFast(const uint8_t m[3], uint32_t rxUs) {
  if (m[0] != TM_LOOP) return false;
  uint16_t sq = m[1] | (m[2] << 7);
  if (sq < LOOP_MAX) {
    hLoop.add(rxUs - loopSent[sq]);
    loopRx++;
  }
  return true;
}

// Test for messages without a channel (clock, etc.): CC ch16 #104
//   0 = clear count and stop echoing / 1 = echo back what is received / 2 = send a fixed sequence / 3 = report received count via #103
static volatile uint32_t sysRx = 0;
static volatile bool sysEcho = false;
static const uint8_t SYS_SET[][3] = {
  {0xFA, 0, 0}, {0xF8, 0, 0}, {0xF8, 0, 0}, {0xF8, 0, 0}, {0xF2, 0x12, 0x34}, {0xFB, 0, 0}, {0xF8, 0, 0},
  {0xF3, 0x05, 0}, {0xF1, 0x23, 0}, {0xF6, 0, 0}, {0xFE, 0, 0}, {0xFC, 0, 0},
};

// Handle received MIDI (called from the mimuz-link receive task)
static void onMidi(const uint8_t m[3], uint32_t rxUs) {
  if (m[0] >= 0xF0) {
    sysRx++;
    if (sysEcho) appSend(m[0], m[1], m[2]);
  } else if (m[0] == TM_CTRL && m[1] == 104) {
    if (m[2] == 0) { sysRx = 0; sysEcho = false; }
    else if (m[2] == 1) sysEcho = true;
    else if (m[2] == 2) { for (auto &s : SYS_SET) appSend(s[0], s[1], s[2]); }
    else if (m[2] == 3) appSend(TM_CTRL, 103, sysRx > 127 ? 127 : sysRx);
  } else if (m[0] == TM_RXTEST) {  // receive-side measurement: only record the arrival time
    if (rxLogN < RXLOG_MAX) {
      rxLogSeq[rxLogN] = m[1] | (m[2] << 7);
      rxLogT[rxLogN] = rxUs;
      rxLogN++;
    }
  } else if (m[0] == TM_CTRL && m[1] == 107) {  // listen channel: 0 = none, 1-16 = that channel, 17 = OMNI
    uint16_t mask = m[2] == 0 ? 0 : m[2] >= 17 ? 0xFFFF : (uint16_t)(1u << (m[2] - 1));
    mlink::setRxChannels(mask, mlink::rxFlags());
  } else if (m[0] == TM_CTRL && m[1] == 106) {  // for testing: 1 = also receive our own messages
    mlink::setRxChannels(mlink::rxChannels(), m[2] ? ENM_SUB_SELF : 0);
  } else if (m[0] == TM_CTRL && m[1] == 105) {  // clear two-hop measurement results
    hLoop.reset();
    loopRx = 0;
  } else if (m[0] == TM_CTRL && m[1] == 108) {  // age after which a stale Note On is dropped (value x 10 ms, 0 = never)
    mlink::setNoteOnMaxAgeMs((uint16_t)m[2] * 10);
  } else if (m[0] == TM_CTRL && m[1] == 127) {
    if (m[2] == 0) rxLogN = 0;
    else if (m[2] == 1) rxDumpReq = true;  // the report is sent from loop(), with gaps between messages
  } else if ((m[0] & 0xF0) == 0x90 && m[2] > 0) {  // Note On -> LED on (sound module stand-in)
    noteBits[m[1] >> 3] |= 1 << (m[1] & 7);
    noteOnRx++;
    wl::extLed = true;
  } else if ((m[0] & 0xF0) == 0x80 || ((m[0] & 0xF0) == 0x90 && m[2] == 0)) {
    noteBits[m[1] >> 3] &= ~(1 << (m[1] & 7));
    noteOffRx++;
    wl::extLed = noteHeldCount() > 0;
  } else if (m[0] == TM_PING_RADIO) {
    appSend(m[0], m[1], m[2]);  // echo back as is
  } else if (m[0] == TM_CTRL && m[1] == 122) {  // info request -> reply with CC ch16
    auto c7 = [](uint32_t v) { return (uint8_t)(v > 127 ? 127 : v); };
    appSend(TM_CTRL, 120, c7(mlink::bleIntervalUnits()));  // BLE connection interval (1.25 ms units)
    appSend(TM_CTRL, 121, c7(mlink::bleMtuSize() / 4));    // MTU / 4
    appSend(TM_CTRL, 123, c7(mlink::bleRetryCount()));     // notify retry count
    appSend(TM_CTRL, 124, c7(mlink::bleDropCount()));      // messages dropped because the queue overflowed
    appSend(TM_CTRL, 110, c7(noteHeldCount()));            // number of notes still sounding
    appSend(TM_CTRL, 111, noteOnRx & 0x7F);                // number of Note On received (14 bit)
    appSend(TM_CTRL, 112, (noteOnRx >> 7) & 0x7F);
    appSend(TM_CTRL, 113, noteOffRx & 0x7F);               // number of Note Off received (14 bit)
    appSend(TM_CTRL, 114, (noteOffRx >> 7) & 0x7F);
  } else if (m[0] == TM_CTRL && m[1] == 109) {  // clear note state and counts
    memset(noteBits, 0, sizeof(noteBits));
    noteOnRx = 0;
    noteOffRx = 0;
  } else if (m[0] == TM_CTRL) {
    wl::handleCtrl(m[1], m[2]);  // burst / loop() load / encoder simulation
  }
}

static void printStats() {
  mlink::printStats(Serial);
  Serial.printf("notes: held=%u onRx=%u offRx=%u | loop test: received=%lu\n", noteHeldCount(), noteOnRx, noteOffRx, (unsigned long)loopRx);
  hLoop.print(Serial, "loop C6->bridge->C6");
  Serial.printf("workload: scans=%lu scanMax=%luus scanLateMax=%luus loopLoad=%luus encSim=%u/s encSent=%lu\n",
                (unsigned long)wl::scanCount, (unsigned long)wl::scanMaxUs, (unsigned long)wl::scanLateMax,
                (unsigned long)wl::loopLoadUs, wl::encSimRate, (unsigned long)wl::encSent);
}

static void serialCmd() {
  static char prev = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (mlink::serialInput(c)) continue;  // lines starting with @ are config commands (handled by mimuz-link)
    if (prev == 'r' && c >= '0' && c < '0' + ENM_RATE_NUM) {
      mlink::setRate(c - '0');
      Serial.printf("rate -> %s\n", enm_rate_name(mlink::rate()));
    } else if (c == 'p' || c == 'P') {
      if (!mlink::radioPing(c == 'p' ? 10000 : 100000)) Serial.println("radio ping は ESP-NOW のときだけ");
    } else if (c == 'b' || c == 'e' || c == 'u') {
      Serial.printf("reboot: %s...\n", c == 'b' ? "BLE-MIDI" : c == 'e' ? "auto (bridge -> ESP-NOW, else BLE)" : "erase registration and pair again");
      mlink::restart(c == 'b' ? mlink::BOOT_BLE : c == 'e' ? mlink::BOOT_AUTO : mlink::BOOT_REPAIR);
    } else if (c == 's') {
      printStats();
    } else if (c == 'c') {
      mlink::clearStats();
      Serial.println("cleared");
    }
    prev = c;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  delay(1500);  // wait for USB CDC to connect (so the log can be read)
  // To erase the registration and pair again, use the serial command u (or the config command unpair)
  mlink::Config cfg;
#ifdef TEST_TASK_PRIO
  cfg.txTaskPriority = TEST_TASK_PRIO;  // for comparison: e.g. above the scan task (12)
  cfg.rxTaskPriority = TEST_TASK_PRIO;
#endif

  mlink::onMidi(onMidi);
  mlink::onMidiFast(onMidiFast);
  mlink::Transport tr = mlink::begin(cfg);

#ifndef TEST_WORKLOAD
#define TEST_WORKLOAD 1
#endif
#if TEST_WORKLOAD
  wl::begin(appSend, 12);  // scan task priority 12 (above the mimuz-link send task at 11)
#endif
  Serial.printf("ready (%s). commands: p P s c r0..r3  b=BLE e=auto u=re-pair\n", tr == mlink::TR_BLE ? "BLE-MIDI" : "ESP-NOW");
}

void loop() {
  wl::loopLoad();  // loop() load (sending and receiving do not go through loop(), so they are unaffected)
  if (rxDumpReq) {  // report the receive records to the PC (3 messages per record, sent with gaps)
    rxDumpReq = false;
    uint16_t n = rxLogN;
    for (uint16_t i = 0; i < n; i++) {
      uint32_t dt = rxLogT[i] - rxLogT[0];
      appSend(TM_RXDUMP_SEQ, rxLogSeq[i] & 0x7F, (rxLogSeq[i] >> 7) & 0x7F);
      appSend(TM_RXDUMP_T0, dt & 0x7F, (dt >> 7) & 0x7F);
      appSend(TM_RXDUMP_T1, (dt >> 14) & 0x7F, (dt >> 21) & 0x7F);
      delay(4);
    }
    appSend(TM_CTRL, 127, 127);
  }
  serialCmd();
  mlink::loop();
  delay(5);
}
