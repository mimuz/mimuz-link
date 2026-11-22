/*

 mimuz_link_proto mimuz-link packet format and shared constants

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing
 - 2026/11/21 : Hand packets to the radio under the Wi-Fi driver's global lock (enm_radio_send)

*/
// mimuz-link: protocol definitions (shared by client and bridge)
// Packet format, packet types and constants for carrying MIDI over ESP-NOW. Device-specific (application) logic is not included.
#pragma once
#include <stdint.h>
#include <string.h>
#include <Print.h>
#include <esp_now.h>
#include <atomic>
#include <Arduino.h>
#include <esp_private/wifi_os_adapter.h>

#define ENM_MAGIC 0xE7
#define ENM_VERSION 8
// Carried messages:
//   - Channel messages and messages without a channel (real-time messages such as clock, song position, etc.). SysEx is not carried.
//     Inside a packet every message occupies a 3-byte slot (unused bytes are 0). The actual length is determined by the status byte.
// Default PHY rate index: 6M (1). At close range it is only about 0.05 ms slower one way than 24M (2),
// but at long range (-84 dBm) 24M dropped packets while 6M did not.
#define ENM_RATE_DEFAULT 1
// Detecting and recovering from a stalled radio driver:
//   If the send-complete callback (which should arrive whether or not delivery succeeded) is missing ENM_STALL_TIMEOUTS times
//   in a row, ESP-NOW is restarted. If it happens again within ENM_STALL_WIFI_WINDOW_MS of that restart, Wi-Fi is restarted too.
//   Restarts are never done at intervals shorter than ENM_STALL_BACKOFF_MS.
#define ENM_STALL_TIMEOUTS 5
#define ENM_STALL_BACKOFF_MS 5000
#define ENM_STALL_WIFI_WINDOW_MS 30000
//   If the driver refuses every packet (NO_MEM) for 1 s, restarting ESP-NOW or Wi-Fi does not help, so the Wi-Fi driver is
//   torn down and initialized again right away.
//   If no send-complete callback at all arrives between restarts, the restarts escalate and are retried after
//   ENM_STALL_RETRY_MS instead of ENM_STALL_BACKOFF_MS: after ENM_STALL_REINIT_AFTER such restarts in a row the Wi-Fi driver
//   is initialized again, and after ENM_STALL_REBOOT_AFTER the device itself is restarted.
//   Initializing the driver again after a real stall loses some internal memory, so when less than ENM_STALL_HEAP_MIN bytes
//   of internal memory are free the device is restarted instead.
#define ENM_STALL_RETRY_MS 1000
#define ENM_STALL_REINIT_AFTER 1
#define ENM_STALL_REBOOT_AFTER 3
#define ENM_STALL_HEAP_MIN 24000
#define MIMUZ_LINK_VERSION "0.1.0"
// Config command relay:
//   The bridge forwards a config command received over serial to the client on the given port with PKT_CMD.
//   The client executes the command and returns a single reply line with PKT_CMD_REPLY.
//   Both consist of the 12-byte header (4th byte = character count) followed by the string (not terminated).
#define ENM_CMD_TEXT_MAX 180
// Distribution by receive channel:
//   The bridge is treated as "one MIDI cable that everyone is connected to". Each receiving C6 announces which MIDI channels it listens to.
//   - The C6 sends its listen-channel mask on every connect (CONNECT seq) and whenever it changes (PKT_SUBSCRIBE aux).
//     The mask is 16 bits: bit0 = channel 1 ... bit15 = channel 16. 0xFFFF = OMNI, 0 = listen to nothing (default).
//   - Channel messages received from a C6 are passed to the PC (USB) by the bridge.
//     In addition, they are sent to every other C6 listening on that channel. They are not sent back to the sender.
//   - Messages coming from the PC are delivered only to the C6 on the port they were sent to (they are not distributed).
//   - The bridge stores nothing. There is no routing configuration.
//   - For testing: setting bit0 of len makes a C6 also receive what it sent itself (to measure two-hop latency with a single C6).
#define ENM_SUB_SELF 0x01
// Note Off retransmission:
//   Among packets that were not delivered even after the radio layer's retries (send-complete callback reported failure),
//   only Note Off messages are resent by the application layer.
//   Other messages (Note On, CC, program change, etc.) are not resent. No decision is made by CC number.
//   Resends use PKT_MIDI_RETRY: the 12-byte header (4th byte = count) followed by entries of [seq low, seq high, 3 MIDI bytes].
//   The receiver accepts only entries whose sequence number is newer than the last one it has received.
//   (So that a message that actually arrived, with only its acknowledgement lost, is not processed twice.)
//   The sender does not send new messages until the resend is done, so the order is never swapped.
#define ENM_RETRY_MAX 40           // number of entries one resend packet can carry (12 + 5x40 = 212 bytes)
#define ENM_RETRY_GIVEUP_MS 1000   // give up if still not delivered after resending for this long
// Dropping stale Note On:
//   A Note On that has waited in the send queue longer than this is dropped instead of sent (a note sounding late is harmful).
//   Note Off is always sent, however long it has waited. Other messages (CC, etc.) are sent even if delayed.
//   0 disables dropping. Can be changed from the PC with CC ch16 #108 = value x 10 ms (applies to both the C6 and the bridge).
#define ENM_NOTEON_MAX_AGE_MS 80
static inline bool enm_is_note_on(const uint8_t *m) { return (m[0] & 0xF0) == 0x90 && m[2] > 0; }
// Channel selection:
//   At startup the bridge measures how busy every channel is and picks a quiet one (the chosen channel is saved).
//   CONNECT_OK carries the bridge's current channel in seq. If the C6 gets no reply on its saved channel,
//   it sweeps all channels sending CONNECT, then moves to the channel given in the reply and saves it.
//   When the bridge changes channel during operation, it first sends CH_SWITCH (aux = new channel) to the connected C6s.
// MIDI batching:
//   PKT_MIDI is the 12-byte header (magic, ver, type, count, seq, aux, t_us) followed by MIDI messages, 3 bytes each.
//   The 4th byte (len) is the number of messages (1 to ENM_BATCH_MAX). seq is the sequence number of the first message; each following one is +1.
//   A single message gives a 15-byte packet. Whatever is waiting in the send queue is packed into one packet.
#define ENM_HDR_LEN 12
#define ENM_BATCH_MAX 64  // 12 + 3x64 = 204 bytes (ESP-NOW limit is 250)
#define ENM_CHANNEL 6  // Wi-Fi channel used by the bridge (S3). During pairing the C6 searches all channels and saves the one it found

