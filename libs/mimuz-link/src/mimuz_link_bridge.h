/*

 mimuz_link_bridge mimuz-link bridge side (ESP-NOW to USB-MIDI bridge for XIAO ESP32-S3)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing
 - 2026/11/21 : Send to clients in parallel by default (tx_parallel=1)
 - 2026/11/21 : Escalate radio recovery (driver reinit, device restart); report heap
 - 2026/11/21 : Reinitialize the driver at once when it refuses every packet; restart the device when memory runs low
 - 2026/11/21 : Keep the channel and the port numbers across a restart caused by a stalled radio
 - 2026/11/21 : Hand packets to the radio under the Wi-Fi driver's global lock (send_lock)
 - 2026/11/22 : Trace how long marked messages spend in the bridge and on the radio (trace_status, trace)
 - 2026/11/22 : Record radio sends whose result came late (slow_tx_ms, slowtx)
 - 2026/11/22 : Set how many times the radio retries an unacknowledged frame (retry_limit, 8 by default)

*/
// mimuz-link: communication part of the bridge (XIAO ESP32-S3)
//
// To the PC it appears as a composite device: USB-MIDI "<prefix>-<unique ID>" (fixed 8 ports) plus CDC serial.
//
// What this file handles (platform):
//   - Fixed 8 USB-MIDI ports (one port per client)
//   - Client registration (saved in non-volatile memory), connection and port assignment, port release
//   - Choosing a free radio channel (measured at startup) and switching channels while running
//   - PC -> client send queue, batching, Note Off resending, dropping stale Note On
//   - Client -> PC forwarding (including waiting for USB writes)
//   - Routing by receive channel (messages from a client go to other clients listening on that channel)
//   - Radio diagnostics (PING replies, statistics, logging)
// What it does not handle (application = sketch):
//   - Handling of test messages, serial commands
//
// Usage (from a sketch):
//   #define MIMUZ_BRIDGE_NAME_PREFIX "mi:muz:bridgeESPS3"   // optional (place before the include)
//   #include <mimuz_link_bridge.h>
//   mlink::onUsbMidi(handler);   // optional: inspect MIDI from the PC first (return true to not send it to clients)
//   mlink::begin();              // once in setup() (channel survey takes about 12 s)
//   mlink::loop();               // call from loop()
//
// Board settings: USB Mode = USB-OTG (TinyUSB), USB CDC On Boot = Enabled
// Header-only library. Include it from only one file of the sketch.
#pragma once
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <new>
// Frequently executed code (send task, receive handling, USB forwarding, send-complete callback) is placed in internal fast memory (IRAM).
// This bypasses the flash cache and shortens the round trip by about 0.15 ms.
// Define MIMUZ_LINK_NO_IRAM to place it in flash instead, for comparison.
#ifndef MIMUZ_LINK_NO_IRAM
#define MLINK_HOT IRAM_ATTR
#else
#define MLINK_HOT
#endif
#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include <Preferences.h>
#include "mimuz_link_proto.h"
#include "mimuz_link_lathist.h"
#include "mimuz_link_cmd.h"

#if ARDUINO_USB_MODE
#error "USB Mode を USB-OTG (TinyUSB) にしてください"
#endif

#ifndef MIMUZ_BRIDGE_NAME_PREFIX
#define MIMUZ_BRIDGE_NAME_PREFIX "mi:muz:bridgeESPS3"
#endif
// macOS remembers the port layout per VID/PID + serial number. Experimental PID (VID stays Seeed's; a product needs an official VID/PID)
#ifndef MIMUZ_BRIDGE_USB_PID
#define MIMUZ_BRIDGE_USB_PID 0x8057
#endif

namespace mlink {

// ================= Public types =================
// Inspects MIDI from the PC (USB) before it is sent to clients. port is 0-based. Return true to not send that message to clients.
// Called from the USB receive task.
typedef bool (*UsbMidiHandler)(uint8_t port, const uint8_t m[3]);
// Decides whether MIDI from a client is logged (one serial line). Return true to log. If not set, everything is logged only in verbose mode.
typedef bool (*MidiLogFilter)(const uint8_t m[3]);

static UsbMidiHandler usbMidiHandler = nullptr;
static MidiLogFilter midiLogFilter = nullptr;
static inline void onUsbMidi(UsbMidiHandler h) { usbMidiHandler = h; }
static inline void setMidiLogFilter(MidiLogFilter f) { midiLogFilter = f; }

}  // namespace mlink

// ================= Internal from here on =================
// ---- USB-MIDI: fixed 8 ports (USB-MIDI "cables") ----
// One port (slot) is assigned per C6. The number and names of ports are fixed, so swapping C6 units does not change what the OS sees.
// The standard Arduino USBMIDI class supports only one port, so it is not used; the descriptor is registered here directly.
#define NUM_PORTS 8
// Product name = prefix + "-" + unique ID (11 chars). Multiple bridges connected at once can be told apart by name (same approach as the RP2040 version of web-pio).
// The same unique ID is used as the serial number.
#define USB_NAME_PREFIX MIMUZ_BRIDGE_NAME_PREFIX
static char usbProductName[40];
static char usbUid[12];
#define USB_PID_EXPERIMENT MIMUZ_BRIDGE_USB_PID
#define MP_DESC_LEN (TUD_MIDI_DESC_HEAD_LEN + NUM_PORTS * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(NUM_PORTS))

extern "C" uint16_t mp_load_descriptor(uint8_t *dst, uint8_t *itf) {
  static const char *names[NUM_PORTS] = {"Port 1", "Port 2", "Port 3", "Port 4", "Port 5", "Port 6", "Port 7", "Port 8"};
  uint8_t sDev = tinyusb_add_string_descriptor(usbProductName);
  uint8_t s[NUM_PORTS];
  for (int i = 0; i < NUM_PORTS; i++) s[i] = tinyusb_add_string_descriptor(names[i]);
  uint8_t epIn = tinyusb_get_free_in_endpoint();
  uint8_t epOut = tinyusb_get_free_out_endpoint();
  TU_VERIFY(epIn != 0 && epOut != 0);
  uint8_t d[MP_DESC_LEN] = {
    TUD_MIDI_DESC_HEAD(*itf, sDev, NUM_PORTS),
    TUD_MIDI_DESC_JACK_DESC(1, s[0]), TUD_MIDI_DESC_JACK_DESC(2, s[1]), TUD_MIDI_DESC_JACK_DESC(3, s[2]), TUD_MIDI_DESC_JACK_DESC(4, s[3]),
    TUD_MIDI_DESC_JACK_DESC(5, s[4]), TUD_MIDI_DESC_JACK_DESC(6, s[5]), TUD_MIDI_DESC_JACK_DESC(7, s[6]), TUD_MIDI_DESC_JACK_DESC(8, s[7]),
    TUD_MIDI_DESC_EP(epOut, CFG_TUD_ENDPOINT_SIZE, NUM_PORTS),
    TUD_MIDI_JACKID_IN_EMB(1), TUD_MIDI_JACKID_IN_EMB(2), TUD_MIDI_JACKID_IN_EMB(3), TUD_MIDI_JACKID_IN_EMB(4),
    TUD_MIDI_JACKID_IN_EMB(5), TUD_MIDI_JACKID_IN_EMB(6), TUD_MIDI_JACKID_IN_EMB(7), TUD_MIDI_JACKID_IN_EMB(8),
    TUD_MIDI_DESC_EP((uint8_t)(0x80 | epIn), CFG_TUD_ENDPOINT_SIZE, NUM_PORTS),
    TUD_MIDI_JACKID_OUT_EMB(1), TUD_MIDI_JACKID_OUT_EMB(2), TUD_MIDI_JACKID_OUT_EMB(3), TUD_MIDI_JACKID_OUT_EMB(4),
    TUD_MIDI_JACKID_OUT_EMB(5), TUD_MIDI_JACKID_OUT_EMB(6), TUD_MIDI_JACKID_OUT_EMB(7), TUD_MIDI_JACKID_OUT_EMB(8),
  };
  *itf += 2;
  memcpy(dst, d, MP_DESC_LEN);
  return MP_DESC_LEN;
}
// With CDC On Boot enabled, USB starts before setup(), so the descriptor is registered during global variable initialization
struct MpRegister {
  MpRegister() {
    tinyusb_enable_interface(USB_INTERFACE_MIDI, MP_DESC_LEN, mp_load_descriptor);
    enm_make_uid(usbUid);
    snprintf(usbProductName, sizeof(usbProductName), "%s-%s", USB_NAME_PREFIX, usbUid);
    USB.PID(USB_PID_EXPERIMENT);
    USB.productName(usbProductName);
    USB.serialNumber(usbUid);
  }
};
static MpRegister mpRegister;

namespace mlink {

static inline uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }
static uint8_t wifiChannel = ENM_CHANNEL;

// ---- Event log (queued from the receive callback etc., printed in loop()) ----
struct EvLog { char s[96]; };
static QueueHandle_t evlogq;
static void evlog(const char *fmt, ...) {
  EvLog e;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.s, sizeof(e.s), fmt, ap);
  va_end(ap);
  if (evlogq) xQueueSend(evlogq, &e, 0);
}
#define MACSTR_ "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC2STR_(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

// ---- Registry (saved in non-volatile memory; independent of ports) ----
#define REG_MAX 64  // when exceeded, the oldest entry is overwritten
static Preferences prefs;
static uint8_t regMac[REG_MAX][6];
static uint8_t regCount = 0, regNext = 0;
static volatile bool regDirty = false;

static int regFind(const uint8_t *mac) {
  for (int i = 0; i < regCount; i++)
    if (memcmp(regMac[i], mac, 6) == 0) return i;
  return -1;
}
static void regAdd(const uint8_t *mac) {
  if (regFind(mac) >= 0) return;
  int idx = regCount < REG_MAX ? regCount++ : regNext;
  regNext = (idx + 1) % REG_MAX;
  memcpy(regMac[idx], mac, 6);
  regDirty = true;  // saved in loop() (do not write non-volatile memory inside the receive callback)
}
static void regLoad() {
  regCount = prefs.getUChar("n", 0);
  regNext = prefs.getUChar("next", 0);
  if (regCount > REG_MAX) regCount = 0;
  if (regCount) prefs.getBytes("mac", regMac, regCount * 6);
}
static void regSave() {
  prefs.putUChar("n", regCount);
  prefs.putUChar("next", regNext);
  if (regCount) prefs.putBytes("mac", regMac, regCount * 6);
  else prefs.remove("mac");
}

// ---- Ports (assigned to connected C6 units, lowest free first) ----
struct Port {
  volatile bool active;
  uint8_t mac[6];
  volatile uint32_t lastRxMs;
  uint8_t rate;
  uint16_t txSeq;
  volatile uint8_t failStreak;  // consecutive delivery failures (reset to 0 when anything arrives)
  uint32_t probeMs;             // time of the last probe send while unreachable
  uint16_t probeGap;            // interval until the next probe (0 = not given up on)
  int8_t rssi;                  // signal strength of the last received packet [dBm]
  volatile uint32_t txFailCnt;  // packets to this client not delivered even after radio-layer retries
  volatile uint16_t rxChMask;   // MIDI channels this C6 listens to (bit0 = ch1; 0 = none)
  volatile uint8_t subFlags;    // ENM_SUB_SELF = also receive its own messages (for testing)
  uint16_t rxLastSeq;           // last sequence number received so far (to detect duplicate resends)
  bool rxHaveSeq;
  uint16_t rxPktSeq;            // sequence number, send time and count of the previous packet (to detect duplicates from radio-layer retries)
  uint32_t rxPktTus;
  uint8_t rxPktCnt;
  // Statistics (used by fwdTask)
  bool haveSeq, haveOff;
  uint16_t lastSeq;
  uint32_t minOff;
};
static Port ports[NUM_PORTS];

