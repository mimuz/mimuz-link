/*

 s3_bridge mimuz-link USB-MIDI bridge firmware (XIAO ESP32-S3)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing

*/
// Bridge (application): XIAO ESP32-S3
//
// Communication is left to mimuz-link (libs/mimuz-link). This sketch contains only the test-specific parts:
//   - handling of the test MIDI messages (mimuz_link_testproto.h; counterpart of latency_test.py on the PC)
//       PolyAT ch14 (0xAD) is not sent to the clients; the bridge echoes it back to the PC (baseline for the USB segment only)
//       CC ch16 #108 is also applied on the bridge as the age after which a stale Note On is dropped (value x 10 ms) (and sent on to the client)
//   - test serial commands
//       v=toggle showing all messages  s=statistics  c=clear statistics  l=list registrations and ports  x=erase all registrations
//       z=simulate 150 ms of no reception
//       h=channel survey results  H=re-survey and re-select  1-9,A-D=move manually to channel 1-13
//
// Board settings: USB Mode = USB-OTG (TinyUSB), USB CDC On Boot = Enabled
// Build (from examples/bridge): arduino-cli compile --libraries ../../libs --libraries ../../test --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default s3_bridge
#define MIMUZ_BRIDGE_NAME_PREFIX "mi:muz:bridgeESPS3"
#include <mimuz_link_bridge.h>
#include <mimuz_link_testproto.h>

// Inspect MIDI from the PC before it is sent to the clients
static bool onUsbMidi(uint8_t port, const uint8_t m[3]) {
  if (m[0] == TM_PING_USB) {  // send straight back to the port it came from
    mlink::usbSend(port, m);
    return true;
  }
  if (m[0] == TM_CTRL && m[1] == 108) mlink::setNoteOnMaxAgeMs((uint16_t)m[2] * 10);
  return false;
}

// Measurement messages are not logged one by one (v shows everything)
static bool logFilter(const uint8_t m[3]) {
  return !(m[0] == TM_PING_RADIO || m[0] == TM_BURST || m[0] == TM_LOOP);
}

void setup() {
  Serial.begin(115200);
  mlink::onUsbMidi(onUsbMidi);
  mlink::setMidiLogFilter(logFilter);
  mlink::begin();
}

void loop() {
  static uint32_t lastStat = 0, lastRx = 0;
  uint32_t ms = millis();
  mlink::loop();

  // after receiving something, print statistics once it has been quiet for 5 s
  uint32_t rx = mlink::rxMidiCount();
  if (rx != lastRx) { lastRx = rx; lastStat = ms; }
  else if (lastStat && ms - lastStat > 5000) { lastStat = 0; mlink::printStats(Serial); }

  while (Serial.available()) {
    char c = Serial.read();
    if (mlink::serialInput(c)) continue;  // lines starting with @ are config commands (handled by mimuz-link)
    if (c == 'v') { mlink::setVerbose(!mlink::isVerbose()); Serial.printf("verbose=%d\n", mlink::isVerbose()); }
    else if (c == 's') mlink::printStats(Serial);
    else if (c == 'l') mlink::printList(Serial);
    else if (c == 'h') mlink::printSurvey();
    else if (c == 'z') mlink::debugDeaf(150);
    else if (c == 'H') {
      uint8_t from = mlink::radioChannel();
      uint8_t to = mlink::resurvey();
      mlink::printSurvey();
      Serial.printf("channel %d -> %d\n", from, to);
    } else if ((c >= '1' && c <= '9') || (c >= 'A' && c <= 'D')) {
      uint8_t to = c <= '9' ? c - '0' : 10 + (c - 'A');
      Serial.printf("channel %d -> %d\n", mlink::radioChannel(), to);
      mlink::switchChannel(to);
    } else if (c == 'x') {
      mlink::clearRegistry();
      Serial.println("registry cleared");
    } else if (c == 'c') {
      mlink::clearStats();
      Serial.println("cleared");
    }
  }
  delay(2);
}
