/*

 mimuz_link_client mimuz-link client side (ESP-NOW / BLE-MIDI client for XIAO ESP32-C6)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing
 - 2026/11/21 : Hand packets to the radio under the Wi-Fi driver's global lock (send_lock)
 - 2026/11/22 : Set how many times the radio retries an unacknowledged frame (retry_limit)
 - 2026/11/22 : The BLE-MIDI send task takes its priority from Config::txTaskPriority like the ESP-NOW one (was fixed at 9)

*/
// mimuz-link: communication layer for the client side (XIAO ESP32-C6, etc.)
//
// What this file handles (the platform):
//   - Registering (pairing) with and connecting to the bridge, keeping the link alive, following the bridge's channel, and searching again when the link is lost
//   - The MIDI send queue, batching, Note Off retransmission, and discarding stale Note On messages
//   - Telling the bridge which MIDI channels to listen to
//   - BLE-MIDI when the bridge cannot be reached
//   - Choosing the transport (automatic at boot: ESP-NOW if the bridge is reachable, otherwise BLE)
//   - Radio diagnostics (round-trip time measurement, statistics)
// What it does not handle (the application, i.e. the per-device sketch):
//   - Reading switches and encoders, LEDs, sound generation, interpreting MIDI messages, test messages
//
// Usage (from a sketch):
//   mlink::Config cfg;                    // set namePrefix or repair if needed
//   mlink::onMidi(handler);               // received MIDI (called from the receive task)
//   mlink::begin(cfg);                    // once in setup()
//   mlink::sendMidi(status, d1, d2);      // callable from any task
//   mlink::loop();                        // call from loop() (keeps the link alive, etc.)
//
// Header-only library. Include it from only one file of the sketch.
#pragma once
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "host/ble_hs.h"
#include <Preferences.h>
#include "mimuz_link_proto.h"
// Frequently executed code (receive, the send task, the send-complete callback, and the task that hands received MIDI to the app) is placed in internal fast memory (IRAM),
// so it does not go through the flash cache. Define MIMUZ_LINK_NO_IRAM to place these functions in flash instead, for comparison.
#ifndef MLINK_HOT
#ifndef MIMUZ_LINK_NO_IRAM
#define MLINK_HOT IRAM_ATTR
#else
#define MLINK_HOT
#endif
#endif
#include "mimuz_link_lathist.h"
#include "mimuz_link_cmd.h"

namespace mlink {

// ================= Public types =================
enum Transport { TR_ESPNOW, TR_BLE };
enum BootRequest { BOOT_AUTO, BOOT_BLE, BOOT_REPAIR };  // passed to restart(): automatic / fixed to BLE / erase the registration and pair again

struct Config {
  const char *namePrefix = "mi:muz:conESP32C6";  // device name = prefix + "-" + unique ID (11 chars)
  bool repair = false;                           // true: erase the registration and pair again (e.g. booted with a switch held down)
  uint8_t txTaskPriority = 11;                   // priority of the send task (ESP-NOW and BLE-MIDI)
  uint8_t rxTaskPriority = 10;                   // priority of the task that hands received MIDI to the app
};

// Receiver of incoming MIDI. m = 3 bytes, rxUs = time it was received over the radio (esp_timer, lower 32 bits)
typedef void (*MidiHandler)(const uint8_t m[3], uint32_t rxUs);
// Called immediately inside the radio receive callback. Returning true keeps the message from being passed to MidiHandler.
// Keep it short (e.g. recording a timestamp).
typedef bool (*MidiFastHandler)(const uint8_t m[3], uint32_t rxUs);

static MidiHandler midiHandler = nullptr;
static MidiFastHandler midiFastHandler = nullptr;
static inline void onMidi(MidiHandler h) { midiHandler = h; }
static inline void onMidiFast(MidiFastHandler h) { midiFastHandler = h; }

// ================= Internal from here on =================
#define BLE_MIDI_SVC "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define BLE_MIDI_CHR "7772E5DB-3868-4112-A1A9-F2669D106BF3"
// Requested BLE connection interval (in 1.25 ms units). Whether it is accepted depends on the PC/OS
#define BLE_ITVL_MIN 6   // 7.5ms
#define BLE_ITVL_MAX 12  // 15ms

enum Mode { MODE_ESPNOW, MODE_BLE };
#define BOOTREQ_BLE 0xB1E0B1E0u
#define BOOTREQ_ESPNOW 0xE5B0E5B0u
#define BOOTREQ_REPAIR 0x4E3A114Eu  // erase the registration and pair again (same as booting with a switch held down)
RTC_NOINIT_ATTR static uint32_t bootReq;  // survives a software reset
static Mode mode;

// ---- Queue for handing received MIDI to the app ----
struct RxEv {
  uint8_t m[3];
  uint32_t t_us;
};
static QueueHandle_t evq;
static TaskHandle_t evTaskH, pingTaskH;
static volatile uint32_t evDrop;
static inline void rxPush(uint8_t m0, uint8_t m1, uint8_t m2, uint32_t t) {
  const uint8_t m[3] = {m0, m1, m2};
  if (midiFastHandler && midiFastHandler(m, t)) return;
  RxEv e = {{m0, m1, m2}, t};
  if (xQueueSend(evq, &e, 0) != pdTRUE) evDrop++;
}

static inline uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }

// ---- Statistics ----
static LatHist hSendAck;  // esp_now_send() -> send-complete callback (until the MAC ACK is received)
static LatHist hRtt;      // radio round trip (C6 -> bridge -> C6)
static volatile uint32_t sendFail, sendErr, pingLost;
static volatile int8_t lastRssi;
static uint8_t rateIdx = ENM_RATE_DEFAULT;

// ================= ESP-NOW =================
static uint8_t peerMac[6];
static volatile bool paired = false;
static uint16_t midiSeq = 0;
static portMUX_TYPE txMux = portMUX_INITIALIZER_UNLOCKED;
// FIFO of send start times (assumes send-complete callbacks arrive in send order)
static uint32_t txStart[64];
static volatile uint8_t txHead, txTail;

static volatile uint8_t failStreak = 0;  // consecutive sends that were not delivered (reset to 0 on delivery)
static volatile uint32_t lastTxMs = 0;  // time of the last send (used to decide whether a KEEPALIVE is needed)
static TaskHandle_t c6TxTaskH;
static volatile uint8_t txDone = 0;       // result of the packet sent from the send queue: 0 = pending, 1 = delivered, 2 = not delivered
static volatile uint32_t cbTypeUnknown;  // times the packet type could not be determined in the send-complete callback
static volatile uint32_t txTimeouts;     // times the completion callback did not arrive within 100 ms
static volatile uint32_t cbMidi, cbOther, cbLateMidi;  // callback breakdown: MIDI, other types, MIDI arriving while nobody was waiting
static volatile uint8_t cbLastOtherType;
static volatile bool txWaiting = false;
// Hand one packet to the radio. Returns an error if it is not accepted (not counted; the caller decides whether to wait and resend)
static esp_err_t enmSendRaw(const uint8_t *buf, size_t len);
static esp_err_t enmSendE(const enm_pkt_t &p) { return enmSendRaw((const uint8_t *)&p, sizeof(p)); }
static esp_err_t MLINK_HOT enmSendRaw(const uint8_t *buf, size_t len) {
  if (!paired) return ESP_ERR_ESPNOW_NOT_FOUND;
  lastTxMs = millis();
  portENTER_CRITICAL(&txMux);
  uint8_t h = txHead;
  txStart[h & 63] = now_us();
  txHead = h + 1;
  portEXIT_CRITICAL(&txMux);
  esp_err_t e = enm_radio_send(peerMac, buf, len);
  if (e != ESP_OK) {
    // Not queued for sending -> no callback will come, so undo the entry (simple handling that assumes no other send happens in between)
    portENTER_CRITICAL(&txMux);
    if (txHead == (uint8_t)(h + 1)) txHead = h;
    portEXIT_CRITICAL(&txMux);
  }
  return e;
}
// Control packets (PING, CONNECT, KEEPALIVE, etc.) are sent without waiting. If not accepted, give up
static bool enmSend(const enm_pkt_t &p) {
  if (enmSendE(p) == ESP_OK) return true;
  sendErr++;
  return false;
}