// Registration (pairing) and connection
//  - Pairing: an unregistered C6 broadcasts PAIR_REQ while stepping through channels 1-13.
//             The bridge accepts requests only from a C6 with a strong signal (i.e. right next to it), saves its MAC and replies PAIR_ACK.
//             The C6 saves the bridge's MAC and channel. Pairing is independent of ports.
//  - Connect: on every power-up the C6 sends CONNECT to the saved bridge.
//             If registered, the bridge assigns the lowest free port number and replies CONNECT_OK.
//             If not registered it replies REJECT(reason = unregistered); if all ports are taken, REJECT(reason = full).
//  - Keepalive: the C6 sends KEEPALIVE only after a period with nothing sent. The bridge frees the port if nothing arrives for a while.
#define ENM_PAIR_RSSI_MIN (-55)    // minimum signal strength to accept pairing [dBm] (only devices held close pass)
// ESP-NOW has no notion of a connection, so disconnection is detected by "nothing has arrived for a while".
// The C6 sends KEEPALIVE only when it has sent nothing for ENM_KEEPALIVE_MS (never while being played).
#define ENM_KEEPALIVE_MS 5000
#define ENM_PORT_TIMEOUT_MS 16000  // the bridge frees the port if nothing arrives for this long (3 KEEPALIVE periods)
#define ENM_REJECT_UNREGISTERED 1
#define ENM_REJECT_FULL 2