static int MLINK_HOT portFind(const uint8_t *mac) {
  for (int i = 0; i < NUM_PORTS; i++)
    if (ports[i].active && memcmp(ports[i].mac, mac, 6) == 0) return i;
  return -1;
}
static bool addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t pi = {};
  memcpy(pi.peer_addr, mac, 6);
  pi.ifidx = WIFI_IF_STA;
  return esp_now_add_peer(&pi) == ESP_OK;
}
// Ports held for the clients that were connected before a restart caused by a stalled radio, so that each comes back on
// the same port number (and the computer keeps routing it to the same place)
#define PORT_KEEP_MS 30000
static uint8_t keptMac[NUM_PORTS][6];
static bool keptAny = false;
static bool portKept(int k) {
  if (!keptAny) return false;
  if (millis() > PORT_KEEP_MS) { keptAny = false; return false; }
  static const uint8_t zero[6] = {0};
  return memcmp(keptMac[k], zero, 6) != 0;
}
// Assign the port held for this client, or else the lowest-numbered free port. Returns -1 if full
static int portAssign(const uint8_t *mac, uint8_t rate) {
  int i = portFind(mac);
  if (i < 0) {
    for (int k = 0; k < NUM_PORTS && i < 0; k++)
      if (!ports[k].active && portKept(k) && !memcmp(keptMac[k], mac, 6)) i = k;
    for (int k = 0; k < NUM_PORTS && i < 0; k++)
      if (!ports[k].active && !portKept(k)) i = k;
    for (int k = 0; k < NUM_PORTS && i < 0; k++)  // all free ports are held: give up holding one
      if (!ports[k].active) i = k;
    if (i < 0) return -1;
    if (!addPeer(mac)) return -1;
    memcpy(ports[i].mac, mac, 6);
    ports[i].txSeq = 0;
    ports[i].failStreak = 0;
    ports[i].rxChMask = 0;
    ports[i].subFlags = 0;
    ports[i].rxHaveSeq = false;
    ports[i].rxPktCnt = 0;
    ports[i].txFailCnt = 0;
    ports[i].haveSeq = ports[i].haveOff = false;
    ports[i].lastRxMs = millis();
    ports[i].active = true;
    evlog("port %d <- " MACSTR_ " (connected)", i + 1, MAC2STR_(mac));
  }
  if (rate < ENM_RATE_NUM) {
    ports[i].rate = rate;
    enm_apply_rate(mac, rate);
  }
  return i;
}
static void portFree(int i, const char *why) {
  if (!ports[i].active) return;
  ports[i].active = false;
  esp_now_del_peer(ports[i].mac);
  evlog("port %d freed (%s) " MACSTR_, i + 1, why, MAC2STR_(ports[i].mac));
}

// Registration and reject replies go to MACs not yet added as peers; add them temporarily to send
static uint8_t tmpMac[6];
static bool tmpValid = false;
static void sendTo(const uint8_t *mac, const enm_pkt_t &p) {
  if (!esp_now_is_peer_exist(mac)) {
    if (tmpValid && portFind(tmpMac) < 0) esp_now_del_peer(tmpMac);
    tmpValid = addPeer(mac);
    memcpy(tmpMac, mac, 6);
  }
  enm_radio_send(mac, (const uint8_t *)&p, sizeof(p));
}
static void reply(const uint8_t *mac, uint8_t type, uint16_t aux, uint16_t seq = 0) {
  enm_pkt_t r;
  enm_init(r, type);
  r.aux = aux;
  r.seq = seq;
  r.t_us = now_us();
  sendTo(mac, r);
}

struct RxItem {
  uint8_t m[3];
  uint8_t len;
  uint8_t port;
  uint16_t seq;
  uint16_t aux;  // C6: button -> send [us]
  int8_t rssi;
  uint32_t t_cb;
  uint32_t t_send;  // C6 send time (C6 clock)
};
static QueueHandle_t rxq, logq;
static TaskHandle_t usbInTaskH;

// ---- Message trace (diagnostic) ----
// When traceStatus is not 0, each message from the PC whose status byte equals it is stamped when it arrives over USB,
// keyed by its port and its two data bytes. When the client on that port sends the same message back, the bridge records
// the time from the USB arrival until the reply has been written to USB, and the part of it until the reply arrived over
// the radio. Subtracting the first from the round trip measured on the PC gives the time spent on the PC and USB side.
// The TRACE_TOP slowest are kept with their keys so that the PC can match them with its own slowest round trips.
// The tables (about 10 KB of internal memory) are allocated only while tracing is on, so that they do not reduce the
// memory left for restarting the radio driver.
#define TRACE_SLOTS 32
#define TRACE_TOP 16
struct TraceSlot { uint32_t tIn; uint16_t key; bool used; };
struct TraceTop { uint32_t inUs, totalUs, radioUs; uint16_t key; uint8_t port; };
struct TraceData {
  TraceSlot slot[NUM_PORTS][TRACE_SLOTS];
  LatHist h;  // USB arrival of a traced message -> USB write of its reply complete
};
static volatile uint8_t traceStatus = 0;
static TraceData *volatile trace = nullptr;
static TraceTop traceTop[TRACE_TOP];
static uint8_t traceTopN;
static volatile uint32_t traceDoneN, traceMiss;
static inline uint16_t traceKey(const uint8_t *m) { return (uint16_t)(m[1] | (m[2] << 7)); }
static void MLINK_HOT traceIn(uint8_t port, const uint8_t *m) {
  TraceData *td = trace;
  if (!td) return;
  uint16_t k = traceKey(m);
  TraceSlot &s = td->slot[port][k % TRACE_SLOTS];
  s.key = k;
  s.tIn = now_us();
  s.used = true;
}
static void traceOut(const RxItem &it, uint32_t tDone) {
  TraceData *td = trace;
  if (!td) return;
  uint16_t k = traceKey(it.m);
  TraceSlot &s = td->slot[it.port][k % TRACE_SLOTS];
  if (!s.used || s.key != k) { traceMiss++; return; }
  s.used = false;
  TraceTop t = {s.tIn, tDone - s.tIn, it.t_cb - s.tIn, k, it.port};
  td->h.add(t.totalUs);
  traceDoneN++;
  if (traceTopN < TRACE_TOP) { traceTop[traceTopN++] = t; return; }
  int lo = 0;
  for (int i = 1; i < TRACE_TOP; i++) if (traceTop[i].totalUs < traceTop[lo].totalUs) lo = i;
  if (t.totalUs > traceTop[lo].totalUs) traceTop[lo] = t;
}
static void traceClear() {
  TraceData *td = trace;
  if (td) {
    memset(td->slot, 0, sizeof(td->slot));
    td->h.reset();
  }
  traceTopN = 0;
  traceDoneN = traceMiss = 0;
}
// Turns tracing on (status byte) or off (0). Returns false if the tables could not be allocated
static bool traceSet(uint8_t st) {
  traceStatus = 0;
  if (!st) {
    TraceData *td = trace;
    trace = nullptr;
    vTaskDelay(pdMS_TO_TICKS(5));  // let a task that is inside traceIn / traceOut finish
    delete td;
    traceClear();
    return true;
  }
  if (!trace) trace = new (std::nothrow) TraceData();
  if (!trace) return false;
  traceClear();
  traceStatus = st;
  return true;
}

// ---- PC -> C6 send queue (callers wait when sending is congested) ----
struct TxItem {
  uint8_t port;
  uint8_t mac[6];
  enm_pkt_t p;
};
#define TXQ_LEN 256
// Packets for a client that failed this many times in a row are dropped from the send queue.
// While dropping, a single packet is still sent occasionally as a probe; if it is delivered, the client is reinstated
// (a receive-only client rarely transmits on its own, so without probing it would never be reinstated).
// The probe interval starts at 250 ms and doubles up to 1 s, because each send to a client that has really gone away
// holds up the other clients by about 10 ms of radio-layer retries
#define TX_FAIL_STREAK_MAX 3
#define TX_PROBE_FIRST_MS 250
#define TX_PROBE_MAX_MS 1000
static volatile uint8_t txFailMax = TX_FAIL_STREAK_MAX;  // adjustable for comparison (tx_fail_max; not saved)
static volatile bool txProbe = true;                      // for comparison (tx_probe; not saved)
static inline bool txSkipPeer(volatile uint8_t &streak, uint32_t &probeMs, uint16_t &probeGap) {
  if (streak < txFailMax) { probeGap = 0; return false; }
  if (!txProbe) return true;
  uint32_t ms = millis();
  if (!probeGap) { probeGap = TX_PROBE_FIRST_MS; probeMs = ms; return true; }  // start giving up
  if (ms - probeMs >= probeGap) {  // send a probe
    probeMs = ms;
    probeGap = probeGap * 2 > TX_PROBE_MAX_MS ? TX_PROBE_MAX_MS : probeGap * 2;
    return false;
  }
  return true;
}
static QueueHandle_t txq;
static TaskHandle_t txTaskH;
static volatile uint32_t txSent, txRetry, txErr, txFail, txSkipped, txWaits, txqPeak;

// stats
static LatHist hCb2Usb;  // ESP-NOW receive callback -> USB write complete
static LatHist hRel;     // relative delay of the radio segment (minimum = 0)
static uint32_t rxMidi, seqGap, usbFail, rxqDrop, rejects;
static volatile uint32_t usbRetry;
static int32_t rssiSum;
static uint32_t rssiN;
static bool verbose = false;
static volatile uint32_t busRouted, busDrop;  // routing by receive channel: number queued, number dropped because the queue was full
static volatile uint16_t noteOnMaxAgeMs = ENM_NOTEON_MAX_AGE_MS;  // 0 = never drop
static volatile uint32_t rxPktDup;
static volatile uint32_t retryAccepted, retryDup, cbTypeUnknown;        // receiver-side statistics (C6 -> PC)
static uint32_t deafCount = 0;

struct LogItem {
  uint8_t m[3];
  uint8_t port;
  uint16_t seq, aux;
  int8_t rssi;
  uint32_t cb2usb;
  uint32_t rel;  // relative delay of the radio segment [us] (fastest = 0)
};

// Routing by receive channel: queue messages from the C6 on port src to other C6 units listening on that channel.
// Called from the receive callback, so it never waits when the queue is full (drops and counts instead).
// ---- Relaying configuration commands (replies from clients) ----
static char relayReply[ENM_CMD_TEXT_MAX + 1];
static volatile bool relayReplyReady = false;
static char relayId[16];             // ID of the command awaiting a reply (empty = not waiting)
static uint32_t relayDeadline = 0;
// Relay resend: until a reply arrives, resend the same packet (same sequence number and time) every RELAY_RESEND_MS.
// The client does not run the same command twice (it identifies it by sequence number and time and returns the previous reply again)
#define RELAY_RESEND_MS 150
#define RELAY_TIMEOUT_MS 1500
static uint8_t relayBuf[ENM_HDR_LEN + ENM_CMD_TEXT_MAX + 2];
static uint16_t relayLen = 0, relaySeq = 0;
static uint8_t relayMac[6];
static uint32_t relayLastSendMs = 0;
static volatile uint32_t relayResends = 0;
// ---- Settings (saved in non-volatile memory) ----
static uint8_t chMin = 1, chMax = 13;  // allowed range of radio channels

static void MLINK_HOT busRoute(uint8_t src, const uint8_t *m) {
  if (!midi_len(m[0])) return;  // not carried (SysEx etc.)
  // Channel messages go to clients listening on that channel.
  // Messages without a channel (clock etc.) go to every client listening to anything
  uint16_t bit = m[0] < 0xF0 ? (uint16_t)(1u << (m[0] & 0x0F)) : 0xFFFF;
  for (uint8_t j = 0; j < NUM_PORTS; j++) {
    if (!ports[j].active || !(ports[j].rxChMask & bit)) continue;
    if (j == src && !(ports[j].subFlags & ENM_SUB_SELF)) continue;  // do not send back to the sender
    TxItem it;
    it.port = j;
    memcpy(it.mac, ports[j].mac, 6);
    enm_init(it.p, PKT_MIDI);
    it.p.len = midi_len(m[0]);
    it.p.t_us = now_us();
    memcpy(it.p.midi, m, 3);
    if (xQueueSend(txq, &it, 0) == pdTRUE) {
      busRouted++;
      if (txTaskH) xTaskNotifyGive(txTaskH);
    }
    else busDrop++;
  }
}