// ---- MIDI send queue (holds messages when sending is congested) ----
// sendMidi() only enqueues. A dedicated send task passes messages to the radio in order; when they are not accepted, it waits and resends instead of dropping them.
#define C6_TXQ_LEN 128
static QueueHandle_t c6Txq;
static volatile uint32_t c6TxRetry, c6TxDrop, c6TxqPeak;
static volatile uint32_t c6TxPackets, c6TxMsgs, c6TxBatchMax;
static volatile uint32_t pktFail, offRetried, offRecovered, offGiveUp;  // Note Off retransmission statistics
static volatile uint16_t noteOnMaxAgeMs = ENM_NOTEON_MAX_AGE_MS;         // 0 = never discard
static volatile uint32_t noteOnExpired;                                 // number of Note On messages discarded as stale

// ---- Detecting and restarting a stalled radio driver ----
static volatile uint32_t stallRestarts, wifiRestarts;
static volatile bool debugStallFlag = false;  // diagnostic: discard send-complete callbacks to simulate a stall (cleared by the restart)
static uint32_t lastRestartMs = 0;
static uint8_t txTimeoutStreak = 0;
static uint16_t noMemStreak = 0;
static void onSent(const esp_now_send_info_t *info, esp_now_send_status_t st);
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int n);
static uint8_t stallChannel();
// Radio retry limit (see enm_retry_set). 0 = the library's value (32). Applied whenever ESP-NOW starts or restarts
static uint8_t retryLimit = 0;
// Restart ESP-NOW (and Wi-Fi too if withWifi = true). Restores the link to the bridge (peer registration, rate, channel)
static void radioRestart(bool withWifi) {
  Serial.println(withWifi ? "radio stalled again: restarting Wi-Fi and ESP-NOW" : "radio stalled: restarting ESP-NOW");
  enm_radio_block();
  esp_now_deinit();
  if (withWifi) {
    esp_wifi_stop();
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  uint8_t ch = stallChannel();
  if (ch >= 1 && ch <= 13) esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);
  enm_radio_unblock();
  enm_retry_set(retryLimit);
  if (paired) {
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, peerMac, 6);
    pi.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&pi);
    enm_apply_rate(peerMac, rateIdx);
  }
  portENTER_CRITICAL(&txMux);
  txHead = txTail = 0;
  portEXIT_CRITICAL(&txMux);
  debugStallFlag = false;
  txTimeoutStreak = 0;
  noMemStreak = 0;
  lastRestartMs = millis();
  if (withWifi) wifiRestarts++;
  else stallRestarts++;
}
static void stallHandle() {
  uint32_t since = millis() - lastRestartMs;
  if (lastRestartMs && since < ENM_STALL_BACKOFF_MS) return;  // do not restart repeatedly in quick succession
  radioRestart(lastRestartMs && since < ENM_STALL_WIFI_WINDOW_MS);
}

// Hand one packet to the radio and wait for the send to finish. Returns true if delivered (ACK received)
static bool MLINK_HOT c6SendWait(const uint8_t *buf, size_t len) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, 0);  // clear stale notifications
    txDone = 0;
    txWaiting = true;
    esp_err_t e = enmSendRaw(buf, len);
    if (e == ESP_OK) {
      uint32_t t0 = millis();
      while (!txDone && millis() - t0 < 100) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
      txWaiting = false;
      if (!txDone) {  // no completion callback (delivery unknown). If this keeps happening, the driver is stalled
        txTimeouts++;
        if (++txTimeoutStreak >= ENM_STALL_TIMEOUTS) stallHandle();
      } else {
        txTimeoutStreak = 0;
      }
      return txDone == 1;
    }
    if (e != ESP_ERR_ESPNOW_NO_MEM) { sendErr++; return false; }
    c6TxRetry++;
    if (++noMemStreak >= 1000) {  // not accepted for 1 second -> stalled
      noMemStreak = 0;
      stallHandle();
      return false;
    }
    vTaskDelay(1);  // full with other sends (PING, etc.). Wait 1 ms and resend
  }
}

static void MLINK_HOT c6TxTask(void *) {
  static uint8_t buf[ENM_HDR_LEN + 5 * ENM_RETRY_MAX > ENM_HDR_LEN + 3 * ENM_BATCH_MAX ? ENM_HDR_LEN + 5 * ENM_RETRY_MAX : ENM_HDR_LEN + 3 * ENM_BATCH_MAX];
  // Note Off messages from undelivered packets (waiting to be resent)
  static struct { uint16_t seq; uint8_t m[3]; } rq[ENM_RETRY_MAX];
  uint8_t rqN = 0;
  uint32_t rqSince = 0;
  enm_pkt_t p;
  for (;;) {
    if (rqN) {
      // Do the retransmission first (it goes out before new messages, so the order is preserved)
      enm_pkt_t h;
      enm_init(h, PKT_MIDI_RETRY);
      h.len = rqN;
      h.t_us = now_us();
      memcpy(buf, &h, ENM_HDR_LEN);
      for (uint8_t k = 0; k < rqN; k++) {
        uint8_t *q = buf + ENM_HDR_LEN + 5 * k;
        q[0] = rq[k].seq & 0xFF;
        q[1] = rq[k].seq >> 8;
        memcpy(q + 2, rq[k].m, 3);
      }
      if (c6SendWait(buf, ENM_HDR_LEN + 5 * rqN)) {
        offRecovered += rqN;
        rqN = 0;
      } else if (millis() - rqSince > ENM_RETRY_GIVEUP_MS) {
        offGiveUp += rqN;
        rqN = 0;
      } else {
        vTaskDelay(2);
      }
      continue;
    }
    if (xQueueReceive(c6Txq, &p, portMAX_DELAY) != pdTRUE) continue;
    // Combine what is waiting in the queue into one packet (using the header of the first entry)
    // Note On messages that waited too long in the queue are discarded here (p.t_us is the enqueue time)
    uint8_t n = 0;
    enm_pkt_t h = p;
    uint32_t nowUs = now_us(), maxAgeUs = (uint32_t)noteOnMaxAgeMs * 1000;
    do {
      if (maxAgeUs && enm_is_note_on(p.midi) && (uint32_t)(nowUs - p.t_us) > maxAgeUs) {
        noteOnExpired++;
        continue;
      }
      if (n == 0) h = p;
      memcpy(buf + ENM_HDR_LEN + 3 * n, p.midi, 3);
      n++;
    } while (n < ENM_BATCH_MAX && xQueueReceive(c6Txq, &p, 0) == pdTRUE);
    if (n == 0) continue;  // all discarded
    h.len = n;
    uint16_t seq0 = midiSeq;  // sequence numbers are assigned here, where the send order is fixed
    h.seq = seq0;
    midiSeq += n;
    h.t_us = now_us();  // time it is actually handed to the radio
    memcpy(buf, &h, ENM_HDR_LEN);
    // One packet at a time: after handing a packet to the radio, wait for that send to finish before building the next one.
    // Messages that arrive while waiting go into the next packet.
    bool ok = c6SendWait(buf, ENM_HDR_LEN + 3 * n);
    c6TxPackets++;
    c6TxMsgs += n;
    if (n > c6TxBatchMax) c6TxBatchMax = n;
    if (!ok) {
      // Not delivered. Pick out only the Note Off messages for retransmission (others are not resent)
      pktFail++;
      for (uint8_t k = 0; k < n && rqN < ENM_RETRY_MAX; k++) {
        const uint8_t *m = buf + ENM_HDR_LEN + 3 * k;
        if (enm_is_note_off(m)) {
          rq[rqN].seq = (uint16_t)(seq0 + k);
          memcpy(rq[rqN].m, m, 3);
          rqN++;
        }
      }
      // buf is overwritten by the retransmission packet after this, so the extraction is done here
      if (rqN) {
        offRetried += rqN;
        rqSince = millis();
      }
    }
  }
}