enum : uint8_t {
  PKT_MIDI = 3,     // both ways       : MIDI messages
  PKT_PING = 4,     // C6 -> S3        : for radio RTT measurement
  PKT_PONG = 5,     // S3 -> C6        : PING echo
  PKT_CFG = 6,      // C6 -> S3        : aux = PHY rate index
  PKT_PAIR_REQ = 10,   // C6 -> broadcast : pairing request
  PKT_PAIR_ACK = 11,   // S3 -> C6        : paired
  PKT_CONNECT = 12,    // C6 -> S3        : connect request (aux = PHY rate index, seq = listen-channel mask, len = flags)
  PKT_CONNECT_OK = 13, // S3 -> C6        : aux = assigned port number (0-based), seq = bridge channel
  PKT_REJECT = 14,     // S3 -> C6        : aux = reason (ENM_REJECT_*)
  PKT_KEEPALIVE = 15,  // C6 -> S3        : keep the connection alive
  PKT_CH_SWITCH = 16,  // S3 -> C6        : aux = channel about to switch to
  PKT_MIDI_RETRY = 17, // both ways       : resend of undelivered Note Off
  PKT_SUBSCRIBE = 18,  // C6 -> S3        : aux = listen-channel mask (16 bit), len = flags (ENM_SUB_*)
  PKT_CMD = 19,        // S3 -> C6        : relayed config command (one line of text)
  PKT_CMD_REPLY = 20,  // C6 -> S3        : its reply (one line of text)
};

typedef struct __attribute__((packed)) {
  uint8_t magic, ver, type, len;  // for PKT_MIDI, len = number of messages
  uint16_t seq;                   // per-type sequence number
  uint16_t aux;                   // MIDI(C6->S3): input->send [us], CFG/CONNECT: rate index, CONNECT_OK: port, REJECT: reason
  uint32_t t_us;                  // sender esp_timer (low 32 bit). PONG: echoed PING t_us
  uint8_t midi[3];
} enm_pkt_t;  // 15 bytes

static inline bool enm_valid(const uint8_t *d, int n) {
  return n >= (int)sizeof(enm_pkt_t) && d[0] == ENM_MAGIC && d[1] == ENM_VERSION;
}
static inline void enm_init(enm_pkt_t &p, uint8_t type) {
  memset(&p, 0, sizeof(p));
  p.magic = ENM_MAGIC;
  p.ver = ENM_VERSION;
  p.type = type;
}

// Length of a MIDI message (determined by the status byte). 0 for what is not carried (SysEx, undefined, data bytes)
static inline uint8_t midi_len(uint8_t st) {
  if (st < 0x80) return 0;
  if (st < 0xF0) {
    uint8_t h = st & 0xF0;
    return (h == 0xC0 || h == 0xD0) ? 2 : 3;
  }
  switch (st) {
    case 0xF1: case 0xF3: return 2;  // MTC quarter frame, song select
    case 0xF2: return 3;             // song position
    case 0xF6: return 1;             // tune request
    case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF: return 1;  // clock, start, continue, stop, active sensing, reset
    default: return 0;               // F0/F7 (SysEx) and undefined
  }
}

// Whether this is a Note Off (per the MIDI spec, Note On with velocity 0 equals Note Off)
static inline bool enm_is_note_off(const uint8_t *m) {
  uint8_t h = m[0] & 0xF0;
  return h == 0x80 || (h == 0x90 && m[2] == 0);
}
// Whether sequence number s is newer than ref (wraps around at 16 bits)
static inline bool enm_seq_newer(uint16_t s, uint16_t ref) {
  uint16_t d = (uint16_t)(s - ref);
  return d != 0 && d < 0x8000;
}
// Determine which packet type a send-complete callback refers to. Returns 0 if unknown.
// Only fixed positions in the sent frame are examined. An ESP-NOW frame is the 24-byte 802.11 header followed by
// 15 bytes [category 1][OUI 3][random 4][element ID 1][length 1][OUI 3][type 1][version 1], then the sent data.
// (Searching for magic and ver instead would misdetect when the 4 random bytes happen to match; about 1 in 20,000.)
#define ENM_CB_OFF (24 + 15)
static inline uint8_t enm_cb_type(const esp_now_send_info_t *info) {
  if (!info || !info->data || info->data_len < 15 + 3) return 0;
  const uint8_t *d = info->data + ENM_CB_OFF;
  if (d[0] == ENM_MAGIC && d[1] == ENM_VERSION) return d[2];
  return 0;
}

