/*
 * MADEIRA (WOW64_DESIGN.md section 8.4, measurement 1): [d3d9-census]
 * counters and report.
 *
 * This file is Madeira's own work, distributed under GPL-3.0-or-later.
 * See research/dxmt/LICENSE-MADEIRA.md.
 *
 * Modelled on src/winemetal/wmt_api_census.c, with the two differences the
 * question forces:
 *
 *  - The clock is PRESENTED FRAMES, not total calls. Section 8.4 multiplies
 *    calls-per-frame by the cost of one unix call, so a summary that is not
 *    divided by a frame count answers nothing. Present is also the only event
 *    in a D3D9 frontend that is guaranteed to be reached exactly once per
 *    frame from the app's own render thread.
 *  - Everything printed is a WINDOW (since the previous summary), not a
 *    lifetime total. A lifetime average over a run that includes loading
 *    screens, shader compilation and a menu is not the steady-state number
 *    section 8.4 needs; the window is. The lifetime total is printed too, on
 *    the same line, so nothing is lost.
 *
 * Summaries land at present 1, 100, 1000 and then every 5000 presents,
 * because an iOS app is KILLED rather than exited: atexit never fires, so
 * "the whole run" can only ever mean "the latest checkpoint". The 1000 ->
 * 5000 gap is the one real exposure (250 s at 20 fps), so the interval is
 * overridable with MADEIRA_D3D9_CENSUS_EVERY.
 */

#include "d3d9_census.hpp"

#define D3D9_CENSUS_NAME_TABLE
#include "d3d9_census_names.h"

#include "log/log.hpp"
#include "util_env.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxmt::census {

std::atomic<uint32_t> g_calls[D3D9_CENSUS_COUNT];