static void MLINK_HOT onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int n) {
  uint32_t t = now_us();
  if (!enm_valid(d, n)) return;
  enm_pkt_t p;
  memcpy(&p, d, sizeof(p));
  const uint8_t *src = info->src_addr;
  int8_t rssi = info->rx_ctrl->rssi;
  static uint32_t lastWeakMs = 0, lastRejectMs = 0;

  if (p.type == PKT_PAIR_REQ) {  // registration: accept only C6 units very close by
    if (rssi < ENM_PAIR_RSSI_MIN) {
      if (millis() - lastWeakMs > 1000) {
        lastWeakMs = millis();
        evlog("pair request ignored (rssi %d < %d) " MACSTR_, rssi, ENM_PAIR_RSSI_MIN, MAC2STR_(src));
      }
      return;
    }
    bool isNew = regFind(src) < 0;
    regAdd(src);
    reply(src, PKT_PAIR_ACK, wifiChannel);
    evlog("%s " MACSTR_ " (rssi %d)", isNew ? "registered" : "already registered", MAC2STR_(src), rssi);
    return;
  }
  if (p.type == PKT_CONNECT) {
    if (regFind(src) < 0) {
      reply(src, PKT_REJECT, ENM_REJECT_UNREGISTERED);
      evlog("connect rejected (not registered) " MACSTR_, MAC2STR_(src));
      return;
    }
    int i = portAssign(src, (uint8_t)p.aux);
    if (i < 0) {
      reply(src, PKT_REJECT, ENM_REJECT_FULL);
      evlog("connect rejected (no free port) " MACSTR_, MAC2STR_(src));
      return;
    }
    ports[i].lastRxMs = millis();
    // If the C6 restarted and reconnected, sequence numbers start from 0 on both sides
    ports[i].rxHaveSeq = false;
    ports[i].rxPktCnt = 0;
    ports[i].txFailCnt = 0;
    ports[i].haveSeq = false;
    ports[i].haveOff = false;  // its clock started again, so the offset to it has changed
    ports[i].txSeq = 0;
    ports[i].rxChMask = p.seq;   // the C6 announces its listening channels on every connect
    ports[i].subFlags = p.len;
    reply(src, PKT_CONNECT_OK, i, wifiChannel);
    return;
  }

  // Below: packets from connected C6 units (that hold a port)
  int i = portFind(src);
  if (i < 0) {
    // If registered, assign a port on the spot (recovery after a bridge restart or a temporary dropout)
    uint8_t reason = ENM_REJECT_UNREGISTERED;
    if (regFind(src) >= 0) {
      i = portAssign(src, ENM_RATE_DEFAULT);  // use the default rate until the next CONNECT or CFG reports it
      reason = ENM_REJECT_FULL;
    }
    if (i < 0) {
      rejects++;
      if (millis() - lastRejectMs > 1000) {  // reply at most once per second
        lastRejectMs = millis();
        reply(src, PKT_REJECT, reason);
        evlog("packet rejected (%s) " MACSTR_, reason == ENM_REJECT_FULL ? "no free port" : "not registered", MAC2STR_(src));
      }
      return;
    }
  }
  ports[i].lastRxMs = millis();
  ports[i].rssi = rssi;
  ports[i].failStreak = 0;
  switch (p.type) {
    case PKT_PING: {  // reply immediately, highest priority
      p.type = PKT_PONG;
      enm_radio_send(src, (const uint8_t *)&p, sizeof(p));
      break;
    }
    case PKT_MIDI: {  // arrives batched; the 4th byte is the count
      uint8_t cnt = d[3];
      if (cnt < 1 || cnt > ENM_BATCH_MAX || n < ENM_HDR_LEN + 3 * cnt) break;
      // The same packet may arrive twice (when the peer missed the ACK and the radio layer resent it)
      if (ports[i].rxPktCnt && p.seq == ports[i].rxPktSeq && p.t_us == ports[i].rxPktTus && cnt == ports[i].rxPktCnt) { rxPktDup++; break; }
      ports[i].rxPktSeq = p.seq; ports[i].rxPktTus = p.t_us; ports[i].rxPktCnt = cnt;
      for (uint8_t k = 0; k < cnt; k++) {
        const uint8_t *m = d + ENM_HDR_LEN + 3 * k;
        RxItem it = {{m[0], m[1], m[2]}, midi_len(m[0]), (uint8_t)i, (uint16_t)(p.seq + k), p.aux, rssi, t, p.t_us};
        if (xQueueSend(rxq, &it, 0) != pdTRUE) rxqDrop++;
        busRoute(i, m);
      }
      ports[i].rxLastSeq = (uint16_t)(p.seq + cnt - 1);  // last sequence number received so far
      ports[i].rxHaveSeq = true;
      break;
    }
    case PKT_MIDI_RETRY: {  // resend of undelivered Note Offs. Accept only those not yet received
      uint8_t cnt = d[3];
      if (cnt < 1 || cnt > ENM_RETRY_MAX || n < ENM_HDR_LEN + 5 * cnt) break;
      bool any = false;
      uint16_t newest = ports[i].rxLastSeq;
      for (uint8_t k = 0; k < cnt; k++) {
        const uint8_t *q = d + ENM_HDR_LEN + 5 * k;
        uint16_t sq = q[0] | (q[1] << 8);
        if (ports[i].rxHaveSeq && !enm_seq_newer(sq, ports[i].rxLastSeq)) { retryDup++; continue; }  // already received
        RxItem it = {{q[2], q[3], q[4]}, midi_len(q[2]), (uint8_t)i, sq, 0, rssi, t, p.t_us};
        if (xQueueSend(rxq, &it, 0) != pdTRUE) rxqDrop++;
        busRoute(i, q + 2);
        retryAccepted++;
        if (!any || enm_seq_newer(sq, newest)) newest = sq;
        any = true;
      }
      if (any) { ports[i].rxLastSeq = newest; ports[i].rxHaveSeq = true; }
      break;
    }
    case PKT_SUBSCRIBE:
      ports[i].rxChMask = p.aux;
      ports[i].subFlags = p.len;
      evlog("port %d receive channels: mask=0x%04X flags=%u", i + 1, p.aux, p.len);
      break;
    case PKT_CFG:
      if (p.aux < ENM_RATE_NUM) {
        ports[i].rate = p.aux;
        enm_apply_rate(src, p.aux);
      }
      break;
    case PKT_CMD_REPLY: {  // reply to a relayed configuration command; printed to serial in loop()
      uint8_t len = d[3];
      if (len > ENM_CMD_TEXT_MAX || n < ENM_HDR_LEN + len || relayReplyReady) break;
      {  // accept only the reply to the pending command (discard replies duplicated by resends and stale ones)
        size_t il = strlen(relayId);
        if (!il || len < il + 2 || d[ENM_HDR_LEN] != '@' || memcmp(d + ENM_HDR_LEN + 1, relayId, il) != 0 || d[ENM_HDR_LEN + 1 + il] != ' ') break;
      }
      memcpy(relayReply, d + ENM_HDR_LEN, len);
      relayReply[len] = 0;
      relayReplyReady = true;
      break;
    }
    default: break;  // KEEPALIVE etc.
  }
}

// port: 0-based. First byte = port number<<4 | CIN (= upper 4 bits of status)
// CIN (USB-MIDI packet type): for channel messages, the upper 4 bits of status; for system messages, determined by length
static bool MLINK_HOT usbWrite(uint8_t port, const uint8_t *m, uint8_t len) {
  uint8_t cin;
  if (m[0] < 0xF0) cin = m[0] >> 4;
  else if (m[0] >= 0xF8) cin = 0xF;   // realtime (1 byte)
  else if (len == 1) cin = 0x5;       // 1-byte system common (tune request)
  else if (len == 2) cin = 0x2;       // 2-byte system common
  else cin = 0x3;                     // 3-byte system common (song position)
  uint8_t pk[4] = {(uint8_t)((port << 4) | cin), m[0], (uint8_t)(len >= 2 ? m[1] : 0), (uint8_t)(len == 3 ? m[2] : 0)};
  return tud_midi_packet_write(pk);
}

// ESP-NOW → USB-MIDI
static void MLINK_HOT fwdTask(void *) {
  RxItem it;
  for (;;) {
    if (xQueueReceive(rxq, &it, portMAX_DELAY) != pdTRUE) continue;
    // If the USB send buffer (16 entries) is full, wait until there is room and write again (so batched arrivals do not overflow).
    // Wait at most 50 ms in case the PC is not reading.
    bool ok = usbWrite(it.port, it.m, it.len);
    for (int k = 0; !ok && k < 50 && tud_mounted(); k++) {
      usbRetry++;
      vTaskDelay(1);
      ok = usbWrite(it.port, it.m, it.len);
    }
    uint32_t tDone = now_us();
    uint32_t dt = tDone - it.t_cb;
    if (!ok) usbFail++;
    else hCb2Usb.add(dt);
    if (ok && traceStatus && it.m[0] == traceStatus) traceOut(it, tDone);
    rxMidi++;
    Port &pt = ports[it.port];
    if (pt.haveSeq && it.seq != (uint16_t)(pt.lastSeq + 1)) seqGap += (uint16_t)(it.seq - pt.lastSeq - 1);
    pt.lastSeq = it.seq;
    pt.haveSeq = true;
    rssiSum += it.rssi;
    rssiN++;
    // Relative one-way delay: (S3 receive time - C6 send time) = clock offset + delay. Its minimum is taken as 0 and the difference is observed.
    // Clock drift (tens of ppm) shifts it by up to ~2 ms per minute, so reset with 'c' for long runs
    uint32_t off = it.t_cb - it.t_send;
    if (!pt.haveOff || (int32_t)(off - pt.minOff) < 0) { pt.minOff = off; pt.haveOff = true; }
    uint32_t rel = off - pt.minOff;
    hRel.add(rel);
    if (verbose || (midiLogFilter && midiLogFilter(it.m))) {
      LogItem l = {{it.m[0], it.m[1], it.m[2]}, it.port, it.seq, it.aux, it.rssi, dt, rel};
      xQueueSend(logq, &l, 0);
    }
  }
}

// USB-MIDI → ESP-NOW
static void usbRxNotify() {
  if (usbInTaskH) xTaskNotifyGive(usbInTaskH);
}
static void MLINK_HOT usbInTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));  // check every 1 ms even without a notification
    uint8_t pk[4];
    while (tud_midi_packet_read(pk)) {
      uint8_t port = pk[0] >> 4;
      uint8_t cin = pk[0] & 0x0F;
      if (cin == 0x4 || cin == 0x6 || cin == 0x7) continue;  // SysEx continuation and end are not handled
      uint8_t m[3] = {pk[1], pk[2], pk[3]};
      uint8_t L = midi_len(m[0]);
      if (!L) continue;  // SysEx etc. are not carried
      if (L < 3) m[2] = 0;
      if (L < 2) m[1] = 0;
      if (usbMidiHandler && usbMidiHandler(port, m)) continue;  // handled by the application
      if (port >= NUM_PORTS || !ports[port].active) continue;  // send only to the C6 connected to that port
      if (traceStatus && m[0] == traceStatus) traceIn(port, m);
      TxItem it;
      it.port = port;
      memcpy(it.mac, ports[port].mac, 6);
      enm_init(it.p, PKT_MIDI);
      it.p.len = L;
      it.p.t_us = now_us();  // time queued (used by the send task to judge how long it waited)
      memcpy(it.p.midi, m, 3);
      // If the queue is full, wait here until there is room. USB is not read meanwhile, so the PC is made to wait (nothing is dropped anywhere)
      if (xQueueSend(txq, &it, 0) != pdTRUE) {
        txWaits++;
        if (txTaskH) xTaskNotifyGive(txTaskH);
        xQueueSend(txq, &it, portMAX_DELAY);
      }
      UBaseType_t depth = uxQueueMessagesWaiting(txq);
      if (depth > txqPeak) txqPeak = depth;
      if (txTaskH) xTaskNotifyGive(txTaskH);
    }
  }
}

