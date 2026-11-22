/*

 mlping round trips to several mimuz-link ports through CoreMIDI, timed without Python

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing
 - 2026/11/22 : Stop after a given number of pings per port, or when interrupted (SIGINT / SIGTERM)

*/
// Sends numbered pings (status byte, then the number as two 7-bit data bytes) to each given port of a MIDI device and
// waits for the client to send them back, like multi_test.py, but without Python's thread scheduling in the timing:
//   - one sender thread with a real-time (time constraint) policy keeps one ping out per port, waiting a random gap
//     after each reply (or timeout) before the next
//   - replies are handled on CoreMIDI's own receive thread; both the packet's time stamp (set by CoreMIDI) and the time
//     the receive callback ran are kept
// One CSV line per ping goes to stdout:
//   port,key,send_ns,recv_ts_ns,recv_cb_ns,late_ns
//   send_ns     host time just before MIDISend
//   recv_ts_ns  time stamp of the reply packet (0 = lost)
//   recv_cb_ns  host time when the receive callback ran (0 = lost)
//   late_ns     how much later than planned the sender thread woke up to send this ping
// All times are in ns of the host clock (mach_absolute_time), the same clock as Python's time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)
// (not time.perf_counter_ns(), which counts from the start of the interpreter).
//
// It stops after <seconds>, after <count> pings on every port (0 = no limit), or on SIGINT / SIGTERM; in every case it
// waits for the pings that are out and then prints the results.
//
// usage: mlping <device name part> <ports e.g. 1,2,3> <seconds> [gap_min_ms gap_max_ms [timeout_ms [status [count]]]]
//
// build: clang -O2 -o mlping mlping.c -framework CoreMIDI -framework CoreFoundation

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PORTS 8
#define MAX_PINGS 65536

typedef struct {
  uint16_t key;
  uint64_t send, recvTs, recvCb, late;
} Ping;

typedef struct {
  int num;                     // port number as the OS names it (1..8)
  MIDIEndpointRef src, dst;
  Ping *pings;
  int n;                       // pings sent
  _Atomic int want;            // key of the ping that is out, -1 = none
  _Atomic uint64_t gotTs, gotCb;
  _Atomic int got;             // set by the receive callback when the wanted reply arrived
  uint64_t next;               // host time to send the next ping
  uint64_t deadline;           // host time to give up on the ping that is out
} Port;

static Port ports[MAX_PORTS];
static int nPorts;
static uint8_t status = 0xAF;
static mach_timebase_info_data_t tb;
static volatile sig_atomic_t stopNow;

static void onSignal(int sig) { (void)sig; stopNow = 1; }

static uint64_t ns2abs(uint64_t ns) { return ns * tb.denom / tb.numer; }
static uint64_t abs2ns(uint64_t a) { return a * tb.numer / tb.denom; }

static void readProc(const MIDIPacketList *pl, void *readRef, void *srcRef) {
  uint64_t now = mach_absolute_time();
  Port *p = (Port *)srcRef;
  const MIDIPacket *pk = &pl->packet[0];
  for (UInt32 i = 0; i < pl->numPackets; i++) {
    for (UInt16 j = 0; j + 2 < pk->length; j++) {
      if (pk->data[j] != status) continue;
      int key = pk->data[j + 1] | (pk->data[j + 2] << 7);
      if (key == atomic_load(&p->want) && !atomic_load(&p->got)) {
        atomic_store(&p->gotTs, pk->timeStamp ? pk->timeStamp : now);
        atomic_store(&p->gotCb, now);
        atomic_store(&p->got, 1);
      }
      j += 2;
    }
    pk = MIDIPacketNext(pk);
  }
}

static int nameMatches(MIDIEndpointRef e, const char *part, int num) {
  CFStringRef s = NULL;
  if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) != noErr || !s) return 0;
  char buf[256];
  int ok = 0;
  if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) {
    char tail[16];
    snprintf(tail, sizeof(tail), "Port %d", num);
    size_t L = strlen(buf), T = strlen(tail);
    while (L && buf[L - 1] == ' ') buf[--L] = 0;
    ok = strcasestr(buf, part) && L >= T && !strcmp(buf + L - T, tail);
  }
  CFRelease(s);
  return ok;
}