static void MLINK_HOT onSent(const esp_now_send_info_t *info, esp_now_send_status_t st) {
  if (debugStallFlag) return;
  uint32_t t = now_us();
  portENTER_CRITICAL(&txMux);
  bool have = txTail != txHead;
  uint32_t t0 = have ? txStart[txTail & 63] : 0;
  if (have) txTail++;
  portEXIT_CRITICAL(&txMux);
  if (st != ESP_NOW_SEND_SUCCESS) {
    sendFail++;
    if (failStreak < 255) failStreak++;
  } else {
    failStreak = 0;
    if (have) hSendAck.add(t - t0);
  }
  // Notify the send task only of results for packets sent from the send queue (MIDI and its retransmissions)
  uint8_t ty = enm_cb_type(info);
  if (!ty) cbTypeUnknown++;
  if (!ty || ty == PKT_MIDI || ty == PKT_MIDI_RETRY) {
    cbMidi++;
    if (!txWaiting) cbLateMidi++;
    txDone = st == ESP_NOW_SEND_SUCCESS ? 1 : 2;
    if (c6TxTaskH) xTaskNotifyGive(c6TxTaskH);
  } else {
    cbOther++;
    cbLastOtherType = ty;
  }
}

static volatile uint16_t pingWaitSeq = 0xFFFF;
// MIDI channels to listen to (the bridge forwards messages on these channels that it received from other C6 devices)
// bit0 = channel 1 ... bit15 = channel 16. 0xFFFF = OMNI, 0 = none.
static volatile uint16_t rxChMask = 0;
static volatile uint8_t rxSubFlags = 0;     // ENM_SUB_SELF = also receive messages this device sent (for testing)
static volatile bool subDirty = false;      // changed, so the bridge needs to be told
// Sequence number of received MIDI (to detect duplicates from retransmission)
static uint16_t rxLastSeq = 0;
static bool rxHaveSeq = false;
static volatile uint32_t retryAccepted, retryDup;
// The same packet can arrive twice (when the peer misses the ACK and the radio layer resends). If the sequence number,
// send time and count all match the previous packet, drop it as a duplicate
static uint16_t rxPktSeq = 0;
static uint32_t rxPktTus = 0;
static uint8_t rxPktCnt = 0;
static volatile uint32_t rxPktDup = 0;

// ---- Relayed config commands (received via the bridge) ----
static char relayLine[ENM_CMD_TEXT_MAX + 1];
static volatile bool relayPending = false;
// Handling relay retransmissions: if the same command (same sequence number and time) arrives again, do not execute it; send the previous reply again
static uint16_t relaySeq = 0, relayDoneSeq = 0;
static uint32_t relayTus = 0, relayDoneTus = 0;
static bool relayHaveDone = false;
static uint16_t relayReplyLen = 0;
static uint8_t cmdPendingRadioRestart = 0;  // 0 = none, 1 = ESP-NOW only, 2 = Wi-Fi too (done after the reply is sent)
static uint8_t cmdPendingRestart = 0;  // 0 = none, otherwise BootRequest + 1 (restart after the reply is sent)

// ---- Registration and connection (proto.h v2) ----
static Preferences prefs;
static uint8_t bridgeCh = 0;
static uint8_t stallChannel() { return bridgeCh; }
static volatile bool connected = false;
static volatile int8_t myPort = -1;     // port assigned by the bridge (0-based)
static volatile uint8_t hsType = 0;     // type of reply received (0 = none yet)
static volatile uint16_t hsAux = 0, hsSeq = 0;
static volatile uint8_t pendingCh = 0;   // channel the bridge announced it is moving to
static volatile int8_t hsRssi = 0;
static uint8_t hsMac[6];
static volatile uint8_t rtReject = 0;   // reason of a REJECT received during operation

static void MLINK_HOT onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int n) {
  uint32_t t = now_us();
  if (!enm_valid(d, n)) return;
  enm_pkt_t p;
  memcpy(&p, d, sizeof(p));
  lastRssi = info->rx_ctrl->rssi;
  switch (p.type) {
    case PKT_PAIR_ACK:
    case PKT_CONNECT_OK:
    case PKT_REJECT:
      // Replies to the boot-time handshake. Read by the waiting side (setup)
      memcpy(hsMac, info->src_addr, 6);
      hsAux = p.aux;
      hsSeq = p.seq;
      hsRssi = info->rx_ctrl->rssi;
      hsType = p.type;
      if (p.type == PKT_REJECT && connected) rtReject = p.aux;  // rejected during operation
      break;
    case PKT_CH_SWITCH:
      if (p.aux >= 1 && p.aux <= 13) pendingCh = (uint8_t)p.aux;
      break;
    case PKT_PONG:
      if (p.seq == pingWaitSeq && pingTaskH) xTaskNotify(pingTaskH, t - p.t_us, eSetValueWithOverwrite);
      break;
    case PKT_MIDI: {  // arrives batched; the 4th byte is the count
      uint8_t cnt = d[3];
      if (cnt < 1 || cnt > ENM_BATCH_MAX || n < ENM_HDR_LEN + 3 * cnt) break;
      if (rxPktCnt && p.seq == rxPktSeq && p.t_us == rxPktTus && cnt == rxPktCnt) { rxPktDup++; break; }  // duplicate
      rxPktSeq = p.seq; rxPktTus = p.t_us; rxPktCnt = cnt;
      for (uint8_t k = 0; k < cnt; k++) {
        const uint8_t *m = d + ENM_HDR_LEN + 3 * k;
        rxPush(m[0], m[1], m[2], t);
      }
      rxLastSeq = (uint16_t)(p.seq + cnt - 1);  // last sequence number received so far
      rxHaveSeq = true;
      break;
    }
    case PKT_MIDI_RETRY: {  // retransmitted Note Off messages that were not delivered. Accept only those not received yet
      uint8_t cnt = d[3];
      if (cnt < 1 || cnt > ENM_RETRY_MAX || n < ENM_HDR_LEN + 5 * cnt) break;
      bool any = false;
      uint16_t newest = rxLastSeq;
      for (uint8_t k = 0; k < cnt; k++) {
        const uint8_t *q = d + ENM_HDR_LEN + 5 * k;
        uint16_t sq = q[0] | (q[1] << 8);
        if (rxHaveSeq && !enm_seq_newer(sq, rxLastSeq)) { retryDup++; continue; }  // already received
        rxPush(q[2], q[3], q[4], t);
        retryAccepted++;
        if (!any || enm_seq_newer(sq, newest)) newest = sq;
        any = true;
      }
      if (any) { rxLastSeq = newest; rxHaveSeq = true; }
      break;
    }
    case PKT_CMD: {  // config command relayed by the bridge. Only stored here; executed in loop()
      uint8_t len = d[3];
      if (len < 1 || len > ENM_CMD_TEXT_MAX || n < ENM_HDR_LEN + len || relayPending) break;
      memcpy(relayLine, d + ENM_HDR_LEN, len);
      relayLine[len] = 0;
      relaySeq = p.seq;
      relayTus = p.t_us;
      relayPending = true;
      break;
    }
    default: break;
  }
}

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void espnowInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (esp_now_init() != ESP_OK) Serial.println("esp_now_init failed");
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);
  enm_radio_unblock();
  enm_retry_set(retryLimit);
}
static void espnowStop() {
  enm_radio_block();
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  paired = false;
}

