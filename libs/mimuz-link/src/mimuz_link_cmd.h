/*

 mimuz_link_cmd mimuz-link USB-serial config command helpers

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing

*/
// mimuz-link: shared part of the config commands (USB serial). Used by both the client and the bridge
//
// Configuration is exchanged over USB serial as single plain-text lines (MIDI flows over USB-MIDI and radio; serial is for configuration only).
//   PC -> device : @<id> <command> <args...>
//   device -> PC : @<id> ok <name=value ...>      success
//                  @<id> err <reason>             failure
//                  @<id> + <name=value ...>       intermediate line of a multi-line reply (an ok line comes last)
//   <id> is a sequence tag assigned by the PC (to match replies to requests). Any alphanumeric string is accepted.
//   Lines not starting with @ are log output (for humans to read).
// Example:
//   @1 info
//   @1 ok name=mi:muz:conESP32C6-j3f5__w9H50 kind=client fw=0.1.0 transport=espnow connected=1 port=1
//   @2 set rx_channels 0xFFFF
//   @2 ok rx_channels=0xFFFF
//
// On the bridge, prefixing a command with "port <n>" relays it over radio to the client on that port.
//   @5 port 2 set rx_channels 0x0001   ->   @5 ok rx_channels=0x0001
//
// Values set with set are saved to non-volatile memory immediately and loaded and applied at the next boot.
#pragma once
#include <Arduino.h>

namespace mlink {

// Print that writes into a string buffer (used to build relayed replies)
class BufPrint : public Print {
public:
  BufPrint(char *buf, size_t cap) : b(buf), cap(cap), n(0) { b[0] = 0; }
  size_t write(uint8_t ch) override {
    if (n + 1 >= cap) return 0;
    b[n++] = ch;
    b[n] = 0;
    return 1;
  }
  size_t length() const { return n; }

private:
  char *b;
  size_t cap, n;
};

struct Cmd {
  const char *id;  // id assigned by the PC
  int argc;        // number of command + arguments
  char *argv[10];  // argv[0] = command
};

// Device-specific command handling (in mimuz_link_client.h / mimuz_link_bridge.h)
static void cmdExec(const Cmd &c, Print &out);
// Called after a command has been handled (for actions such as reboot that must wait until the reply is out)
static void cmdAfter();

static inline bool cmdIs(const Cmd &c, const char *verb) { return c.argc > 0 && strcmp(c.argv[0], verb) == 0; }
static inline void cmdErr(const Cmd &c, Print &o, const char *reason) { o.printf("@%s err %s\n", c.id, reason); }
// Parse a number (decimal, or hex with 0x prefix)
static inline bool cmdUint(const char *s, uint32_t &v) {
  char *e;
  v = strtoul(s, &e, 0);
  return e != s && *e == 0;
}

// Handle one line (without the leading @) and write the reply to out
static void cmdRunLine(char *line, Print &out) {
  Cmd c;
  c.id = nullptr;
  c.argc = 0;
  char *save = nullptr;
  for (char *t = strtok_r(line, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
    if (!c.id) c.id = t;
    else if (c.argc < 10) c.argv[c.argc++] = t;
  }
  if (!c.id) return;
  if (c.argc == 0) { cmdErr(c, out, "empty"); return; }
  cmdExec(c, out);
}

// Feed one character read from serial. Returns true if it is part of a config line (starting with @), in which case it must not be passed to the sketch's own commands
#define MIMUZ_LINK_CMD_MAX 180
static char cmdLineBuf[MIMUZ_LINK_CMD_MAX + 1];
static uint8_t cmdLineLen = 0;
static bool cmdLineActive = false;
static bool serialInput(char ch, Print &out = Serial) {
  if (!cmdLineActive) {
    if (ch != '@') return false;
    cmdLineActive = true;
    cmdLineLen = 0;
    return true;
  }
  if (ch == '\n' || ch == '\r') {
    cmdLineBuf[cmdLineLen] = 0;
    cmdLineActive = false;
    cmdRunLine(cmdLineBuf, out);
    cmdAfter();
    return true;
  }
  if (cmdLineLen < MIMUZ_LINK_CMD_MAX) cmdLineBuf[cmdLineLen++] = ch;
  return true;
}

}  // namespace mlink