// Dedicated send task: sends to the radio in queue order. When the radio cannot accept, waits and retries instead of dropping
static volatile uint32_t txPackets, txBatchMax;
static volatile uint32_t pktFail, offRetried, offRecovered, offGiveUp;  // Note Off resend statistics (PC -> C6)
static volatile uint32_t noteOnExpired;                                 // Note Ons dropped as stale
static volatile uint8_t txDone = 0;  // result of a packet sent from the send queue: 0 = pending, 1 = delivered, 2 = not delivered
// Breakdown of send-complete notifications (for investigating missed notifications)
static volatile uint32_t txTimeouts, cbMidi, cbOther, cbLateMidi, cbLateUs;
static volatile uint8_t cbLastOtherType;
static volatile bool txWaiting = false;
static volatile uint32_t txWaitStartUs;

// Per-client (per-port) send state
#define PTX_LEN 64
struct PTx {
  TxItem ring[PTX_LEN];   // pending sends
  uint16_t head, tail;
  volatile bool inflight;        // handed to the radio, awaiting the result
  volatile uint8_t result;       // 0 = pending, 1 = delivered, 2 = not delivered
  bool havePkt;                  // pkt holds a packet the radio has not yet accepted
  bool isRetry;
  uint32_t sentMs, sentUs, enqUs;
  volatile uint32_t cbUs;
  uint8_t mac[6];
  uint8_t pkt[ENM_HDR_LEN + (5 * ENM_RETRY_MAX > 3 * ENM_BATCH_MAX ? 5 * ENM_RETRY_MAX : 3 * ENM_BATCH_MAX)];
  uint16_t len;
  uint8_t n;
  uint16_t seq0;
  struct { uint16_t seq; uint8_t m[3]; } rq[ENM_RETRY_MAX];  // Note Offs from undelivered packets (awaiting resend)
  uint8_t rqN;
  uint8_t rqMac[6];
  uint32_t rqSince, rqHoldUntil;
};
static PTx ptx[NUM_PORTS];
static void ptxAbortInflight();

// ---- Radio retry limit (see enm_retry_set) ----
// The library's limit is 32. With 8, the longest delays of near clients while a far client (about -87 dBm) does not
// answer were about 2.6 ms shorter (p99 about 0.9 ms), while that far client lost about 0.6% of its packets instead of
// 0.08% (Note Offs are resent by this bridge). Going below 8 did not shorten the delays further and lost more packets.
// retryLimit = 0 leaves the library's value; otherwise it is applied at startup and again after every restart of the radio.
#define ENM_RETRY_LIMIT_DEFAULT 8
static uint8_t retryLimit = ENM_RETRY_LIMIT_DEFAULT;

// ---- Slow radio sends (diagnostic) ----
// A packet whose result (delivered or not) came slowTxUs or more after it was handed to the radio is recorded with its
// port, times and result, so that the PC can see which packets were held up together, and whether an undelivered packet
// (which the radio keeps retrying) came first. Times are the bridge's microsecond clock (as in_us of the trace).
// The last SLOWTX_N are kept.
#define SLOWTX_N 64
#define SLOWTX_OK 0x01       // delivered
#define SLOWTX_RESEND 0x02   // a Note Off resend packet
#define SLOWTX_TIMEOUT 0x04  // no result at all (counted as not delivered)
struct SlowTx { uint32_t sentUs, airUs; uint16_t noMem; uint8_t port, flags, len; int8_t rssi; };
static SlowTx slowTx[SLOWTX_N];
static volatile uint32_t slowTxN;
static volatile uint32_t slowTxUs = 10000;

// ---- Detecting and restarting a stalled radio driver ----
static volatile uint32_t stallRestarts, wifiRestarts, driverReinits;
static uint32_t stallReboots;  // restarts of the whole device because the radio could not be recovered (saved)
static volatile bool debugStallFlag = false;  // diagnostic: drop send-complete notifications to simulate a stall (cleared by restart)
static volatile bool debugStallHold = false;  // diagnostic: keep simulating the stall across radio restarts (to test escalation)
static uint32_t lastRestartMs = 0;
static uint8_t txTimeoutStreak = 0;
static uint16_t noMemStreak = 0;
static volatile bool cbSinceRestart = true;  // a send-complete callback arrived since the last restart (the driver is working)
static uint8_t noRecover = 0;                // restarts in a row after which no send-complete callback arrived
static void onSent(const esp_now_send_info_t *info, esp_now_send_status_t st);
// Restart the radio. level 0 = ESP-NOW, 1 = also Wi-Fi (stop / start), 2 = also tear down and initialize the Wi-Fi driver.
// Restores connected clients (peer entries, rates) and the channel
static void radioRestart(uint8_t level) {
  static const char *const msg[] = {"radio stalled: restarting ESP-NOW", "radio stalled again: restarting Wi-Fi and ESP-NOW",
                                    "radio not recovering: initializing the Wi-Fi driver again"};
  uint32_t t0 = millis();
  size_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  evlog("%s (internal heap %u)", msg[level > 2 ? 2 : level], (unsigned)h0);
  enm_radio_block();
  if (level >= 2) esp_wifi_stop();  // stop first so that frames queued in the stalled driver are released
  esp_now_deinit();
  if (level >= 2) {
    WiFi.mode(WIFI_OFF);  // stops and deinitializes the Wi-Fi driver
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
  } else if (level == 1) {
    esp_wifi_stop();
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  enm_radio_unblock();
  tmpValid = false;
  for (int i = 0; i < NUM_PORTS; i++) {
    if (!ports[i].active) continue;
    addPeer(ports[i].mac);
    enm_apply_rate(ports[i].mac, ports[i].rate);
  }
  if (retryLimit) enm_retry_set(retryLimit);
  if (!debugStallHold) debugStallFlag = false;
  ptxAbortInflight();  // notifications for packets already handed over will never arrive
  txTimeoutStreak = 0;
  noMemStreak = 0;
  cbSinceRestart = false;
  lastRestartMs = millis();
  if (level >= 2) driverReinits++;
  else if (level == 1) wifiRestarts++;
  else stallRestarts++;
  evlog("radio restarted in %lu ms (internal heap %u -> %u)", (unsigned long)(millis() - t0), (unsigned)h0,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
// Survives a software restart: tells begin() to keep the channel instead of surveying again (so clients are back in seconds)
#define STALL_BOOT_MAGIC 0x5A11B000u
RTC_NOINIT_ATTR static uint32_t stallBootTag;
RTC_NOINIT_ATTR static uint8_t stallBootMac[NUM_PORTS][6];  // client on each port before the restart (all 0 = none)
static void stallReboot(const char *why) {
  evlog("%s: restarting the device", why);
  prefs.putULong("stallboot", stallReboots + 1);
  for (int i = 0; i < NUM_PORTS; i++) {
    if (ports[i].active) memcpy(stallBootMac[i], ports[i].mac, 6);
    else memset(stallBootMac[i], 0, 6);
  }
  stallBootTag = STALL_BOOT_MAGIC | wifiChannel;
  delay(200);  // let the log reach serial
  esp_restart();
}
// noMem: the driver has refused every packet for 1 s (rather than accepting packets and never reporting their results)
static void stallHandle(bool noMem) {
  uint32_t since = millis() - lastRestartMs;
  // do not restart back to back. If the last restart did not bring the driver back, retry sooner
  if (lastRestartMs && since < (cbSinceRestart ? ENM_STALL_BACKOFF_MS : ENM_STALL_RETRY_MS)) return;
  if (cbSinceRestart) noRecover = 0;
  else if (noRecover < 255) noRecover++;
  // nothing helped: restart the device (USB disconnects and comes back)
  if (noRecover >= ENM_STALL_REBOOT_AFTER) stallReboot("radio not recovering");
  bool reinit = noMem || noRecover >= ENM_STALL_REINIT_AFTER;
  if (reinit && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < ENM_STALL_HEAP_MIN) stallReboot("radio stalled, low memory");
  if (reinit) radioRestart(2);
  else if (lastRestartMs && since < ENM_STALL_WIFI_WINDOW_MS) radioRestart(1);
  else radioRestart(0);
}

// ---- Send task ----
// Each client (port) has its own send queue and "one packet handed to the radio, awaiting its result".
// Order is preserved only within one client; order across clients is not preserved.
//   txParallel = true (default): one packet per client handed to the radio in parallel, so a client that is hard to reach
//                                holds up the others less
//   txParallel = false         : only one packet in flight across all clients (same behavior as the single-queue send task)
// Either way, for one client the next packet is handed over only after the previous one's result, so order does not change.
static volatile bool txParallel = true;
static volatile bool txImplOld = false;  // diagnostic: true = use the single-queue send task (txTaskOld) for comparison
static TaskHandle_t txTaskNewH, txTaskOldH;
static LatHist hTxQ;        // time from queuing to handing to the radio (first message of the batched packet)
static LatHist hTxAir;      // time from handing to the radio until the result
static LatHist hQWake;      // time from queuing until the send task takes it out
static LatHist hApi;        // time spent in the esp_now_send() call
static LatHist hSendCb;     // from just before esp_now_send() until the completion notification (onSent)
static LatHist hCbWake;     // from the completion notification until the send task processes it
static volatile uint32_t cbUsG;

static volatile uint32_t txInflightPeak = 0;
static uint8_t txInflight = 0;
static uint8_t txRR = 0;
static uint32_t loopUs = 0;
static volatile uint64_t twCycles = 0;   // CPU cycles spent in the send task itself (excluding esp_now_send and waiting)
static volatile uint32_t twPackets = 0, twMaxCycles = 0;
// Clean up packets in flight (used when restarting the radio)
static void ptxAbortInflight() {
  for (int i = 0; i < NUM_PORTS; i++)
    if (ptx[i].inflight && !ptx[i].result) ptx[i].result = 2;
}

// Cleanup for a packet whose result is known. r: 1 = delivered, 2 = not delivered
static void MLINK_HOT ptxComplete(int i, uint8_t r) {
  PTx &q = ptx[i];
  q.inflight = false;
  q.havePkt = false;
  if (txInflight) txInflight--;
  if (q.isRetry) {
    if (r == 1) {
      offRecovered += q.rqN;
      q.rqN = 0;
    } else if (millis() - q.rqSince > ENM_RETRY_GIVEUP_MS) {
      offGiveUp += q.rqN;
      q.rqN = 0;
    } else {
      q.rqHoldUntil = millis() + 2;
    }
    return;
  }
  if (r != 2) return;
  // Not delivered. Extract only Note Offs and queue them for resending (nothing else is resent)
  pktFail++;
  for (uint8_t k = 0; k < q.n && q.rqN < ENM_RETRY_MAX; k++) {
    const uint8_t *m = q.pkt + ENM_HDR_LEN + 3 * k;
    if (enm_is_note_off(m)) {
      q.rq[q.rqN].seq = (uint16_t)(q.seq0 + k);
      memcpy(q.rq[q.rqN].m, m, 3);
      q.rqN++;
    }
  }
  if (q.rqN) {
    offRetried += q.rqN;
    q.rqSince = millis();
    q.rqHoldUntil = 0;
    memcpy(q.rqMac, q.mac, 6);
  }
}

// Build the next packet to send. Returns true if built (into q.pkt / q.len / q.mac)
static bool MLINK_HOT ptxBuild(int i) {
  PTx &q = ptx[i];
  Port &pt = ports[i];
  if (q.rqN) {
    // Do resends first (sent before new messages, so order is not changed)
    if (!pt.active || memcmp(pt.mac, q.rqMac, 6) != 0) { offGiveUp += q.rqN; q.rqN = 0; return false; }  // the peer has gone
    if ((int32_t)(millis() - q.rqHoldUntil) < 0) return false;
    enm_pkt_t h;
    enm_init(h, PKT_MIDI_RETRY);
    h.len = q.rqN;
    h.t_us = now_us();
    memcpy(q.pkt, &h, ENM_HDR_LEN);
    for (uint8_t k = 0; k < q.rqN; k++) {
      uint8_t *d = q.pkt + ENM_HDR_LEN + 5 * k;
      d[0] = q.rq[k].seq & 0xFF;
      d[1] = q.rq[k].seq >> 8;
      memcpy(d + 2, q.rq[k].m, 3);
    }
    q.len = ENM_HDR_LEN + 5 * q.rqN;
    q.isRetry = true;
    memcpy(q.mac, q.rqMac, 6);
    return true;
  }
  // Batch pending sends for the same client into one packet. Note Ons that waited too long in the queue are dropped here
  uint8_t n = 0;
  enm_pkt_t first;
  uint32_t nowUs = now_us(), maxAgeUs = (uint32_t)noteOnMaxAgeMs * 1000;
  while (n < ENM_BATCH_MAX && q.head != q.tail) {
    TxItem &x = q.ring[q.tail % PTX_LEN];
    q.tail++;
    if (!pt.active || memcmp(pt.mac, x.mac, 6) != 0) { txSkipped++; continue; }  // the destination has gone or changed
    if (maxAgeUs && enm_is_note_on(x.p.midi) && (uint32_t)(nowUs - x.p.t_us) > maxAgeUs) { noteOnExpired++; continue; }
    if (n == 0) { first = x.p; memcpy(q.mac, x.mac, 6); }
    memcpy(q.pkt + ENM_HDR_LEN + 3 * n, x.p.midi, 3);
    n++;
  }
  if (n == 0) return false;
  q.enqUs = first.t_us;
  int32_t wake = (int32_t)(loopUs - first.t_us);  // negative when queued after this pass of the send task started
  hQWake.add(wake > 0 ? (uint32_t)wake : 0);
  enm_pkt_t h = first;
  h.len = n;
  q.seq0 = pt.txSeq;  // sequence numbers are assigned here, once the send order is fixed
  h.seq = q.seq0;
  pt.txSeq += n;
  h.t_us = now_us();
  memcpy(q.pkt, &h, ENM_HDR_LEN);
  q.n = n;
  q.len = ENM_HDR_LEN + 3 * n;
  q.isRetry = false;
  txSent += n;
  txPackets++;
  if (n > txBatchMax) txBatchMax = n;
  return true;
}

static void MLINK_HOT txTask(void *) {
  TxItem it;
  bool busy = false;
  for (;;) {
    if (txImplOld) { vTaskDelay(10); continue; }  // diagnostic: idle while the single-queue send task is selected
    // Wait for a notification (queued / send finished). Only while results or resends are pending, also check every 1 ms.
    // When idle, sleep until notified (waking every 1 ms would slightly delay other tasks)
    ulTaskNotifyTake(pdTRUE, busy ? pdMS_TO_TICKS(1) : pdMS_TO_TICKS(50));
    loopUs = now_us();
    uint32_t cyc0 = esp_cpu_get_cycle_count(), cycApi = 0;
    // (1) Move from the shared queue to per-client send queues
    while (xQueuePeek(txq, &it, 0) == pdTRUE) {
      Port &pt = ports[it.port];
      // Do not send to destinations that have gone or to unresponsive clients (cleared when anything arrives from that client)
      if (!pt.active || memcmp(pt.mac, it.mac, 6) != 0 || txSkipPeer(pt.failStreak, pt.probeMs, pt.probeGap)) {
        xQueueReceive(txq, &it, 0);
        txSkipped++;
        continue;
      }
      PTx &q = ptx[it.port];
      if ((uint16_t)(q.head - q.tail) >= PTX_LEN) break;  // this client's queue is full; wait for room
      xQueueReceive(txq, &it, 0);
      q.ring[q.head % PTX_LEN] = it;
      q.head++;
    }
    // (2) Clean up packets whose results are known
    for (int i = 0; i < NUM_PORTS; i++) {
      PTx &q = ptx[i];
      if (!q.inflight) continue;
      uint8_t r = q.result;
      bool timedOut = !r;
      if (!r) {
        if (millis() - q.sentMs < 100) continue;
        txTimeouts++;  // no completion notification (delivery unknown). If it persists, the driver is stalled
        if (++txTimeoutStreak >= ENM_STALL_TIMEOUTS) stallHandle(false);
        r = 2;
      } else {
        txTimeoutStreak = 0;
        uint32_t w = now_us();
        hTxAir.add(w - q.sentUs);
        hSendCb.add(q.cbUs - q.sentUs);
        hCbWake.add(w - q.cbUs);
      }
      uint32_t air = (timedOut ? now_us() : q.cbUs) - q.sentUs;
      if (slowTxUs && air >= slowTxUs) {
        SlowTx &e = slowTx[slowTxN % SLOWTX_N];
        e = {q.sentUs, air, (uint16_t)txRetry, (uint8_t)i,
             (uint8_t)((r == 1 ? SLOWTX_OK : 0) | (q.isRetry ? SLOWTX_RESEND : 0) | (timedOut ? SLOWTX_TIMEOUT : 0)),
             (uint8_t)(q.len > 255 ? 255 : q.len), ports[i].rssi};
        slowTxN++;
      }
      ptxComplete(i, r);
    }
    // (3) Hand packets for clients that can send to the radio
    uint8_t limit = txParallel ? NUM_PORTS : 1;
    for (int k = 0; k < NUM_PORTS && txInflight < limit; k++) {
      int i = (txRR + k) % NUM_PORTS;
      PTx &q = ptx[i];
      if (q.inflight) continue;
      if (!q.havePkt) {
        if (!q.rqN && q.head == q.tail) continue;
        if (!ptxBuild(i)) continue;
        q.havePkt = true;
      }
      q.result = 0;
      q.sentMs = millis();
      q.sentUs = now_us();
      q.inflight = true;  // set before handing over, in case the completion notification arrives first
      txInflight++;
      uint32_t ca = esp_cpu_get_cycle_count();
      esp_err_t e = enm_radio_send(q.mac, q.pkt, q.len);
      cycApi += esp_cpu_get_cycle_count() - ca;
      hApi.add(now_us() - q.sentUs);
      if (e == ESP_OK) twPackets++;
      if (e == ESP_OK) {
        noMemStreak = 0;
        if (!q.isRetry) hTxQ.add(q.sentUs - q.enqUs);
        if (txInflight > txInflightPeak) txInflightPeak = txInflight;
        continue;
      }
      q.inflight = false;
      txInflight--;
      if (e == ESP_ERR_ESPNOW_NO_MEM) {  // the radio cannot accept. Keep the built packet and hand it over again on the next pass
        txRetry++;
        if (++noMemStreak >= 1000) { noMemStreak = 0; stallHandle(true); }
        break;
      }
      txErr++;  // peer not registered, etc. Drop this packet (Note Offs go to resend)
      q.inflight = true;
      txInflight++;
      ptxComplete(i, 2);
    }
    txRR = (txRR + 1) % NUM_PORTS;
    busy = uxQueueMessagesWaiting(txq) > 0;
    for (int i = 0; i < NUM_PORTS && !busy; i++) busy = ptx[i].inflight || ptx[i].havePkt || ptx[i].rqN || ptx[i].head != ptx[i].tail;
    {
      uint32_t w = esp_cpu_get_cycle_count() - cyc0 - cycApi;
      twCycles += w;
      if (w > twMaxCycles) twMaxCycles = w;
    }
  }
}

// ---- Diagnostic: single-queue send task (selected with tx_impl=0, for comparison) ----
// Hand one packet to the radio and wait until sending finishes. Returns true = delivered (ACK received)
static bool MLINK_HOT txSendWaitOld(const uint8_t *mac, const uint8_t *buf, size_t len) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, 0);  // clear stale notifications
    txDone = 0;
    txWaitStartUs = now_us();
    txWaiting = true;
    uint32_t sU = now_us();
    esp_err_t e = enm_radio_send(mac, buf, len);
    hApi.add(now_us() - sU);
    if (e == ESP_OK) {
      uint32_t t0 = millis();
      while (!txDone && millis() - t0 < 100) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
      if (txDone) {
        uint32_t w = now_us();
        hSendCb.add(cbUsG - sU);
        hCbWake.add(w - cbUsG);
      }
      txWaiting = false;
      if (!txDone) {  // no completion notification (delivery unknown). If it persists, the driver is stalled
        txTimeouts++;
        if (++txTimeoutStreak >= ENM_STALL_TIMEOUTS) stallHandle(false);
      } else {
        txTimeoutStreak = 0;
      }
      return txDone == 1;
    }
    if (e != ESP_ERR_ESPNOW_NO_MEM) { txErr++; return false; }  // peer not registered, etc.
    txRetry++;
    if (++noMemStreak >= 1000) {  // not accepted for 1 second -> stalled
      noMemStreak = 0;
      stallHandle(true);
      return false;
    }
    vTaskDelay(1);  // filled by other sends (PONG etc.). Wait 1 ms and retry
  }
}