static bool bridgeLoad() {
  bridgeCh = prefs.getUChar("bch", 0);
  return bridgeCh >= 1 && bridgeCh <= 13 && prefs.getBytes("bmac", peerMac, 6) == 6;
}
static void bridgeSave(const uint8_t *mac, uint8_t ch) {
  prefs.putBytes("bmac", mac, 6);
  prefs.putUChar("bch", ch);
  memcpy(peerMac, mac, 6);
  bridgeCh = ch;
}
static void bridgeForget() {
  prefs.remove("bmac");
  prefs.remove("bch");
  bridgeCh = 0;
}

// Wait for a reply. Returns the type if type1 or type2 arrives, 0 on timeout
static uint8_t waitReply(uint8_t type1, uint8_t type2, uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    uint8_t t = hsType;
    if (t == type1 || t == type2) return t;
    delay(2);
  }
  return 0;
}

// Pairing: cycle through channels 1-13 looking for a bridge close by
static bool pairScan() {
  Serial.println("pairing: searching for a bridge nearby (ch 1-13)...");
  esp_now_peer_info_t bp = {};
  memcpy(bp.peer_addr, BCAST, 6);
  bp.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&bp);
  bool found = false;
  for (int round = 0; round < 2 && !found; round++) {
    for (uint8_t ch = 1; ch <= 13 && !found; ch++) {
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      hsType = 0;
      enm_pkt_t p;
      enm_init(p, PKT_PAIR_REQ);
      for (int k = 0; k < 2; k++) {
        p.t_us = now_us();
        enm_radio_send(BCAST, (const uint8_t *)&p, sizeof(p));
        delay(15);
      }
      if (waitReply(PKT_PAIR_ACK, PKT_PAIR_ACK, 100)) {
        // A neighboring channel can leak through, so use the channel written in the reply
        uint8_t bch = (uint8_t)hsAux;
        if (hsRssi >= ENM_PAIR_RSSI_MIN && bch >= 1 && bch <= 13) {
          bridgeSave(hsMac, bch);
          Serial.printf("pairing: registered with bridge %02X:%02X:%02X:%02X:%02X:%02X ch=%d (rssi %d, heard on ch %d)\n", hsMac[0],
                        hsMac[1], hsMac[2], hsMac[3], hsMac[4], hsMac[5], bch, hsRssi, ch);
          found = true;
        } else {
          Serial.printf("pairing: ignored a reply (rssi %d, ch %d)\n", hsRssi, bch);
        }
      }
    }
  }
  esp_now_del_peer(BCAST);
  if (!found) Serial.println("pairing: no bridge found nearby");
  return found;
}

// Send CONNECT on the given channel and wait for a reply. Returns 1 = connected, 0 = no reply, negative = rejected (-ENM_REJECT_*)
static int tryConnectOn(uint8_t ch, uint32_t waitMs) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  hsType = 0;
  enm_pkt_t p;
  enm_init(p, PKT_CONNECT);
  p.aux = rateIdx;
  p.seq = rxChMask;   // mask of MIDI channels to listen to
  p.len = rxSubFlags;
  p.t_us = now_us();
  enmSend(p);
  uint8_t r = waitReply(PKT_CONNECT_OK, PKT_REJECT, waitMs);
  if (r == PKT_REJECT) return -(int)hsAux;
  if (r != PKT_CONNECT_OK) return 0;
  // A neighboring channel can leak through, so use the channel written in the reply
  uint8_t real = (uint8_t)hsSeq;
  if (real < 1 || real > 13) real = ch;
  if (real != ch) esp_wifi_set_channel(real, WIFI_SECOND_CHAN_NONE);
  if (real != bridgeCh) {
    Serial.printf("bridge moved: ch %d -> %d (saved)\n", bridgeCh, real);
    prefs.putUChar("bch", real);
    bridgeCh = real;
  }
  myPort = (int8_t)hsAux;
  rxHaveSeq = false;  // the bridge's sequence numbers start from 0 on every connection
  rxPktCnt = 0;
  failStreak = 0;
  connected = true;
  Serial.printf("connected to bridge %02X:%02X:%02X:%02X:%02X:%02X ch=%d -> Port %d\n", peerMac[0], peerMac[1], peerMac[2], peerMac[3],
                peerMac[4], peerMac[5], bridgeCh, myPort + 1);
  return 1;
}

// Connect: announce to the saved bridge. If there is no reply on the saved channel, search all channels
// Returns 1 = connected, 0 = no reply, negative = rejected
static int bridgeConnect() {
  if (!paired) {
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, peerMac, 6);
    pi.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&pi) != ESP_OK) return 0;
    enm_apply_rate(peerMac, rateIdx);
    paired = true;
  }
  uint8_t saved = bridgeCh;
  for (int k = 0; k < 2; k++) {
    int r = tryConnectOn(saved, 200);
    if (r) return r;
  }
  Serial.printf("bridge does not answer on ch %d, searching ch 1-13...\n", saved);
  for (int round = 0; round < 3; round++) {
    for (uint8_t ch = 1; ch <= 13; ch++) {
      int r = tryConnectOn(ch, 70);
      if (r) return r;
    }
  }
  esp_wifi_set_channel(saved, WIFI_SECOND_CHAN_NONE);
  return 0;
}

// Choose the transport at boot: ESP-NOW if the bridge is reachable, otherwise BLE
//   repair = true: erase the registration and pair again
static Mode decideTransport(bool repair) {
  espnowInit();
  Serial.printf("MAC=%s\n", WiFi.macAddress().c_str());
  bool have = bridgeLoad();
  if (repair && have) {
    Serial.println("re-pair requested: registration erased");
    bridgeForget();
    have = false;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!have) {
      have = pairScan();
      if (!have) break;
    }
    int r = bridgeConnect();
    if (r == 1) return MODE_ESPNOW;
    if (r == -ENM_REJECT_UNREGISTERED) {  // the bridge has lost the registration -> erase ours too and pair again from the start
      Serial.println("bridge says: not registered -> registration erased");
      bridgeForget();
      have = false;
      continue;
    }
    Serial.println(r == -ENM_REJECT_FULL ? "bridge says: no free port" : "bridge does not answer");
    break;
  }
  Serial.println("-> BLE-MIDI");
  espnowStop();
  return MODE_BLE;
}

// Radio RTT measurement task: send PING and time until PONG with the same clock (RTT/2 ~ one-way)
static QueueHandle_t pingReq;
static void pingTask(void *) {
  for (;;) {
    uint32_t count;
    xQueueReceive(pingReq, &count, portMAX_DELAY);
    if (!connected) { Serial.println("not connected to a bridge"); continue; }
    hRtt.reset();
    pingLost = 0;
    uint32_t failBefore = sendFail;
    Serial.printf("radio ping x%lu  rate=%s ...\n", (unsigned long)count, enm_rate_name(rateIdx));
    for (uint32_t i = 0; i < count; i++) {
      enm_pkt_t p;
      enm_init(p, PKT_PING);
      p.seq = (uint16_t)i;
      xTaskNotifyStateClear(NULL);
      pingWaitSeq = p.seq;
      p.t_us = now_us();
      uint32_t rtt;
      if (enmSend(p) && xTaskNotifyWait(0, UINT32_MAX, &rtt, pdMS_TO_TICKS(50)) == pdTRUE) hRtt.add(rtt);
      else pingLost++;
      pingWaitSeq = 0xFFFF;
      if (count >= 10 && (i + 1) % (count / 10) == 0) Serial.printf("  %lu%%\n", (unsigned long)((i + 1) * 100 / count));
      vTaskDelay(1 + (esp_random() % 3));  // random interval of 1-3 ms
    }
    Serial.println("---- radio RTT result ----");
    hRtt.print(Serial, "RTT       ");
    hRtt.print(Serial, "RTT/2(片道)", 2);
    Serial.printf("lost=%lu/%lu (%.4f%%)  MAC送信失敗(再送後)=%lu  rssi=%d dBm\n", (unsigned long)pingLost,
                  (unsigned long)count, 100.0 * pingLost / count, (unsigned long)(sendFail - failBefore), lastRssi);
  }
}

