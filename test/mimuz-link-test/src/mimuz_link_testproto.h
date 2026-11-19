/*

 mimuz_link_testproto mimuz-link test message definitions

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing

*/
// Test MIDI message definitions (not part of mimuz-link; used by the test sketches and the PC scripts)
#pragma once

// ---- test MIDI messages (must match the PC script) ----
#define TM_PING_RADIO 0xAF  // PolyAT ch16 : PC -> S3 -> echoed by C6 (with BLE, echoed by C6 directly)
#define TM_PING_USB 0xAD    // PolyAT ch14 : PC -> echoed by S3 (baseline for the USB segment only)
#define TM_BURST 0xAE       // PolyAT ch15 : burst sent by C6 at a fixed period
#define TM_CTRL 0xBF        // CC ch16     : #118 = count/10, #119 = period ms (starts on receipt)
// Receive-side (PC -> C6) measurement: the PC sends TM_RXTEST at a fixed interval and the C6 records arrival times.
// When the PC sends CC ch16 #127 = 1, the C6 reports its records as 3 messages each (0 = clear records; end marker is #127 = 127).
#define TM_RXTEST 0xA9      // PolyAT ch10 : seq (low 7 bits, high 7 bits). C6 only records it and does not reply
#define TM_RXDUMP_SEQ 0xAC  // PolyAT ch13 : seq
#define TM_RXDUMP_T0 0xAB   // PolyAT ch12 : arrival time [us] (relative to the first record), bits 0-13
#define TM_RXDUMP_T1 0xAA   // PolyAT ch11 : same, bits 14-27
// Two-hop (C6 -> bridge -> C6) latency measurement: the C6 sends TM_LOOP and measures, with its own clock, the time until it comes back via the bridge.
// Set the C6 to listen on channel 9 (and to receive its own messages). Start with CC ch16 #116 = interval ms (count via #118).
#define TM_LOOP 0xA8        // PolyAT ch9  : seq (low 7 bits, high 7 bits)
