/*

 mimuz_link_lathist mimuz-link latency histogram helper

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/19 : Start Writing

*/
// mimuz-link: latency distribution counter (for statistics and diagnostics)
#pragma once
#include <stdint.h>
#include <string.h>
#include <Print.h>

// ---- latency histogram: 10us bins up to 20ms (+overflow) ----
struct LatHist {
  static const int NB = 2000;
  static const uint32_t US = 10;
  uint32_t b[NB + 1];
  uint32_t n, mn, mx;
  uint64_t sum;
  LatHist() { reset(); }
  void reset() {
    memset(b, 0, sizeof(b));
    n = 0; mn = UINT32_MAX; mx = 0; sum = 0;
  }
  void add(uint32_t us) {
    uint32_t i = us / US;
    b[i > (uint32_t)NB ? NB : i]++;
    n++; sum += us;
    if (us < mn) mn = us;
    if (us > mx) mx = us;
  }
  // p in [0,1]. returns bin upper edge [us] (conservative)
  uint32_t pct(double p) const {
    if (!n) return 0;
    uint32_t target = (uint32_t)(p * n + 0.999999);
    if (target < 1) target = 1;
    uint32_t c = 0;
    for (int i = 0; i <= NB; i++) {
      c += b[i];
      if (c >= target) return i == NB ? mx : (i + 1) * US;
    }
    return mx;
  }
  // div: 2 prints RTT/2
  void print(Print &o, const char *name, uint32_t div = 1) const {
    if (!n) { o.printf("%s: no data\n", name); return; }
    auto ms = [&](uint32_t us) { return us / 1000.0 / div; };
    o.printf("%s: n=%lu  min=%.3f  p50=%.3f  p95=%.3f  p99=%.3f  p99.9=%.3f  max=%.3f  avg=%.3f [ms]\n",
             name, (unsigned long)n, ms(mn), ms(pct(0.50)), ms(pct(0.95)), ms(pct(0.99)), ms(pct(0.999)), ms(mx),
             (double)sum / n / 1000.0 / div);
  }
};