// ================= BLE-MIDI =================
static char devName[32];
static char devUid[12];
static BLECharacteristic *bleChr;
static volatile bool bleConn = false;
#define BLE_REPROBE_MS 20000          // while waiting on BLE, search for the bridge again at this interval
static bool bleForced = false;        // fixed to BLE via serial 'b'
static bool bleHaveBridge = false;    // a bridge is registered
static uint32_t bleIdleSince = 0;
static volatile uint16_t bleItvl = 0, bleItvlReq = 0;
static volatile bool bleItvlChanged = false;

static void bleParse(const uint8_t *b, size_t n) {
  // BLE-MIDI: [header][ts][status][data..][ts][status]... with running status
  if (n < 3 || !(b[0] & 0x80)) return;
  uint32_t t = now_us();
  size_t i = 1;
  uint8_t rs = 0;
  while (i < n) {
    if (b[i] & 0x80) {  // timestamp
      i++;
      if (i >= n) break;
      if (b[i] & 0x80) {
        uint8_t st = b[i++];
        if (st >= 0xF8) {  // real-time (clock, etc.): 1 byte. Running status is unchanged
          if (midi_len(st)) rxPush(st, 0, 0, t);
          continue;
        }
        if (st == 0xF0) {  // skip SysEx
          while (i < n && b[i] != 0xF7) i++;
          rs = 0;
          continue;
        }
        if (st >= 0xF0) {  // other system messages (song position, etc.). Running status is cleared
          rs = 0;
          uint8_t SL = midi_len(st);
          if (!SL || i + (SL - 1) > n) continue;
          rxPush(st, SL >= 2 ? b[i] : 0, SL == 3 ? b[i + 1] : 0, t);
          i += SL - 1;
          continue;
        }
        rs = st;
      }
    }
    if (!rs) { i++; continue; }
    uint8_t L = midi_len(rs);
    if (i + (L - 1) > n) break;
    rxPush(rs, b[i], (uint8_t)(L == 3 ? b[i + 1] : 0), t);
    i += L - 1;
  }
}

class ChrCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override { bleParse(c->getData(), c->getLength()); }
};

// ---- BLE send queue (batched sending) ----
// sendMidi() only enqueues. bleTxTask packs as many messages as fit in the MTU into one BLE-MIDI packet and sends a notify.
// If the notify fails (e.g. out of buffers), wait 2 ms and resend the same content.
struct BleMsg {
  uint8_t m[3];
  uint8_t len;
  uint16_t ts;  // 13bit ms
};
#define BLE_Q 256
static BleMsg bleQ[BLE_Q];
static volatile uint16_t bleQHead = 0, bleQTail = 0;
static portMUX_TYPE bleQMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t bleConnHandle = 0, bleMtu = 23;
static volatile uint32_t bleRetry = 0, bleDrop = 0, blePk = 0, bleMsgs = 0;
static volatile uint32_t bleConnMs = 0;
static volatile bool bleParamReq = false;
static BLEServer *bleSrv;
static TaskHandle_t bleTxTaskH;

static void bleEnqueue(uint8_t s0, uint8_t d1, uint8_t d2, uint8_t L) {
  BleMsg m = {{s0, d1, d2}, L, (uint16_t)(millis() & 0x1FFF)};
  portENTER_CRITICAL(&bleQMux);
  uint16_t nh = (bleQHead + 1) % BLE_Q;
  if (nh == bleQTail) bleDrop++;
  else { bleQ[bleQHead] = m; bleQHead = nh; }
  portEXIT_CRITICAL(&bleQMux);
  if (bleTxTaskH) xTaskNotifyGive(bleTxTaskH);
}

// Send pacing: at most one notify per connection interval. Messages arriving in between go into the next packet.
// (Sending too many notifies piles up small packets inside the BLE stack and the delay accumulates.)
static void bleTxTask(void *) {
  uint8_t buf[256];
  uint32_t lastTx = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    while (bleConn) {
      uint32_t itvlMs = (bleItvl * 5 + 3) / 4;  // 1.25 ms units -> ms (rounded up)
      if (itvlMs < 1) itvlMs = 1;
      uint32_t el = millis() - lastTx;
      if (el < itvlMs) vTaskDelay(pdMS_TO_TICKS(itvlMs - el));
      portENTER_CRITICAL(&bleQMux);
      uint16_t t = bleQTail, h = bleQHead;
      portEXIT_CRITICAL(&bleQMux);
      if (t == h) break;
      size_t maxp = bleMtu > 3 ? bleMtu - 3 : 20;
      if (maxp > sizeof(buf)) maxp = sizeof(buf);
      uint8_t hi = (bleQ[t].ts >> 7) & 0x3F;
      size_t n = 0;
      buf[n++] = 0x80 | hi;
      uint16_t i = t, taken = 0;
      while (i != h) {
        const BleMsg &m = bleQ[i];
        if (((m.ts >> 7) & 0x3F) != hi) break;   // a change in the header's upper bits starts a new packet
        if (n + 1 + m.len > maxp) break;
        buf[n++] = 0x80 | (m.ts & 0x7F);
        memcpy(buf + n, m.m, m.len);
        n += m.len;
        taken++;
        i = (i + 1) % BLE_Q;
      }
      if (!taken) {  // not even one fits (should not happen; just in case) -> drop it
        portENTER_CRITICAL(&bleQMux);
        bleQTail = (bleQTail + 1) % BLE_Q;
        portEXIT_CRITICAL(&bleQMux);
        bleDrop++;
        continue;
      }
      struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
      int rc = om ? ble_gatts_notify_custom(bleConnHandle, bleChr->getHandle(), om) : BLE_HS_ENOMEM;
      if (rc == 0) {
        lastTx = millis();
        portENTER_CRITICAL(&bleQMux);
        bleQTail = i;
        portEXIT_CRITICAL(&bleQMux);
        blePk++;
        bleMsgs += taken;
      } else {
        bleRetry++;
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
  }
}

class SrvCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s, ble_gap_conn_desc *d) override {
    bleConnHandle = d->conn_handle;
    bleItvl = d->conn_itvl;
    bleItvlChanged = true;
    bleConnMs = millis();
    bleParamReq = true;  // a request right after connecting may be ignored, so loop() sends it 1.5 s later
    bleConn = true;
  }
  void onDisconnect(BLEServer *, ble_gap_conn_desc *) override {
    bleConn = false;
    bleMtu = 23;
    BLEDevice::startAdvertising();
  }
  void onMtuChanged(BLEServer *, ble_gap_conn_desc *, uint16_t mtu) override { bleMtu = mtu; }
  void onConnParamsUpdate(uint16_t, uint16_t itvl, uint16_t, uint16_t, uint8_t) override {
    bleItvl = itvl;
    bleItvlChanged = true;
  }
};