// ---- Handing a packet to the radio ----
// esp_now_send() puts the frame on the Wi-Fi driver's transmit cache queue without holding the driver's global lock, while
// the driver task takes frames off that queue under the lock. When the two meet, the frame (and every later one) can be
// lost from the queue: no send-complete notification ever comes, the driver soon refuses everything (NO_MEM), and only
// initializing the driver again recovers it. Sending is therefore done under the same lock. The lock is recursive and is
// always taken before the ESP-NOW lock (as esp_now_send() itself does), so the order of the two locks never inverts.
// While the driver is torn down and initialized again (the lock is deleted and created again), nothing is sent:
// call enm_radio_block() before tearing down and enm_radio_unblock() after ESP-NOW is up again.
extern "C" {
extern void *g_wifi_global_lock;
extern wifi_osi_funcs_t *g_osi_funcs_p;
}
static volatile bool enmSendLock = true;  // diagnostic: false = call esp_now_send() without the lock
static std::atomic<int> enmSendUsers{0};
static std::atomic<bool> enmRadioDown{false};
static esp_err_t IRAM_ATTR enm_radio_send(const uint8_t *mac, const uint8_t *d, size_t n) {
  enmSendUsers++;
  if (enmRadioDown) {
    enmSendUsers--;
    return ESP_ERR_ESPNOW_NOT_INIT;
  }
  void *lk = enmSendLock ? g_wifi_global_lock : nullptr;
  if (lk) g_osi_funcs_p->_mutex_lock(lk);
  esp_err_t e = esp_now_send(mac, d, n);
  if (lk) g_osi_funcs_p->_mutex_unlock(lk);
  enmSendUsers--;
  return e;
}
// Stop new sends and wait (up to 100 ms) for sends in progress to finish
static void enm_radio_block() {
  enmRadioDown = true;
  uint32_t t0 = millis();
  while (enmSendUsers.load() > 0 && millis() - t0 < 100) vTaskDelay(1);
}
static void enm_radio_unblock() { enmRadioDown = false; }

// ---- PHY rate (esp_now_set_peer_rate_config) ----
// 0: 11b 1Mbps (ESP-NOW default)  1: 11g 6Mbps  2: 11g 24Mbps  3: HT20 MCS4(39Mbps)
#define ENM_RATE_NUM 4
static inline const char *enm_rate_name(uint8_t i) {
  static const char *n[ENM_RATE_NUM] = {"11b 1M", "11g 6M", "11g 24M", "HT20 MCS4"};
  return i < ENM_RATE_NUM ? n[i] : "?";
}
static inline esp_err_t enm_apply_rate(const uint8_t *mac, uint8_t i) {
  esp_now_rate_config_t c;
  memset(&c, 0, sizeof(c));
  switch (i) {
    case 1:  c.phymode = WIFI_PHY_MODE_11G; c.rate = WIFI_PHY_RATE_6M; break;
    case 2:  c.phymode = WIFI_PHY_MODE_11G; c.rate = WIFI_PHY_RATE_24M; break;
    case 3:  c.phymode = WIFI_PHY_MODE_HT20; c.rate = WIFI_PHY_RATE_MCS4_LGI; break;
    default: c.phymode = WIFI_PHY_MODE_11B; c.rate = WIFI_PHY_RATE_1M_L; break;
  }
  return esp_now_set_peer_rate_config(mac, &c);
}

// ---- Device-unique ID (11 characters) ----
// Same conversion as makeUUID in tadfmac/web-pio: 8 bytes (the chip's unique 6-byte MAC + 2 zero bytes) turned into 11 characters of 6 bits each.
// Characters are 0-9, A-Z, a-z. 62 and 63 use '-' '_' instead of '@' '&' (same substitution as the ESP32C6 version of web-pio).
#include <esp_mac.h>
static inline void enm_make_uid(char out[12]) {
  uint8_t b[8] = {0};
  esp_efuse_mac_get_default(b);  // b[0..5] = MAC
  uint32_t hi = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  uint32_t lo = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
  uint8_t v[11] = {
    (uint8_t)(hi >> 26), (uint8_t)((hi >> 20) & 0x3F), (uint8_t)((hi >> 14) & 0x3F), (uint8_t)((hi >> 8) & 0x3F),
    (uint8_t)((hi >> 2) & 0x3F), (uint8_t)(((hi & 3) << 4) | (lo >> 28)), (uint8_t)((lo >> 22) & 0x3F),
    (uint8_t)((lo >> 16) & 0x3F), (uint8_t)((lo >> 10) & 0x3F), (uint8_t)((lo >> 4) & 0x3F), (uint8_t)(lo & 0x0F)};
  for (int i = 0; i < 11; i++) {
    uint8_t c = v[i];
    out[i] = c < 10 ? '0' + c : c < 36 ? 'A' + (c - 10) : c < 62 ? 'a' + (c - 36) : c == 62 ? '-' : '_';
  }
  out[11] = 0;
}