static void setRealtime(void) {
  // period 1 ms, up to 0.5 ms of computation in each, may be preempted
  thread_time_constraint_policy_data_t pol;
  pol.period = (uint32_t)ns2abs(1000000);
  pol.computation = (uint32_t)ns2abs(100000);
  pol.constraint = (uint32_t)ns2abs(500000);
  pol.preemptible = 1;
  if (thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                        THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
    fprintf(stderr, "mlping: could not set the real-time policy\n");
}

static uint64_t gapAbs(double gmin, double gmax) {
  double ms = gmin + (gmax - gmin) * (double)arc4random() / 4294967295.0;
  return ns2abs((uint64_t)(ms * 1e6));
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: mlping <device name part> <ports e.g. 1,2,3> <seconds> [gap_min_ms gap_max_ms [timeout_ms [status [count]]]]\n");
    return 2;
  }
  const char *part = argv[1];
  double secs = atof(argv[3]);
  double gmin = argc > 5 ? atof(argv[4]) : 10, gmax = argc > 5 ? atof(argv[5]) : 20;
  double tmo = argc > 6 ? atof(argv[6]) : 200;
  if (argc > 7) status = (uint8_t)strtol(argv[7], NULL, 0);
  int count = argc > 8 ? atoi(argv[8]) : 0;
  if (count <= 0 || count > MAX_PINGS) count = MAX_PINGS;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  mach_timebase_info(&tb);

  MIDIClientRef cl;
  MIDIPortRef in, out;
  if (MIDIClientCreate(CFSTR("mlping"), NULL, NULL, &cl) != noErr) { fprintf(stderr, "mlping: no MIDI client\n"); return 1; }
  MIDIInputPortCreate(cl, CFSTR("in"), readProc, NULL, &in);
  MIDIOutputPortCreate(cl, CFSTR("out"), &out);

  char list[64];
  snprintf(list, sizeof(list), "%s", argv[2]);
  for (char *t = strtok(list, ","); t && nPorts < MAX_PORTS; t = strtok(NULL, ",")) {
    Port *p = &ports[nPorts];
    p->num = atoi(t);
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++)
      if (nameMatches(MIDIGetSource(i), part, p->num)) p->src = MIDIGetSource(i);
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++)
      if (nameMatches(MIDIGetDestination(i), part, p->num)) p->dst = MIDIGetDestination(i);
    if (!p->src || !p->dst) { fprintf(stderr, "mlping: port %d of '%s' not found\n", p->num, part); return 1; }
    p->pings = calloc(MAX_PINGS, sizeof(Ping));
    atomic_store(&p->want, -1);
    MIDIPortConnectSource(in, p->src, p);
    nPorts++;
  }

  setRealtime();
  uint64_t t0 = mach_absolute_time(), end = t0 + ns2abs((uint64_t)(secs * 1e9)), tmoAbs = ns2abs((uint64_t)(tmo * 1e6));
  for (int i = 0; i < nPorts; i++) ports[i].next = t0 + gapAbs(0, gmax);
  for (;;) {
    uint64_t now = mach_absolute_time();
    int busy = 0;
    uint64_t wake = now + ns2abs(200000);  // look at replies at least every 0.2 ms
    for (int i = 0; i < nPorts; i++) {
      Port *p = &ports[i];
      if (atomic_load(&p->want) >= 0) {  // a ping is out
        busy = 1;
        Ping *g = &p->pings[p->n - 1];
        if (atomic_load(&p->got)) {
          g->recvTs = atomic_load(&p->gotTs);
          g->recvCb = atomic_load(&p->gotCb);
        } else if (now < p->deadline) {
          continue;
        }
        atomic_store(&p->want, -1);
        p->next = now + gapAbs(gmin, gmax);
      }
      if (stopNow || now >= end || p->n >= count) continue;
      busy = 1;
      if (now < p->next) {
        if (p->next < wake) wake = p->next;
        continue;
      }
      Ping *g = &p->pings[p->n++];
      g->key = (uint16_t)((p->n - 1) & 0x3FFF);
      g->late = now - p->next;
      atomic_store(&p->got, 0);
      atomic_store(&p->want, g->key);
      Byte buf[64];
      MIDIPacketList *pl = (MIDIPacketList *)buf;
      MIDIPacket *pk = MIDIPacketListInit(pl);
      Byte m[3] = {status, (Byte)(g->key & 0x7F), (Byte)(g->key >> 7)};
      g->send = mach_absolute_time();
      pk = MIDIPacketListAdd(pl, sizeof(buf), pk, 0, 3, m);
      MIDISend(out, p->dst, pl);
      p->deadline = g->send + tmoAbs;
    }
    if (!busy) break;
    mach_wait_until(wake);
  }

  printf("port,key,send_ns,recv_ts_ns,recv_cb_ns,late_ns\n");
  for (int i = 0; i < nPorts; i++)
    for (int k = 0; k < ports[i].n; k++) {
      Ping *g = &ports[i].pings[k];
      printf("%d,%u,%llu,%llu,%llu,%llu\n", ports[i].num, g->key, abs2ns(g->send), g->recvTs ? abs2ns(g->recvTs) : 0,
             g->recvCb ? abs2ns(g->recvCb) : 0, abs2ns(g->late));
    }
  return 0;
}