static void bleBegin() {
  // device name = prefix + "-" + unique ID (11 chars) = 29 chars (the limit that fits in the BLE scan response)
  BLEDevice::init(devName);
  BLEServer *srv = BLEDevice::createServer();
  bleSrv = srv;
  srv->setCallbacks(new SrvCB());
  BLEService *svc = srv->createService(BLE_MIDI_SVC);
  bleChr = svc->createCharacteristic(BLE_MIDI_CHR, BLECharacteristic::PROPERTY_READ |
                                                       BLECharacteristic::PROPERTY_WRITE_NR |
                                                       BLECharacteristic::PROPERTY_NOTIFY);
  bleChr->setCallbacks(new ChrCB());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_MIDI_SVC);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("BLE-MIDI mode. name=%s  Mac: Audio MIDI設定 > MIDIスタジオ > Bluetooth で接続\n", devName);
}

// ================= Public functions =================

// Send one MIDI message. Callable from any task (only puts it on the send queue).
//   aux    : arbitrary value (e.g. input-to-send time [us]; shows up in the bridge's log)
//   waitMs : time to wait when the queue is full. Use 0 for tasks that must not block (e.g. a 1 ms scan)
// Returns true if enqueued (false when not connected over BLE)
static bool MLINK_HOT sendMidi(uint8_t s, uint8_t d1, uint8_t d2, uint16_t aux = 0, uint32_t waitMs = 0) {
  uint8_t L = midi_len(s);
  if (!L) return false;  // messages that are not carried (SysEx, etc.)
  if (L < 3) d2 = 0;
  if (L < 2) d1 = 0;
  if (mode == MODE_ESPNOW) {
    enm_pkt_t p;
    enm_init(p, PKT_MIDI);
    p.len = L;
    p.aux = aux;
    p.t_us = now_us();  // enqueue time (the send task uses it to judge how long it waited; overwritten with the send time when sent)
    p.midi[0] = s; p.midi[1] = d1; p.midi[2] = d2;
    bool ok = xQueueSend(c6Txq, &p, pdMS_TO_TICKS(waitMs)) == pdTRUE;
    if (!ok) c6TxDrop++;
    UBaseType_t depth = uxQueueMessagesWaiting(c6Txq);
    if (depth > c6TxqPeak) c6TxqPeak = depth;
    return ok;
  }
  if (!bleConn) return false;
  bleEnqueue(s, d1, d2, L);
  return true;
}

// Task that hands received MIDI to the app's handler
static void MLINK_HOT evTask(void *) {
  RxEv e;
  for (;;) {
    if (xQueueReceive(evq, &e, portMAX_DELAY) != pdTRUE) continue;
    if (midiHandler) midiHandler(e.m, e.t_us);
  }
}

// ---- Status ----
static inline Transport transport() { return mode == MODE_ESPNOW ? TR_ESPNOW : TR_BLE; }
static inline const char *name() { return devName; }
static inline bool isConnected() { return mode == MODE_ESPNOW ? (bool)connected : (bool)bleConn; }
static inline int port() { return myPort; }                 // port assigned by the bridge (0-based, -1 when not connected)
static inline uint8_t radioChannel() { return bridgeCh; }
static inline uint8_t rate() { return rateIdx; }
static inline uint16_t rxChannels() { return rxChMask; }
static inline uint16_t bleIntervalUnits() { return bleItvl; }  // BLE connection interval (in 1.25 ms units)
static inline uint16_t bleMtuSize() { return bleMtu; }
static inline uint32_t bleRetryCount() { return bleRetry; }
static inline uint32_t bleDropCount() { return bleDrop; }

// ---- Settings ----
// MIDI channels to listen to (bit0 = channel 1 ... bit15 = channel 16. 0xFFFF = OMNI, 0 = none). Changes are sent to the bridge
static inline void setRxChannels(uint16_t mask, uint8_t flags = 0) {
  rxChMask = mask;
  rxSubFlags = flags;
  subDirty = true;
}
static inline uint8_t rxFlags() { return rxSubFlags; }
// time [ms] before a stale Note On is discarded (0 = never)
static inline void setNoteOnMaxAgeMs(uint16_t ms) { noteOnMaxAgeMs = ms; }
static inline uint16_t noteOnMaxAge() { return noteOnMaxAgeMs; }
// PHY rate (0 = 1M, 1 = 6M, 2 = 24M, 3 = MCS4). Also sent to the bridge
static void setRate(uint8_t idx) {
  if (idx >= ENM_RATE_NUM) return;
  rateIdx = idx;
  if (paired) {
    enm_apply_rate(peerMac, rateIdx);
    enm_pkt_t p;
    enm_init(p, PKT_CFG);
    p.aux = rateIdx;
    enmSend(p);
  }
}
// Restart. BOOT_AUTO = choose the transport automatically, BOOT_BLE = fixed to BLE, BOOT_REPAIR = erase the registration and pair again
static void restart(BootRequest r) {
  Serial.flush();
  bootReq = r == BOOT_BLE ? BOOTREQ_BLE : r == BOOT_REPAIR ? BOOTREQ_REPAIR : BOOTREQ_ESPNOW;
  delay(100);
  esp_restart();
}

// ---- Diagnostics ----
// Measure the radio round-trip time count times (ESP-NOW only). Results go to Serial
static bool radioPing(uint32_t count) {
  if (mode != MODE_ESPNOW) return false;
  return xQueueSend(pingReq, &count, 0) == pdTRUE;
}

static void printStats(Print &o) {
  o.printf("name=%s mode=%s rate=%s connected=%d port=%d ch=%d rssi=%d sendErr=%lu macFail=%lu evDrop=%lu\n", devName,
           mode == MODE_ESPNOW ? "ESP-NOW" : "BLE", enm_rate_name(rateIdx), (int)connected, myPort + 1, bridgeCh, lastRssi,
           (unsigned long)sendErr, (unsigned long)sendFail, (unsigned long)evDrop);
  if (mode == MODE_BLE)
    o.printf("BLE conn=%d interval=%.2fms mtu=%u packets=%lu msgs=%lu retry=%lu drop=%lu\n", bleConn, bleItvl * 1.25, bleMtu,
             (unsigned long)blePk, (unsigned long)bleMsgs, (unsigned long)bleRetry, (unsigned long)bleDrop);
  o.printf("MIDI tx: msgs=%lu packets=%lu maxBatch=%lu noMemRetry=%lu dropped=%lu queuePeak=%lu/%d\n", (unsigned long)c6TxMsgs,
           (unsigned long)c6TxPackets, (unsigned long)c6TxBatchMax, (unsigned long)c6TxRetry, (unsigned long)c6TxDrop,
           (unsigned long)c6TxqPeak, C6_TXQ_LEN);
  o.printf("NoteOff retry: pktFail=%lu retried=%lu recovered=%lu gaveUp=%lu | rx: accepted=%lu dup=%lu | cbTypeUnknown=%lu\n",
           (unsigned long)pktFail, (unsigned long)offRetried, (unsigned long)offRecovered, (unsigned long)offGiveUp,
           (unsigned long)retryAccepted, (unsigned long)retryDup, (unsigned long)cbTypeUnknown);
  o.printf("tx done cb: midi=%lu other=%lu (last other type=%u) lateMidi=%lu timeouts=%lu | txStart fifo head=%u tail=%u\n",
           (unsigned long)cbMidi, (unsigned long)cbOther, cbLastOtherType, (unsigned long)cbLateMidi, (unsigned long)txTimeouts,
           (unsigned)txHead, (unsigned)txTail);
  o.printf("radio stall: espnowRestarts=%lu wifiRestarts=%lu | duplicate packets dropped=%lu\n", (unsigned long)stallRestarts, (unsigned long)wifiRestarts, (unsigned long)rxPktDup);
  o.printf("NoteOn max age=%ums expired=%lu | receive channels: mask=0x%04X flags=%u\n", noteOnMaxAgeMs, (unsigned long)noteOnExpired,
           rxChMask, rxSubFlags);
  if (mode == MODE_ESPNOW) hSendAck.print(o, "send API->MAC ACK");
}