namespace {

/* Coarse on purpose: the question is "are constant uploads small enough to
 * batch", not the exact distribution. */
constexpr int kHistBuckets = 10;
const char *const kConstLabels[kHistBuckets] = {"1",     "2",     "3-4",    "5-8",     "9-16",
                                                "17-32", "33-64", "65-128", "129-256", ">256"};
const char *const kLockLabels[kHistBuckets] = {"whole",  "1-64",    "65-256",  "257-1K", "1K-4K",
                                               "4K-16K", "16K-64K", "64K-256K", "256K-1M", ">1M"};

std::atomic<uint32_t> g_hist_const[kHistBuckets];
std::atomic<uint32_t> g_hist_lock[kHistBuckets];

std::atomic<uint32_t> g_presents;
std::atomic<bool> g_reporting;

/* Snapshots taken by the previous summary. Only ever touched under
 * g_reporting, so plain types are enough. */
uint32_t g_prev_calls[D3D9_CENSUS_COUNT];
uint32_t g_prev_hist_const[kHistBuckets];
uint32_t g_prev_hist_lock[kHistBuckets];
uint32_t g_prev_presents;
uint32_t g_prev_ticks;
unsigned g_summary_seq;

unsigned g_interval = 5000;

unsigned long long now_ms() {
#ifdef _WIN32
  return GetTickCount();
#else
  return 0;
#endif
}

bool readEnabled() {
  std::string v = env::getEnvVar("MADEIRA_D3D9_CENSUS");
  /* Default ON: this is a measurement build stage, and a knob that has to be
   * set to get any data is a knob nobody sets. */
  return !(v == "0" || v == "off" || v == "no" || v == "false");
}

void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void line(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Logger::info(std::string("[d3d9-census] ") + buf);
}

void report() {
  uint32_t presents = g_presents.load(std::memory_order_relaxed);
  uint32_t frames = presents - g_prev_presents;
  if (!frames)
    frames = 1;
  uint32_t ticks = (uint32_t)now_ms();

  unsigned long long total = 0, window = 0;
  unsigned used = 0;
  static uint32_t cur[D3D9_CENSUS_COUNT];
  for (int i = 0; i < D3D9_CENSUS_COUNT; i++) {
    cur[i] = g_calls[i].load(std::memory_order_relaxed);
    total += cur[i];
    window += (uint32_t)(cur[i] - g_prev_calls[i]); /* unsigned wrap is the right arithmetic */
    if (cur[i])
      used++;
  }

  if (!g_summary_seq)
    line("armed: %d methods, summaries at present 1/100/1000 then every %u "
         "(MADEIRA_D3D9_CENSUS=0 disables, MADEIRA_D3D9_CENSUS_EVERY overrides)",
         D3D9_CENSUS_COUNT, g_interval);

  g_summary_seq++;
  line("---- summary %u: present=%u frames=%u window=%ums ----", g_summary_seq, presents, frames,
       (unsigned)(ticks - g_prev_ticks));
  line("calls: window=%llu total=%llu per_frame=%.1f used=%u/%d", window, total,
       (double)window / (double)frames, used, D3D9_CENSUS_COUNT);
  line("top20 by window count (count, per-frame average):");

  /* 20 passes over 317 entries, once every few thousand frames. */
  bool taken[D3D9_CENSUS_COUNT] = {};
  for (int rank = 1; rank <= 20; rank++) {
    int best = -1;
    uint32_t best_n = 0;
    for (int i = 0; i < D3D9_CENSUS_COUNT; i++) {
      uint32_t n = cur[i] - g_prev_calls[i];
      if (!taken[i] && n > best_n) {
        best_n = n;
        best = i;
      }
    }
    if (best < 0)
      break;
    taken[best] = true;
    line("%4d %-52s %10u %8.1f/f", rank, d3d9_census_names[best], best_n, (double)best_n / (double)frames);
  }

  {
    char buf[512];
    int off = 0;
    for (int i = 0; i < kHistBuckets; i++) {
      uint32_t n = g_hist_const[i].load(std::memory_order_relaxed);
      off += snprintf(buf + off, sizeof(buf) - (size_t)off, " %s=%u", kConstLabels[i],
                      (uint32_t)(n - g_prev_hist_const[i]));
      g_prev_hist_const[i] = n;
    }
    line("setshaderconstf vec4 regs:%s", buf);
    off = 0;
    for (int i = 0; i < kHistBuckets; i++) {
      uint32_t n = g_hist_lock[i].load(std::memory_order_relaxed);
      off += snprintf(buf + off, sizeof(buf) - (size_t)off, " %s=%u", kLockLabels[i],
                      (uint32_t)(n - g_prev_hist_lock[i]));
      g_prev_hist_lock[i] = n;
    }
    line("buffer lock bytes:%s", buf);
  }

  line("---- end summary %u ----", g_summary_seq);

  memcpy(g_prev_calls, cur, sizeof(g_prev_calls));
  g_prev_presents = presents;
  g_prev_ticks = ticks;
}

/* Deliberately silent. This runs as a static initialiser, and Logger's own
 * s_instance lives in another translation unit: with no ordering guarantee
 * between the two, logging here can reach an unconstructed Logger (its mutex
 * included). The "armed" line is therefore printed by the first summary
 * instead, where the ordering question cannot arise. Reading an environment
 * variable and GetTickCount are both safe this early -- neither depends on a
 * C++ object built by another TU's initialiser. */
bool arm() {
  bool on = readEnabled();
  if (on) {
    std::string every = env::getEnvVar("MADEIRA_D3D9_CENSUS_EVERY");
    if (!every.empty()) {
      unsigned long v = strtoul(every.c_str(), nullptr, 10);
      if (v)
        g_interval = (unsigned)v;
    }
    g_prev_ticks = (uint32_t)now_ms();
  }
  return on;
}

} // namespace

/* Dynamic initialiser: runs during this DLL's own CRT init, long before any
 * vtable slot can be entered, so the hot path never has to test "armed yet?".
 */
bool g_on = arm();

void frame(unsigned code) {
  g_calls[code].fetch_add(1, std::memory_order_relaxed);
  uint32_t p = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (p != 1 && p != 100 && p != 1000 && (p % g_interval) != 0)
    return;
  /* A second thread presenting concurrently skips the report rather than
   * interleaving two of them into the log. */
  if (g_reporting.exchange(true, std::memory_order_acquire))
    return;
  report();
  g_reporting.store(false, std::memory_order_release);
}

void shaderConstF(unsigned n) {
  if (!g_on)
    return;
  static const unsigned kMax[kHistBuckets - 1] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
  int b = kHistBuckets - 1;
  for (int i = 0; i < kHistBuckets - 1; i++)
    if (n <= kMax[i]) {
      b = i;
      break;
    }
  g_hist_const[b].fetch_add(1, std::memory_order_relaxed);
}

void lockBytes(unsigned n) {
  if (!g_on)
    return;
  /* SizeToLock == 0 means "to the end of the buffer" in D3D9 and gets its own
   * bucket rather than being folded into the smallest one. */
  static const unsigned kMax[kHistBuckets - 1] = {0, 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};
  int b = kHistBuckets - 1;
  for (int i = 0; i < kHistBuckets - 1; i++)
    if (n <= kMax[i]) {
      b = i;
      break;
    }
  g_hist_lock[b].fetch_add(1, std::memory_order_relaxed);
}

} // namespace dxmt::census