static void MLINK_HOT txTaskOld(void *) {
  static uint8_t buf[ENM_HDR_LEN + 5 * ENM_RETRY_MAX > ENM_HDR_LEN + 3 * ENM_BATCH_MAX ? ENM_HDR_LEN + 5 * ENM_RETRY_MAX : ENM_HDR_LEN + 3 * ENM_BATCH_MAX];
  // Note Offs from undelivered packets (awaiting resend; all to the same peer)
  static struct { uint16_t seq; uint8_t m[3]; } rq[ENM_RETRY_MAX];
  uint8_t rqN = 0, rqPort = 0;
  uint8_t rqMac[6];
  uint32_t rqSince = 0;
  TxItem it;
  for (;;) {
    if (rqN) {
      // Do resends first (sent before new messages, so order is not changed)
      Port &rp = ports[rqPort];
      if (!rp.active || memcmp(rp.mac, rqMac, 6) != 0) { offGiveUp += rqN; rqN = 0; continue; }  // the peer has gone
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
      if (txSendWaitOld(rqMac, buf, ENM_HDR_LEN + 5 * rqN)) {
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
    if (!txImplOld) { vTaskDelay(10); continue; }  // diagnostic: idle while the per-client send task is selected
    if (xQueueReceive(txq, &it, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    hQWake.add(now_us() - it.p.t_us);
    uint32_t cyc0 = esp_cpu_get_cycle_count(), cycB = 0;
    Port &pt = ports[it.port];
    // Do not send to destinations that have gone (port freed / replaced by another C6)
    if (!pt.active || memcmp(pt.mac, it.mac, 6) != 0) { txSkipped++; continue; }
    // Do not let pending sends to an unresponsive C6 block sends to other C6 units (cleared when anything arrives from that C6)
    if (txSkipPeer(pt.failStreak, pt.probeMs, pt.probeGap)) { txSkipped++; continue; }
    // Batch pending sends for the same peer into one packet (stop when one for another peer appears)
    // Note Ons that waited too long in the queue are dropped here (p.t_us is the time queued)
    uint8_t n = 0;
    TxItem first = it, nx;
    uint32_t nowUs = now_us(), maxAgeUs = (uint32_t)noteOnMaxAgeMs * 1000;
    auto expired = [&](const TxItem &x) { return maxAgeUs && enm_is_note_on(x.p.midi) && (uint32_t)(nowUs - x.p.t_us) > maxAgeUs; };
    if (expired(it)) noteOnExpired++;
    else {
      memcpy(buf + ENM_HDR_LEN, it.p.midi, 3);
      n = 1;
    }
    while (n < ENM_BATCH_MAX && xQueuePeek(txq, &nx, 0) == pdTRUE && nx.port == first.port && memcmp(nx.mac, first.mac, 6) == 0) {
      xQueueReceive(txq, &nx, 0);
      if (expired(nx)) { noteOnExpired++; continue; }
      memcpy(buf + ENM_HDR_LEN + 3 * n, nx.p.midi, 3);
      n++;
    }
    if (n == 0) continue;  // all dropped
    enm_pkt_t h = first.p;
    h.len = n;
    uint16_t seq0 = pt.txSeq;  // sequence numbers are assigned here, once the send order is fixed
    h.seq = seq0;
    pt.txSeq += n;
    h.t_us = now_us();
    memcpy(buf, &h, ENM_HDR_LEN);
    // One packet at a time: after handing it to the radio, wait for the send to finish before batching the next one.
    // Messages arriving from the PC meanwhile are batched into the next packet.
    uint32_t tS = now_us();
    hTxQ.add(tS - first.p.t_us);
    cycB = esp_cpu_get_cycle_count() - cyc0;
    bool ok = txSendWaitOld(first.mac, buf, ENM_HDR_LEN + 3 * n);
    hTxAir.add(now_us() - tS);
    uint32_t cyc1 = esp_cpu_get_cycle_count();
    twPackets++;
    txSent += n;
    txPackets++;
    if (n > txBatchMax) txBatchMax = n;
    if (!ok) {
      // Not delivered. Extract only Note Offs and queue them for resending (nothing else is resent)
      pktFail++;
      for (uint8_t k = 0; k < n && rqN < ENM_RETRY_MAX; k++) {
        const uint8_t *m = buf + ENM_HDR_LEN + 3 * k;
        if (enm_is_note_off(m)) {
          rq[rqN].seq = (uint16_t)(seq0 + k);
          memcpy(rq[rqN].m, m, 3);
          rqN++;
        }
      }
      if (rqN) {
        offRetried += rqN;
        rqSince = millis();
        rqPort = first.port;
        memcpy(rqMac, first.mac, 6);
      }
    }
    {
      uint32_t w = cycB + (esp_cpu_get_cycle_count() - cyc1);
      twCycles += w;
      if (w > twMaxCycles) twMaxCycles = w;
    }
  }
}

// Send complete (MAC-layer ACK received, or not delivered after retries)
static void MLINK_HOT onSent(const esp_now_send_info_t *info, esp_now_send_status_t st) {
  if (debugStallFlag) return;
  cbSinceRestart = true;
  if (info && info->des_addr) {
    int i = portFind(info->des_addr);
    if (i >= 0) {
      if (st == ESP_NOW_SEND_SUCCESS) ports[i].failStreak = 0;
      else {
        txFail++;
        ports[i].txFailCnt++;
        if (ports[i].failStreak < 255) ports[i].failStreak++;
      }
    }
  }
  // Notify the send task only of results for packets sent from the send queue (MIDI and its resends)
  uint8_t ty = enm_cb_type(info);
  if (!ty) cbTypeUnknown++;
  if (!ty || ty == PKT_MIDI || ty == PKT_MIDI_RETRY) {
    cbMidi++;
    int pi = (info && info->des_addr) ? portFind(info->des_addr) : -1;
    cbUsG = now_us();  // for the single-queue send task
    txDone = st == ESP_NOW_SEND_SUCCESS ? 1 : 2;
    if (pi >= 0 && ptx[pi].inflight && !ptx[pi].result) {
      ptx[pi].cbUs = now_us();
      ptx[pi].result = st == ESP_NOW_SEND_SUCCESS ? 1 : 2;
    }
    else cbLateMidi++;  // notification that arrived after waiting was abandoned
    if (txTaskH) xTaskNotifyGive(txTaskH);
  } else {
    cbOther++;
    cbLastOtherType = ty;
  }
}

// ---- Channel selection ----
// On each channel, receive all frames in the air for a fixed time and measure the fraction of time the air was busy.
// This looks at actual traffic, not the number of Wi-Fi networks (many networks may still be idle).
// Measured only at bridge startup, so time is spent for accuracy (13 channels x 6 passes x 0.15 s = about 12 s)
#define SURVEY_DWELL_MS 150        // duration of one measurement (longer than the beacon interval of about 102 ms)
#define SURVEY_PASSES 6            // how many times all channels are measured at staggered times (to avoid depending on a chance moment)
#define SURVEY_RSSI_MIN (-85)      // weaker signals are not counted
static volatile uint32_t svAirUs, svFrames;
static uint16_t svBusy[14];        // [1..13] fraction of busy air time (per mille)
static uint16_t svCount[14];       // number of frames received
static uint16_t svPeak[14];        // fraction in the busiest pass (per mille)
static float svScore[14];          // score including neighboring channels (smaller = less busy)

static void svRx(void *buf, wifi_promiscuous_pkt_type_t) {
  const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
  const wifi_pkt_rx_ctrl_t &rc = pk->rx_ctrl;
  if (rc.rssi < SURVEY_RSSI_MIN) return;
  // Do not count frames from registered C6 units (so C6 units searching for the bridge are not treated as congestion)
  // The 6 bytes starting at byte 10 of the 802.11 header are the source address
  if (rc.sig_len >= 16 && regFind(pk->payload + 10) >= 0) return;
  float mbps;
  uint32_t pre;
  if (rc.sig_mode == 0) {  // 11b/g: rate follows the order of wifi_phy_rate_t
    static const float R[16] = {1, 2, 5.5, 11, 1, 2, 5.5, 11, 48, 24, 12, 6, 54, 36, 18, 9};
    mbps = R[rc.rate & 15];
    pre = (rc.rate & 15) < 8 ? 192 : 20;
  } else {  // 11n and later: estimated from MCS
    static const float M[8] = {6.5, 13, 19.5, 26, 39, 52, 58.5, 65};
    mbps = M[rc.mcs & 7] * ((rc.mcs >> 3) + 1) * (rc.cwb ? 2.1f : 1.0f);
    pre = 36;
  }
  svAirUs += pre + (uint32_t)(rc.sig_len * 8 / mbps);
  svFrames++;
}

static void channelSurvey() {
  wifi_promiscuous_filter_t flt = {WIFI_PROMIS_FILTER_MASK_ALL};
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(svRx);
  esp_wifi_set_promiscuous(true);
  uint32_t airSum[14] = {0}, frSum[14] = {0}, airMax[14] = {0};
  for (int pass = 0; pass < SURVEY_PASSES; pass++) {
    for (uint8_t ch = 1; ch <= 13; ch++) {
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      delay(5);
      svAirUs = 0;
      svFrames = 0;
      delay(SURVEY_DWELL_MS);
      uint32_t air = svAirUs;
      airSum[ch] += air;
      if (air > airMax[ch]) airMax[ch] = air;
      frSum[ch] += svFrames;
    }
  }
  for (uint8_t ch = 1; ch <= 13; ch++) {
    // Average and busiest pass weighted half each, since the average alone misses channels with occasional bursts of traffic
    uint32_t mean = airSum[ch] / (SURVEY_DWELL_MS * SURVEY_PASSES);  // us / ms = per mille
    uint32_t peak = airMax[ch] / SURVEY_DWELL_MS;
    uint32_t pm = (mean + peak) / 2;
    svBusy[ch] = pm > 1000 ? 1000 : pm;
    svPeak[ch] = peak > 1000 ? 1000 : peak;
    svCount[ch] = frSum[ch] > 65535 ? 65535 : frSum[ch];
  }
  esp_wifi_set_promiscuous(false);
  // A Wi-Fi signal spans about 4 channels on each side. Closer channels are weighted more
  static const float W[5] = {1.0f, 0.8f, 0.6f, 0.35f, 0.1f};
  for (int c = 1; c <= 13; c++) {
    float sc = 0;
    for (int k = 1; k <= 13; k++) {
      int dd = abs(c - k);
      if (dd <= 4) sc += W[dd] * svBusy[k];
    }
    svScore[c] = sc;
  }
}

// Choose the channel with the smallest score. If the previous channel is free enough, keep it
// (the previous channel is strongly preferred; measurements depend on chance moments, so avoid frequent changes)
// Only channels within the configured range (chMin to chMax) are chosen
static uint8_t channelChoose(uint8_t prev) {
  uint8_t best = chMin;
  for (uint8_t c = chMin + 1; c <= chMax; c++)
    if (svScore[c] < svScore[best]) best = c;
  // change only when clearly worse (score exceeds 1.5 x the best channel's score + 20)
  if (prev >= chMin && prev <= chMax && svScore[prev] <= svScore[best] * 1.5f + 20.0f) return prev;
  return best;
}

static void printSurvey() {
  Serial.printf("channel survey: %d passes x %dms (busy = 平均と最大の中間, peak = いちばん混んでいた回, score = 近くのチャンネルも加えた点数。小さいほど空いている)\n",
                SURVEY_PASSES, SURVEY_DWELL_MS);
  for (int c = 1; c <= 13; c++)
    Serial.printf("  ch %2d: busy %5.1f%%  peak %5.1f%%  frames %4u  score %6.1f%s\n", c, svBusy[c] / 10.0, svPeak[c] / 10.0, svCount[c],
                  svScore[c], c == wifiChannel ? "  <= in use" : "");
}

// Change channel while running: notify connected C6 units first, then move
static void channelSwitch(uint8_t ch) {
  if (ch == wifiChannel) return;
  for (int i = 0; i < NUM_PORTS; i++)
    if (ports[i].active)
      for (int k = 0; k < 2; k++) {
        enm_pkt_t r;
        enm_init(r, PKT_CH_SWITCH);
        r.aux = ch;
        enm_radio_send(ports[i].mac, (const uint8_t *)&r, sizeof(r));
        delay(15);
      }
  delay(30);
  wifiChannel = ch;
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  prefs.putUChar("ch", wifiChannel);
  for (int i = 0; i < NUM_PORTS; i++)
    if (ports[i].active) ports[i].lastRxMs = millis();
}

static void printList(Print &o) {
  o.printf("bridge %s  ch=%d  registered=%d/%d\n", usbProductName, wifiChannel, regCount, REG_MAX);
  for (int i = 0; i < regCount; i++) {
    int pt = portFind(regMac[i]);
    o.printf("  reg %2d: " MACSTR_ "  %s\n", i + 1, MAC2STR_(regMac[i]), pt >= 0 ? "connected" : "-");
  }
  for (int i = 0; i < NUM_PORTS; i++) {
    if (ports[i].active)
      o.printf("  Port %d: " MACSTR_ "  rate=%s  rxCh=0x%04X  last rx %lums ago\n", i + 1, MAC2STR_(ports[i].mac),
                    enm_rate_name(ports[i].rate), ports[i].rxChMask, (unsigned long)(millis() - ports[i].lastRxMs));
    else o.printf("  Port %d: (free)\n", i + 1);
  }
}

static void printStats(Print &o) {
  o.printf("rxMIDI=%lu seqGap(欠落)=%lu rxqDrop=%lu usbFail=%lu rejects=%lu rssiAvg=%.1f\n", (unsigned long)rxMidi,
                (unsigned long)seqGap, (unsigned long)rxqDrop, (unsigned long)usbFail, (unsigned long)rejects,
                rssiN ? (double)rssiSum / rssiN : 0.0);
  o.printf("NoteOff retry (PC->C6): pktFail=%lu retried=%lu recovered=%lu gaveUp=%lu | rx (C6->PC): accepted=%lu dup=%lu | cbTypeUnknown=%lu\n",
                (unsigned long)pktFail, (unsigned long)offRetried, (unsigned long)offRecovered, (unsigned long)offGiveUp,
                (unsigned long)retryAccepted, (unsigned long)retryDup, (unsigned long)cbTypeUnknown);
  o.printf("NoteOn max age=%ums expired=%lu (PC->C6)\n", noteOnMaxAgeMs, (unsigned long)noteOnExpired);
  o.printf("bus (C6->C6): routed=%lu dropped=%lu\n", (unsigned long)busRouted, (unsigned long)busDrop);
  o.printf("tx task work: %.2f us/packet (max %.1f us, packets=%lu)\n", twPackets ? (double)twCycles / twPackets / 240.0 : 0.0,
           twMaxCycles / 240.0, (unsigned long)twPackets);
  hQWake.print(o, "tx q->wake     ");
  hTxQ.print(o, "tx queue->radio");
  hApi.print(o, "tx api         ");
  hSendCb.print(o, "tx send->cb    ");
  hCbWake.print(o, "tx cb->wake    ");
  hTxAir.print(o, "tx radio->done ");
  o.printf("tx: parallel=%u sendLock=%u inflightPeak=%lu | config relay resends=%lu\n", txParallel ? 1 : 0, enmSendLock ? 1 : 0,
           (unsigned long)txInflightPeak, (unsigned long)relayResends);
  o.printf("radio stall: espnowRestarts=%lu wifiRestarts=%lu driverReinits=%lu stallReboots=%lu | duplicate packets dropped=%lu\n",
           (unsigned long)stallRestarts, (unsigned long)wifiRestarts, (unsigned long)driverReinits, (unsigned long)stallReboots,
           (unsigned long)rxPktDup);
  o.printf("heap: free=%u min=%u largest=%u | internal free=%u min=%u largest=%u | uptime=%lus\n", (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned long)(millis() / 1000));
  o.printf("tx done cb: midi=%lu other=%lu (last other type=%u) lateMidi=%lu (last late %.1fms) timeouts=%lu\n", (unsigned long)cbMidi,
                (unsigned long)cbOther, cbLastOtherType, (unsigned long)cbLateMidi, cbLateUs / 1000.0, (unsigned long)txTimeouts);
  o.printf("C6->PC: usbRetry=%lu  ", (unsigned long)usbRetry);
  o.printf("PC->C6: packets=%lu maxBatch=%lu  ", (unsigned long)txPackets, (unsigned long)txBatchMax);
  o.printf("PC->C6: sent=%lu noMemRetry=%lu macFail=%lu err=%lu skipped=%lu usbWaits=%lu queuePeak=%lu/%d\n", (unsigned long)txSent,
                (unsigned long)txRetry, (unsigned long)txFail, (unsigned long)txErr, (unsigned long)txSkipped, (unsigned long)txWaits,
                (unsigned long)txqPeak, TXQ_LEN);
  hCb2Usb.print(o, "S3 recvCB->USB write");
  hRel.print(o, "radio 相対遅延(最小=0)");
  TraceData *td = trace;
  if (traceStatus && td) {
    o.printf("trace: status=0x%02X done=%lu missed=%lu\n", traceStatus, (unsigned long)traceDoneN, (unsigned long)traceMiss);
    td->h.print(o, "trace USB in->reply USB out");
  }
}

// ================= Public functions =================

// ---- Status ----
static inline const char *name() { return usbProductName; }
static inline uint8_t radioChannel() { return wifiChannel; }
static inline uint32_t rxMidiCount() { return rxMidi; }
static inline bool portActive(uint8_t port) { return port < NUM_PORTS && ports[port].active; }

// ---- Settings and operations ----
// Send one MIDI message to the PC (USB). port is 0-based
static inline bool usbSend(uint8_t port, const uint8_t m[3]) { return usbWrite(port, m, midi_len(m[0])); }
// Time before a stale Note On is dropped [ms] (0 = never drop)
static inline void setNoteOnMaxAgeMs(uint16_t ms) { noteOnMaxAgeMs = ms; }
// Whether to log all MIDI from clients
static inline void setVerbose(bool v) { verbose = v; }
static inline bool isVerbose() { return verbose; }
// Clear all registrations (also frees ports of connected clients)
static void clearRegistry() {
  for (int i = 0; i < NUM_PORTS; i++) portFree(i, "registry cleared");
  regCount = 0;
  regNext = 0;
  regSave();
}
// Re-measure and re-choose the channel (communication stops for about 12 s while measuring). Returns the chosen channel
static uint8_t resurvey() {
  uint8_t cur = wifiChannel;
  channelSurvey();
  esp_wifi_set_channel(cur, WIFI_SECOND_CHAN_NONE);
  // On an explicit re-survey the current channel is not preferred (the best-scoring channel is chosen).
  // The previous channel is preferred only at startup
  uint8_t to = channelChoose(0);
  channelSwitch(to);
  return to;
}
// Move to the given channel (after notifying connected clients)
static inline void switchChannel(uint8_t ch) {
  if (ch >= 1 && ch <= 13) channelSwitch(ch);
}
// For testing: move to another channel for ms milliseconds to simulate an unreachable state (clients are not notified)
static void debugDeaf(uint32_t ms) {
  uint8_t other = wifiChannel <= 6 ? wifiChannel + 6 : wifiChannel - 6;
  esp_wifi_set_channel(other, WIFI_SECOND_CHAN_NONE);
  delay(ms);
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  deafCount++;
}
static void clearStats() {
  hCb2Usb.reset();
  rxMidi = seqGap = usbFail = rxqDrop = rejects = 0;
  txSent = txRetry = txErr = txFail = txSkipped = txWaits = txqPeak = 0; txPackets = 0; txBatchMax = 0; usbRetry = 0;
  pktFail = 0; offRetried = 0; offRecovered = 0; offGiveUp = 0; retryAccepted = 0; retryDup = 0; rxPktDup = 0; noteOnExpired = 0; busRouted = 0; busDrop = 0;
  rssiSum = 0; rssiN = 0; hRel.reset(); hTxQ.reset(); hTxAir.reset(); twCycles = 0; twPackets = 0; twMaxCycles = 0; hQWake.reset(); hApi.reset(); hSendCb.reset(); hCbWake.reset();
  for (int i = 0; i < NUM_PORTS; i++) ports[i].haveSeq = ports[i].haveOff = false;
  traceClear();
  slowTxN = 0;
}

// ---- Saving and loading settings (non-volatile memory) ----
// Saved: allowed radio channel range, time before a stale Note On is dropped
static void saveSettings() {
  prefs.putUChar("chmin", chMin);
  prefs.putUChar("chmax", chMax);
  prefs.putUShort("age", noteOnMaxAgeMs);
}
static void loadSettings() {
  chMin = prefs.getUChar("chmin", 1);
  chMax = prefs.getUChar("chmax", 13);
  if (chMin < 1 || chMax > 13 || chMin > chMax) { chMin = 1; chMax = 13; }
  noteOnMaxAgeMs = prefs.getUShort("age", ENM_NOTEON_MAX_AGE_MS);
  stallReboots = prefs.getULong("stallboot", 0);
}

// ---- Configuration commands (called from mimuz_link_cmd.h) ----
static void cmdPrintSettings(Print &o) {
  o.printf(" channel_min=%u channel_max=%u noteon_max_age_ms=%u", chMin, chMax, noteOnMaxAgeMs);
}
static bool cmdMac(const char *s, uint8_t mac[6]) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
  return true;
}
static void cmdExec(const Cmd &c, Print &o) {
  uint32_t v;
  if (cmdIs(c, "port")) {  // port <n> <command...> : relay to the client on that port
    if (c.argc < 3 || !cmdUint(c.argv[1], v) || v < 1 || v > NUM_PORTS) { cmdErr(c, o, "usage:port_<1-8>_<command>"); return; }
    if (!ports[v - 1].active) { cmdErr(c, o, "port_not_connected"); return; }
    if (relayId[0]) { cmdErr(c, o, "busy"); return; }
    uint8_t *buf = relayBuf;
    BufPrint bp((char *)buf + ENM_HDR_LEN, ENM_CMD_TEXT_MAX + 1);
    bp.printf("@%s", c.id);
    for (int i = 2; i < c.argc; i++) bp.printf(" %s", c.argv[i]);
    enm_pkt_t h;
    enm_init(h, PKT_CMD);
    h.len = (uint8_t)bp.length();
    h.seq = ++relaySeq;   // stays the same on resends (so the client can recognize the same command)
    h.t_us = now_us();
    memcpy(buf, &h, ENM_HDR_LEN);
    relayReplyReady = false;
    relayLen = ENM_HDR_LEN + bp.length();
    memcpy(relayMac, ports[v - 1].mac, 6);
    if (enm_radio_send(relayMac, buf, relayLen) != ESP_OK) { cmdErr(c, o, "send_failed"); return; }
    strlcpy(relayId, c.id, sizeof(relayId));
    relayLastSendMs = millis();
    relayDeadline = relayLastSendMs + RELAY_TIMEOUT_MS;  // the reply is printed in loop(). If none arrives, resend; if still none, timeout
  } else if (cmdIs(c, "info")) {
    int used = 0;
    for (int i = 0; i < NUM_PORTS; i++) used += ports[i].active;
    o.printf("@%s ok name=%s kind=bridge fw=%s radio_channel=%u ports_used=%d ports=%d registered=%d\n", c.id, usbProductName,
             MIMUZ_LINK_VERSION, wifiChannel, used, NUM_PORTS, regCount);
  } else if (cmdIs(c, "get")) {
    if (c.argc == 1) {
      o.printf("@%s ok", c.id);
      cmdPrintSettings(o);
      o.print("\n");
    } else if (!strcmp(c.argv[1], "channel_min")) o.printf("@%s ok channel_min=%u\n", c.id, chMin);
    else if (!strcmp(c.argv[1], "channel_max")) o.printf("@%s ok channel_max=%u\n", c.id, chMax);
    else if (!strcmp(c.argv[1], "noteon_max_age_ms")) o.printf("@%s ok noteon_max_age_ms=%u\n", c.id, noteOnMaxAgeMs);
    else if (!strcmp(c.argv[1], "tx_fail_max")) o.printf("@%s ok tx_fail_max=%u\n", c.id, txFailMax);
    else if (!strcmp(c.argv[1], "tx_probe")) o.printf("@%s ok tx_probe=%u\n", c.id, txProbe ? 1 : 0);
    else if (!strcmp(c.argv[1], "tx_impl")) o.printf("@%s ok tx_impl=%u\n", c.id, txImplOld ? 0 : 1);
    else if (!strcmp(c.argv[1], "tx_parallel")) o.printf("@%s ok tx_parallel=%u\n", c.id, txParallel ? 1 : 0);
    else if (!strcmp(c.argv[1], "send_lock")) o.printf("@%s ok send_lock=%u\n", c.id, enmSendLock ? 1 : 0);
    else if (!strcmp(c.argv[1], "trace_status")) o.printf("@%s ok trace_status=%u\n", c.id, traceStatus);
    else if (!strcmp(c.argv[1], "retry_limit"))
      o.printf("@%s ok retry_limit=%u short=%u long=%u default_short=%u default_long=%u\n", c.id, retryLimit, enm_retry_short(),
               enm_retry_long(), enmRetryDefShort, enmRetryDefLong);
    else if (!strcmp(c.argv[1], "slow_tx_ms")) o.printf("@%s ok slow_tx_ms=%lu\n", c.id, (unsigned long)(slowTxUs / 1000));
    else cmdErr(c, o, "unknown_key");
  } else if (cmdIs(c, "set")) {
    if (c.argc < 3 || !cmdUint(c.argv[2], v)) { cmdErr(c, o, "usage:set_<key>_<value>"); return; }
    if (!strcmp(c.argv[1], "channel_min")) {  // takes effect at the next survey (restart or resurvey)
      if (v < 1 || v > chMax) { cmdErr(c, o, "bad_value"); return; }
      chMin = (uint8_t)v;
      saveSettings();
      o.printf("@%s ok channel_min=%u\n", c.id, chMin);
    } else if (!strcmp(c.argv[1], "channel_max")) {
      if (v > 13 || v < chMin) { cmdErr(c, o, "bad_value"); return; }
      chMax = (uint8_t)v;
      saveSettings();
      o.printf("@%s ok channel_max=%u\n", c.id, chMax);
    } else if (!strcmp(c.argv[1], "noteon_max_age_ms")) {
      if (v > 2000) { cmdErr(c, o, "bad_value"); return; }
      noteOnMaxAgeMs = (uint16_t)v;
      saveSettings();
      o.printf("@%s ok noteon_max_age_ms=%u\n", c.id, noteOnMaxAgeMs);
    } else if (!strcmp(c.argv[1], "tx_fail_max")) {  // for comparison (not saved)
      if (v < 1 || v > 50) { cmdErr(c, o, "bad_value"); return; }
      txFailMax = (uint8_t)v;
      o.printf("@%s ok tx_fail_max=%u\n", c.id, txFailMax);
    } else if (!strcmp(c.argv[1], "tx_probe")) {  // for comparison (not saved)
      txProbe = v != 0;
      o.printf("@%s ok tx_probe=%u\n", c.id, txProbe ? 1 : 0);
    } else if (!strcmp(c.argv[1], "tx_impl")) {  // diagnostic: 0 = single-queue send task, 1 = per-client send task (not saved)
      txImplOld = v == 0;
      txTaskH = txImplOld ? txTaskOldH : txTaskNewH;
      o.printf("@%s ok tx_impl=%u\n", c.id, txImplOld ? 0 : 1);
    } else if (!strcmp(c.argv[1], "tx_parallel")) {  // diagnostic: 1 = hand packets to the radio in parallel, one per client (not saved)
      txParallel = v != 0;
      o.printf("@%s ok tx_parallel=%u\n", c.id, txParallel ? 1 : 0);
    } else if (!strcmp(c.argv[1], "send_lock")) {  // diagnostic: 1 = hand packets to the radio under the driver's lock (not saved)
      enmSendLock = v != 0;
      o.printf("@%s ok send_lock=%u\n", c.id, enmSendLock ? 1 : 0);
    } else if (!strcmp(c.argv[1], "retry_limit")) {  // radio retry limit, 0 = the library's value (not saved)
      if (v > 31) { cmdErr(c, o, "bad_value"); return; }
      retryLimit = (uint8_t)v;
      enm_retry_set(retryLimit);
      o.printf("@%s ok retry_limit=%u short=%u long=%u\n", c.id, retryLimit, enm_retry_short(), enm_retry_long());
    } else if (!strcmp(c.argv[1], "slow_tx_ms")) {  // diagnostic: record radio sends whose result took this long, 0 = off (not saved)
      if (v > 1000) { cmdErr(c, o, "bad_value"); return; }
      slowTxUs = v * 1000;
      o.printf("@%s ok slow_tx_ms=%lu\n", c.id, (unsigned long)v);
    } else if (!strcmp(c.argv[1], "trace_status")) {  // diagnostic: status byte of the messages to trace, 0 = off (not saved)
      if (v != 0 && (v < 0x80 || v > 0xEF)) { cmdErr(c, o, "bad_value"); return; }
      if (!traceSet((uint8_t)v)) { cmdErr(c, o, "no_memory"); return; }
      o.printf("@%s ok trace_status=%u\n", c.id, traceStatus);
    } else cmdErr(c, o, "unknown_key");
  } else if (cmdIs(c, "slowtx")) {  // radio sends whose result came late, oldest first; "slowtx clear" starts over
    if (c.argc >= 2 && !strcmp(c.argv[1], "clear")) {
      slowTxN = 0;
      o.printf("@%s ok cleared\n", c.id);
      return;
    }
    uint32_t n = slowTxN, from = n > SLOWTX_N ? n - SLOWTX_N : 0;
    for (uint32_t k = from; k < n; k++) {
      SlowTx e = slowTx[k % SLOWTX_N];
      o.printf("@%s + port=%d sent_us=%lu air_us=%lu ok=%u resend=%u timeout=%u len=%u rssi=%d nomem=%u\n", c.id, e.port + 1,
               (unsigned long)e.sentUs, (unsigned long)e.airUs, (e.flags & SLOWTX_OK) ? 1 : 0, (e.flags & SLOWTX_RESEND) ? 1 : 0,
               (e.flags & SLOWTX_TIMEOUT) ? 1 : 0, e.len, e.rssi, e.noMem);
    }
    o.printf("@%s ok count=%lu total=%lu now_us=%lu\n", c.id, (unsigned long)(n - from), (unsigned long)n, (unsigned long)now_us());
  } else if (cmdIs(c, "trace")) {  // slowest traced messages, slowest first; "trace clear" starts over
    if (c.argc >= 2 && !strcmp(c.argv[1], "clear")) {
      traceClear();
      o.printf("@%s ok cleared\n", c.id);
      return;
    }
    TraceTop t[TRACE_TOP];
    uint8_t n = traceTopN;
    memcpy(t, traceTop, sizeof(t));
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && t[j].totalUs > t[j - 1].totalUs; j--) { TraceTop x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
    for (int i = 0; i < n; i++)
      o.printf("@%s + port=%d key=%u total_us=%lu radio_us=%lu in_us=%lu\n", c.id, t[i].port + 1, t[i].key, (unsigned long)t[i].totalUs,
               (unsigned long)t[i].radioUs, (unsigned long)t[i].inUs);
    o.printf("@%s ok count=%d done=%lu missed=%lu p50_us=%lu p99_us=%lu max_us=%lu\n", c.id, n, (unsigned long)traceDoneN,
             (unsigned long)traceMiss, (unsigned long)(trace ? trace->h.pct(0.5) : 0), (unsigned long)(trace ? trace->h.pct(0.99) : 0),
             (unsigned long)(trace ? trace->h.mx : 0));
  } else if (cmdIs(c, "ports")) {  // connected ports, one per line
    int cnt = 0;
    for (int i = 0; i < NUM_PORTS; i++) {
      if (!ports[i].active) continue;
      o.printf("@%s + port=%d mac=" MACSTR_ " rx_channels=0x%04X phy_rate=%u idle_ms=%lu rssi=%d tx_fail=%lu\n", c.id, i + 1, MAC2STR_(ports[i].mac),
               ports[i].rxChMask, ports[i].rate, (unsigned long)(millis() - ports[i].lastRxMs), ports[i].rssi, (unsigned long)ports[i].txFailCnt);
      cnt++;
    }
    o.printf("@%s ok count=%d\n", c.id, cnt);
  } else if (cmdIs(c, "registry")) {  // registered clients, one per line
    for (int i = 0; i < regCount; i++)
      o.printf("@%s + mac=" MACSTR_ " port=%d\n", c.id, MAC2STR_(regMac[i]), portFind(regMac[i]) + 1);
    o.printf("@%s ok count=%d\n", c.id, regCount);
  } else if (cmdIs(c, "unregister")) {  // unregister <mac|all>
    uint8_t mac[6];
    if (c.argc < 2) { cmdErr(c, o, "usage:unregister_<mac|all>"); return; }
    if (!strcmp(c.argv[1], "all")) {
      clearRegistry();
      o.printf("@%s ok registered=0\n", c.id);
    } else if (cmdMac(c.argv[1], mac)) {
      int r = regFind(mac);
      if (r < 0) { cmdErr(c, o, "not_registered"); return; }
      int pt = portFind(mac);
      if (pt >= 0) portFree(pt, "unregistered");
      memmove(regMac[r], regMac[r + 1], (regCount - r - 1) * 6);
      regCount--;
      if (regNext > regCount) regNext = 0;
      regSave();
      o.printf("@%s ok registered=%d\n", c.id, regCount);
    } else cmdErr(c, o, "bad_value");
  } else if (cmdIs(c, "radio_restart")) {  // diagnostic: restart ESP-NOW (wifi: also Wi-Fi, driver: also reinitialize the Wi-Fi driver)
    if (c.argc > 1 && !strcmp(c.argv[1], "stall")) {  // simulate a stall; it should be detected and the radio restarted automatically
      debugStallFlag = true;
      debugStallHold = c.argc > 2 && !strcmp(c.argv[2], "hold");  // hold: the stall survives restarts, so recovery escalates
      o.printf("@%s ok simulating_stall\n", c.id);
      return;
    }
    o.printf("@%s ok restarting_radio\n", c.id);
    radioRestart(c.argc > 1 && !strcmp(c.argv[1], "driver") ? 2 : c.argc > 1 && !strcmp(c.argv[1], "wifi") ? 1 : 0);
  } else if (cmdIs(c, "resurvey")) {  // re-measure and re-choose the channel (takes about 12 s)
    uint8_t to = resurvey();
    o.printf("@%s ok radio_channel=%u\n", c.id, to);
  } else if (cmdIs(c, "switch_channel")) {
    if (c.argc < 2 || !cmdUint(c.argv[1], v) || v < 1 || v > 13) { cmdErr(c, o, "bad_value"); return; }
    switchChannel((uint8_t)v);
    o.printf("@%s ok radio_channel=%u\n", c.id, wifiChannel);
  } else if (cmdIs(c, "survey")) {  // latest survey results, one per line
    for (int ch = 1; ch <= 13; ch++)
      o.printf("@%s + ch=%d busy=%.1f peak=%.1f score=%.1f\n", c.id, ch, svBusy[ch] / 10.0, svPeak[ch] / 10.0, svScore[ch]);
    o.printf("@%s ok radio_channel=%u\n", c.id, wifiChannel);
  } else cmdErr(c, o, "unknown_command");
}
static void cmdAfter() {}

// ---- Start ----
// Call once in setup(). Measures congestion on all channels at startup, so it takes about 12 s
static void begin() {
  USB.begin();
  prefs.begin("enm", false);
  loadSettings();  // load saved settings (allowed radio channel range etc.)

  rxq = xQueueCreate(192, sizeof(RxItem));  // large enough not to overflow even with consecutive batched arrivals (up to 64 messages)
  logq = xQueueCreate(32, sizeof(LogItem));
  evlogq = xQueueCreate(16, sizeof(EvLog));
  txq = xQueueCreate(TXQ_LEN, sizeof(TxItem));
  regLoad();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  enm_retry_set(retryLimit);
  // Measure congestion on all channels at startup and choose the channel (about 12 s).
  // After a restart caused by a stalled radio, keep the channel the clients are on
  uint32_t svT0 = millis();
  uint8_t prevCh = prefs.getUChar("ch", 0);
  uint32_t tag = stallBootTag;
  stallBootTag = 0;
  uint8_t keepCh = tag & 0xFF;
  if (esp_reset_reason() == ESP_RST_SW && (tag & ~0xFFu) == STALL_BOOT_MAGIC && keepCh >= 1 && keepCh <= 13) {
    wifiChannel = keepCh;
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    memcpy(keptMac, stallBootMac, sizeof(keptMac));
    keptAny = true;
    evlog("restarted after a radio stall: keeping channel %d and the port numbers", wifiChannel);
  } else {
    channelSurvey();
    wifiChannel = channelChoose(prevCh);
    if (wifiChannel != prevCh) prefs.putUChar("ch", wifiChannel);
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    evlog("channel %d selected (previous %d), survey took %lu ms", wifiChannel, prevCh, (unsigned long)(millis() - svT0));
  }
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  // The Wi-Fi task runs on core0. Forwarding tasks run on core1 at high priority
  xTaskCreatePinnedToCore(fwdTask, "mlink_fwd", 4096, NULL, 20, NULL, 1);
  xTaskCreatePinnedToCore(usbInTask, "mlink_usb", 4096, NULL, 19, &usbInTaskH, 1);
  xTaskCreatePinnedToCore(txTask, "mlink_tx", 4096, NULL, 19, &txTaskNewH, 1);
  xTaskCreatePinnedToCore(txTaskOld, "mlink_tx_old", 4096, NULL, 19, &txTaskOldH, 1);
  txTaskH = txImplOld ? txTaskOldH : txTaskNewH;
}


// ---- Periodic processing ----
// Call from loop() (every few ms). Frees ports, saves the registry, prints logs
static void loop() {
  uint32_t ms = millis();

  // Print replies to relayed configuration commands to serial (timeout if none arrives)
  if (relayId[0]) {
    if (relayReplyReady) {
      Serial.println(relayReply);
      relayReplyReady = false;
      relayId[0] = 0;
    } else if ((int32_t)(ms - relayDeadline) > 0) {
      Serial.printf("@%s err timeout\n", relayId);
      relayId[0] = 0;
    } else if (ms - relayLastSendMs >= RELAY_RESEND_MS) {  // no reply -> resend the same packet
      enm_radio_send(relayMac, relayBuf, relayLen);
      relayLastSendMs = ms;
      relayResends++;
    }
  }

  // Free ports of C6 units from which nothing arrives anymore
  for (int i = 0; i < NUM_PORTS; i++)
    if (ports[i].active && ms - ports[i].lastRxMs > ENM_PORT_TIMEOUT_MS && (int32_t)(ms - ports[i].lastRxMs) > 0) portFree(i, "timeout");

  if (regDirty) {
    regDirty = false;
    regSave();
  }

  EvLog ev;
  while (xQueueReceive(evlogq, &ev, 0) == pdTRUE) Serial.printf("[%lu] %s\n", (unsigned long)ms, ev.s);

  LogItem l;
  while (xQueueReceive(logq, &l, 0) == pdTRUE) {
    Serial.printf("[%lu] Port %d MIDI %02X %02X %02X seq=%u rssi=%d  C6:in->send=%uus  radio(相対)=%.2fms  S3:cb->usb=%luus\n",
                  (unsigned long)ms, l.port + 1, l.m[0], l.m[1], l.m[2], l.seq, l.rssi, l.aux, l.rel / 1000.0,
                  (unsigned long)l.cb2usb);
  }
}

}  // namespace mlink

// Called by TinyUSB when USB-MIDI is received
extern "C" void tud_midi_rx_cb(uint8_t itf) {
  (void)itf;
  mlink::usbRxNotify();
}