static void clearStats() {
  hSendAck.reset();
  sendFail = 0; sendErr = 0; evDrop = 0; c6TxRetry = 0; c6TxDrop = 0; c6TxqPeak = 0; c6TxPackets = 0; c6TxMsgs = 0; c6TxBatchMax = 0;
  pktFail = 0; offRetried = 0; offRecovered = 0; offGiveUp = 0; retryAccepted = 0; retryDup = 0; rxPktDup = 0; noteOnExpired = 0;
}

// ---- Saving and loading settings (non-volatile memory) ----
// Saved: MIDI channels to listen to, stale Note On discard time, PHY rate
static void saveSettings() {
  prefs.putUShort("rxmask", rxChMask);
  prefs.putUShort("age", noteOnMaxAgeMs);
  prefs.putUChar("rate", rateIdx);
}
static void loadSettings() {
  rxChMask = prefs.getUShort("rxmask", 0);
  noteOnMaxAgeMs = prefs.getUShort("age", ENM_NOTEON_MAX_AGE_MS);
  uint8_t r = prefs.getUChar("rate", ENM_RATE_DEFAULT);
  rateIdx = r < ENM_RATE_NUM ? r : ENM_RATE_DEFAULT;
}
// Reset settings to their defaults (the registration is kept)
static void resetSettings() {
  prefs.remove("rxmask");
  prefs.remove("age");
  prefs.remove("rate");
  loadSettings();
  subDirty = true;
  setRate(rateIdx);
}

// ---- Config commands (called from mimuz_link_cmd.h) ----
static void cmdPrintSettings(Print &o) {
  o.printf(" rx_channels=0x%04X noteon_max_age_ms=%u phy_rate=%u", rxChMask, noteOnMaxAgeMs, rateIdx);
}
static void cmdExec(const Cmd &c, Print &o) {
  if (cmdIs(c, "info")) {
    o.printf("@%s ok name=%s kind=client fw=%s transport=%s connected=%d port=%d radio_channel=%u bridge=%02X:%02X:%02X:%02X:%02X:%02X\n", c.id,
             devName, MIMUZ_LINK_VERSION, mode == MODE_ESPNOW ? "espnow" : "ble", (int)isConnected(), myPort + 1, bridgeCh, peerMac[0],
             peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  } else if (cmdIs(c, "get")) {
    if (c.argc == 1) {
      o.printf("@%s ok", c.id);
      cmdPrintSettings(o);
      o.print("\n");
    } else if (!strcmp(c.argv[1], "rx_channels")) o.printf("@%s ok rx_channels=0x%04X\n", c.id, rxChMask);
    else if (!strcmp(c.argv[1], "noteon_max_age_ms")) o.printf("@%s ok noteon_max_age_ms=%u\n", c.id, noteOnMaxAgeMs);
    else if (!strcmp(c.argv[1], "phy_rate")) o.printf("@%s ok phy_rate=%u\n", c.id, rateIdx);
    else if (!strcmp(c.argv[1], "send_lock")) o.printf("@%s ok send_lock=%u\n", c.id, enmSendLock ? 1 : 0);
    else if (!strcmp(c.argv[1], "retry_limit"))
      o.printf("@%s ok retry_limit=%u short=%u long=%u default_short=%u default_long=%u\n", c.id, retryLimit, enm_retry_short(),
               enm_retry_long(), enmRetryDefShort, enmRetryDefLong);
    else cmdErr(c, o, "unknown_key");
  } else if (cmdIs(c, "set")) {
    uint32_t v;
    if (c.argc < 3) { cmdErr(c, o, "usage:set_<key>_<value>"); return; }
    if (!strcmp(c.argv[1], "rx_channels")) {
      // on/off for each of the 16 channels (bit0 = channel 1). omni = all, none = none
      if (!strcmp(c.argv[2], "omni")) v = 0xFFFF;
      else if (!strcmp(c.argv[2], "none")) v = 0;
      else if (!cmdUint(c.argv[2], v) || v > 0xFFFF) { cmdErr(c, o, "bad_value"); return; }
      setRxChannels((uint16_t)v, rxSubFlags);
      saveSettings();
      o.printf("@%s ok rx_channels=0x%04X\n", c.id, rxChMask);
    } else if (!strcmp(c.argv[1], "noteon_max_age_ms")) {
      if (!cmdUint(c.argv[2], v) || v > 2000) { cmdErr(c, o, "bad_value"); return; }
      setNoteOnMaxAgeMs((uint16_t)v);
      saveSettings();
      o.printf("@%s ok noteon_max_age_ms=%u\n", c.id, noteOnMaxAgeMs);
    } else if (!strcmp(c.argv[1], "send_lock")) {  // diagnostic: 1 = hand packets to the radio under the driver's lock (not saved)
      if (!cmdUint(c.argv[2], v)) { cmdErr(c, o, "bad_value"); return; }
      enmSendLock = v != 0;
      o.printf("@%s ok send_lock=%u\n", c.id, enmSendLock ? 1 : 0);
    } else if (!strcmp(c.argv[1], "retry_limit")) {  // radio retry limit, 0 = the library's value (not saved)
      if (!cmdUint(c.argv[2], v) || v > 31) { cmdErr(c, o, "bad_value"); return; }
      retryLimit = (uint8_t)v;
      enm_retry_set(retryLimit);
      o.printf("@%s ok retry_limit=%u short=%u long=%u\n", c.id, retryLimit, enm_retry_short(), enm_retry_long());
    } else if (!strcmp(c.argv[1], "phy_rate")) {
      if (!cmdUint(c.argv[2], v) || v >= ENM_RATE_NUM) { cmdErr(c, o, "bad_value"); return; }
      setRate((uint8_t)v);
      saveSettings();
      o.printf("@%s ok phy_rate=%u\n", c.id, rateIdx);
    } else cmdErr(c, o, "unknown_key");
  } else if (cmdIs(c, "radio_restart")) {  // diagnostic: restart ESP-NOW (with wifi, Wi-Fi too)
    if (mode != MODE_ESPNOW) { cmdErr(c, o, "not_espnow"); return; }
    if (c.argc > 1 && !strcmp(c.argv[1], "stall")) {  // simulate a stall; it should be detected and restarted automatically
      debugStallFlag = true;
      o.printf("@%s ok simulating_stall\n", c.id);
      return;
    }
    cmdPendingRadioRestart = (c.argc > 1 && !strcmp(c.argv[1], "wifi")) ? 2 : 1;
    o.printf("@%s ok restarting_radio\n", c.id);
  } else if (cmdIs(c, "reset")) {  // reset settings to their defaults
    resetSettings();
    o.printf("@%s ok", c.id);
    cmdPrintSettings(o);
    o.print("\n");
  } else if (cmdIs(c, "unpair")) {  // erase the registration with the bridge and restart
    bridgeForget();
    cmdPendingRestart = BOOT_AUTO + 1;
    o.printf("@%s ok restarting\n", c.id);
  } else if (cmdIs(c, "restart")) {  // restart [auto|ble]
    cmdPendingRestart = (c.argc > 1 && !strcmp(c.argv[1], "ble") ? BOOT_BLE : BOOT_AUTO) + 1;
    o.printf("@%s ok restarting\n", c.id);
  } else cmdErr(c, o, "unknown_command");
}
static void cmdAfter() {
  if (cmdPendingRadioRestart) {
    bool wifi = cmdPendingRadioRestart == 2;
    cmdPendingRadioRestart = 0;
    radioRestart(wifi);
  }
  if (!cmdPendingRestart) return;
  BootRequest r = (BootRequest)(cmdPendingRestart - 1);
  cmdPendingRestart = 0;
  restart(r);
}

// ---- Start ----
// Call once from setup(). Chooses the transport and starts the send/receive tasks. Returns the chosen transport
static Transport begin(const Config &cfg) {
  uint32_t req = bootReq;
  bootReq = 0;
  prefs.begin("enm", false);
  loadSettings();  // load saved settings (MIDI channels to listen to, PHY rate, etc.)
  bool repair = cfg.repair || req == BOOTREQ_REPAIR;
  enm_make_uid(devUid);
  snprintf(devName, sizeof(devName), "%s-%s", cfg.namePrefix, devUid);
  Serial.printf("\n%s\n", devName);
  // The transport is chosen automatically: ESP-NOW if the bridge is reachable, otherwise BLE (fixed to BLE after restart(BOOT_BLE))
  mode = req == BOOTREQ_BLE ? MODE_BLE : decideTransport(repair);
  bleForced = req == BOOTREQ_BLE;
  bleHaveBridge = prefs.getUChar("bch", 0) >= 1;
  bleIdleSince = millis();

  evq = xQueueCreate(192, sizeof(RxEv));  // large enough not to overflow even with consecutive batched arrivals (up to 64 each)
  pingReq = xQueueCreate(2, sizeof(uint32_t));
  if (mode == MODE_BLE) bleBegin();
  c6Txq = xQueueCreate(C6_TXQ_LEN, sizeof(enm_pkt_t));
  if (mode == MODE_ESPNOW) xTaskCreate(c6TxTask, "mlink_tx", 4096, NULL, cfg.txTaskPriority, &c6TxTaskH);
  xTaskCreate(evTask, "mlink_rx", 4096, NULL, cfg.rxTaskPriority, &evTaskH);
  if (mode == MODE_ESPNOW) xTaskCreate(pingTask, "mlink_ping", 4096, NULL, 9, &pingTaskH);
  else xTaskCreate(bleTxTask, "mlink_ble", 4096, NULL, cfg.txTaskPriority, &bleTxTaskH);
  return transport();
}

// ---- Periodic processing ----
// Call from loop() (every few ms). Keeps the link alive, follows channel changes, searches again when the link is lost, etc.
static void loop() {
  if (mode == MODE_ESPNOW && connected) {
    // Execute a config command relayed by the bridge and send the reply
    if (relayPending) {
      static uint8_t rbuf[ENM_HDR_LEN + ENM_CMD_TEXT_MAX + 2];
      char *text = (char *)rbuf + ENM_HDR_LEN;
      BufPrint bp(text, ENM_CMD_TEXT_MAX + 1);
      char *line = relayLine;
      if (*line == '@') line++;
      bool again = relayHaveDone && relaySeq == relayDoneSeq && relayTus == relayDoneTus;  // the same command arrived again via retransmission
      size_t len;
      if (again) {
        len = relayReplyLen;  // send back the previous reply still in rbuf as is (without executing)
      } else {
        cmdRunLine(line, bp);
        len = bp.length();
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;  // do not send the trailing newline
        enm_pkt_t h;
        enm_init(h, PKT_CMD_REPLY);
        h.len = (uint8_t)len;
        memcpy(rbuf, &h, ENM_HDR_LEN);
        relayReplyLen = (uint16_t)len;
        relayDoneSeq = relaySeq;
        relayDoneTus = relayTus;
        relayHaveDone = true;
      }
      if (enmSendRaw(rbuf, ENM_HDR_LEN + len) != ESP_OK) sendErr++;
      relayPending = false;
      if (!again) {
        delay(30);  // wait for the reply to go out
        cmdAfter();
      }
    }
    // Listen channels changed -> tell the bridge
    if (subDirty) {
      subDirty = false;
      enm_pkt_t sp;
      enm_init(sp, PKT_SUBSCRIBE);
      sp.aux = rxChMask;
      sp.len = rxSubFlags;
      enmSend(sp);
      Serial.printf("receive channels -> mask=0x%04X flags=%u\n", rxChMask, rxSubFlags);
    }
    // The bridge announced a channel change -> follow it
    uint8_t pc = pendingCh;
    if (pc == bridgeCh) pendingCh = pc = 0;  // the announcement arrives twice for safety; ignore the second one
    if (pc) {
      pendingCh = 0;
      delay(40);  // give the bridge a moment to finish moving
      esp_wifi_set_channel(pc, WIFI_SECOND_CHAN_NONE);
      prefs.putUChar("bch", pc);
      Serial.printf("bridge announced channel change: ch %d -> %d (saved)\n", bridgeCh, pc);
      bridgeCh = pc;
      failStreak = 0;
    }
    // Consecutive undelivered sends mean the bridge is gone or changed channel -> search again
    if (failStreak >= 3) {
      Serial.println("link lost (no ACK). searching for the bridge...");
      connected = false;
      int r = bridgeConnect();
      if (r == -ENM_REJECT_UNREGISTERED) rtReject = ENM_REJECT_UNREGISTERED;
      if (r != 1) {
        Serial.println(r == 0 ? "bridge not found. retrying..." : "bridge rejected the connection");
        failStreak = 3;  // search again on the next loop()
        connected = true;  // keep sending (failStreak returns to 0 once delivered)
        delay(500);
      }
    }
    // Keepalive. Right after a failed delivery, send at a shorter interval to notice sooner
    if (millis() - lastTxMs >= (failStreak ? 300u : (uint32_t)ENM_KEEPALIVE_MS)) {
      enm_pkt_t p;
      enm_init(p, PKT_KEEPALIVE);
      p.t_us = now_us();
      enmSend(p);
    }
    uint8_t rj = rtReject;
    if (rj) {
      rtReject = 0;
      if (rj == ENM_REJECT_UNREGISTERED) {  // the bridge removed the registration -> erase ours too and restart
        Serial.println("bridge says: not registered -> registration erased, restarting");
        Serial.flush();
        bridgeForget();
        delay(100);
        esp_restart();
      } else {
        Serial.println("bridge says: no free port");
      }
    }
  }
  // Even while waiting on BLE, search for the bridge occasionally if one is registered.
  // (The bridge takes about 12 s to boot. This lets the C6 connect over ESP-NOW later even if it booted first.)
  // No search while connected to something over BLE, or when fixed to BLE via serial 'b'.
  if (mode == MODE_BLE && !bleConn && !bleForced && bleHaveBridge && millis() - bleIdleSince > BLE_REPROBE_MS) {
    Serial.println("BLE idle: looking for the bridge again...");
    Serial.flush();
    bootReq = BOOTREQ_ESPNOW;
    delay(50);
    esp_restart();
  }
  if (mode == MODE_BLE && bleConn) bleIdleSince = millis();
  if (mode == MODE_BLE && bleConn && bleParamReq && millis() - bleConnMs > 1500) {
    bleParamReq = false;
    bool ok = bleSrv->requestConnParams(bleConnHandle, BLE_ITVL_MIN, BLE_ITVL_MAX, 0, 400);
    Serial.printf("BLE conn param request %.2f-%.2fms -> %s\n", BLE_ITVL_MIN * 1.25, BLE_ITVL_MAX * 1.25, ok ? "sent" : "failed");
  }
  if (bleItvlChanged) {
    bleItvlChanged = false;
    Serial.printf("BLE connected=%d interval=%.2fms (要求 %.2f-%.2fms)\n", bleConn, bleItvl * 1.25,
                  BLE_ITVL_MIN * 1.25, BLE_ITVL_MAX * 1.25);
  }
}

}  // namespace mlink
