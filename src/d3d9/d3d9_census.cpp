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
#include "util_string.hpp"

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

/* MADEIRA [d3d9-query]: see d3d9_census.hpp. */
std::atomic<uint32_t> g_q_issued;
std::atomic<uint32_t> g_q_flushed;
std::atomic<uint32_t> g_q_polls;
std::atomic<uint32_t> g_q_polls_complete;
std::atomic<uint32_t> g_q_polls_parked;
std::atomic<uint32_t> g_q_completions;
std::atomic<uint64_t> g_q_latency_ns;
std::atomic<uint32_t> g_q_latency_max_us;

uint32_t g_prev_q_issued;
uint32_t g_prev_q_flushed;
uint32_t g_prev_q_polls;
uint32_t g_prev_q_polls_complete;
uint32_t g_prev_q_polls_parked;
uint32_t g_prev_q_completions;
uint64_t g_prev_q_latency_ns;

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

/* MADEIRA: own tag so the query instrument can be grepped out of a device log
 * without the 20-line census block around it. */
void qline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void qline(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Logger::info(std::string("[d3d9-query] ") + buf);
}

/* MADEIRA: one window's worth of the query-poll instrument. Same windowing
 * rule as the census itself -- unsigned wrap arithmetic against the previous
 * summary's snapshot, so a wrapped counter still yields the right delta. */
void reportQueries(uint32_t frames) {
  uint32_t issued = g_q_issued.load(std::memory_order_relaxed);
  uint32_t flushed = g_q_flushed.load(std::memory_order_relaxed);
  uint32_t polls = g_q_polls.load(std::memory_order_relaxed);
  uint32_t hits = g_q_polls_complete.load(std::memory_order_relaxed);
  uint32_t parked = g_q_polls_parked.load(std::memory_order_relaxed);
  uint32_t done = g_q_completions.load(std::memory_order_relaxed);
  uint64_t latency = g_q_latency_ns.load(std::memory_order_relaxed);
  uint32_t worst = g_q_latency_max_us.load(std::memory_order_relaxed);

  uint32_t w_issued = issued - g_prev_q_issued;
  uint32_t w_flushed = flushed - g_prev_q_flushed;
  uint32_t w_polls = polls - g_prev_q_polls;
  uint32_t w_hits = hits - g_prev_q_polls_complete;
  uint32_t w_parked = parked - g_prev_q_polls_parked;
  uint32_t w_done = done - g_prev_q_completions;
  uint64_t w_latency = latency - g_prev_q_latency_ns;

  if (!w_issued && !w_polls) {
    qline("no query traffic this window");
  } else {
    qline("issued=%u (%.1f/f) flush_submits=%u polls=%u (%.1f/f) hit=%u parked=%u (%.1f%% of polls)", w_issued,
          (double)w_issued / (double)frames, w_flushed, w_polls, (double)w_polls / (double)frames, w_hits, w_parked,
          w_polls ? 100.0 * (double)w_parked / (double)w_polls : 0.0);
    qline("completions=%u polls_per_completion=%.1f issue_to_complete_avg=%.1fus worst_ever=%uus", w_done,
          w_done ? (double)w_polls / (double)w_done : 0.0, w_done ? (double)w_latency / (double)w_done / 1000.0 : 0.0,
          worst);
  }

  g_prev_q_issued = issued;
  g_prev_q_flushed = flushed;
  g_prev_q_polls = polls;
  g_prev_q_polls_complete = hits;
  g_prev_q_polls_parked = parked;
  g_prev_q_completions = done;
  g_prev_q_latency_ns = latency;
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

  reportQueries(frames);

  line("---- end summary %u ----", g_summary_seq);

  memcpy(g_prev_calls, cur, sizeof(g_prev_calls));
  g_prev_presents = presents;
  g_prev_ticks = ticks;
}

/* MADEIRA [d3d9-last]: the ring itself. See d3d9_census.hpp for why this is
 * deliberately unsynchronised. */
struct LastEntry {
  uint32_t seq;  /* 0 = never written; otherwise the global push order */
  uint16_t code; /* index into d3d9_census_names[] when note == nullptr */
  uint8_t kind;  /* 0 = call (entry), 1 = ret */
  uint8_t pad;
  uint32_t tid;
  int32_t hr;
  uint32_t a0;
  uint32_t a1;
  const char *note; /* static literal naming a non-vtable event, or null */
};

LastEntry g_last[kLastRing];
std::atomic<uint32_t> g_last_head;
std::atomic<uint32_t> g_dumps;

uint32_t currentTid() {
#ifdef _WIN32
  return (uint32_t)GetCurrentThreadId();
#else
  return 0;
#endif
}

/* Deliberately silent. This runs as a static initialiser, and Logger's own
 * s_instance lives in another translation unit: with no ordering guarantee
 * between the two, logging here can reach an unconstructed Logger (its mutex
 * included). The "armed" line is therefore printed by the first summary
 * instead, where the ordering question cannot arise. Reading an environment
 * variable and GetTickCount are both safe this early -- neither depends on a
 * C++ object built by another TU's initialiser. */
#ifdef _WIN32
/* MADEIRA [d3d9-last]: how the ring reaches the log.
 *
 * The 32-bit D3D9 frontend is guest code (d3d9-emulated.dll runs under the
 * emulator), so the ring lives in guest memory and the host-side crash
 * reporter in build/ntdll-unix/signal_arm64_ios.c cannot read it -- its weak
 * d3d9_dump_last_calls() hook resolves against the NATIVE build of this file
 * and would print an empty ring for an emulated title. A vectored handler
 * closes that gap without any address publication: it runs in the guest, in
 * the faulting thread, with the ring right there, on first chance and
 * therefore before the application's own __except can swallow the fault.
 *
 * It only ever observes. EXCEPTION_CONTINUE_SEARCH is returned
 * unconditionally, so dispatch is exactly what it was.
 *
 * The filter matters: a vectored handler sees every exception in the process,
 * and most of them are routine -- MSVC C++ throws (0xE06D7363), the
 * thread-name notification (0x406D1388), OutputDebugString
 * (DBG_PRINTEXCEPTION_C / _WIDE_C), breakpoints under a debugger. Dumping on
 * those would bury the log and tell nobody anything. Only the codes that end
 * a process unhandled are worth a dump. */
LONG CALLBACK lastCallVEH(EXCEPTION_POINTERS *ep) {
  if (!ep || !ep->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_STACK_OVERFLOW:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    break;
  default:
    return EXCEPTION_CONTINUE_SEARCH;
  }
  char why[96];
  snprintf(why, sizeof(why), "guest exception %08x at %p addr %p", (unsigned)code,
           (void *)ep->ExceptionRecord->ExceptionAddress,
           ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
  dumpLastCalls(why);
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

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
#ifdef _WIN32
    /* First in the chain (the 1 argument): the point is to see the fault
     * before anything the application installs later can handle it. */
    AddVectoredExceptionHandler(1, lastCallVEH);
#endif
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

/* MADEIRA [d3d9-query] counters. Unconditional on g_on like the histograms:
 * the callers already test it, and these are not on a 317-slot hot path. */
void queryIssued() {
  g_q_issued.fetch_add(1, std::memory_order_relaxed);
}

void queryFlushed() {
  g_q_flushed.fetch_add(1, std::memory_order_relaxed);
}

void queryPoll(bool complete, bool parked) {
  g_q_polls.fetch_add(1, std::memory_order_relaxed);
  if (complete)
    g_q_polls_complete.fetch_add(1, std::memory_order_relaxed);
  if (parked)
    g_q_polls_parked.fetch_add(1, std::memory_order_relaxed);
}

void queryCompleted(uint64_t issue_to_complete_ns, uint32_t polls) {
  (void)polls;
  g_q_completions.fetch_add(1, std::memory_order_relaxed);
  g_q_latency_ns.fetch_add(issue_to_complete_ns, std::memory_order_relaxed);
  uint32_t us = (uint32_t)(issue_to_complete_ns / 1000u);
  uint32_t prev = g_q_latency_max_us.load(std::memory_order_relaxed);
  while (us > prev && !g_q_latency_max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed))
    ;
}

/* MADEIRA [d3d9-last]. One relaxed fetch_add plus six plain stores. The seq
 * is written LAST so a reader that sees a non-zero seq has, in the
 * overwhelmingly common case, the rest of the entry too. */
void ringCall(unsigned code) {
  uint32_t n = g_last_head.fetch_add(1, std::memory_order_relaxed) + 1;
  LastEntry &e = g_last[n & (kLastRing - 1)];
  e.code = (uint16_t)code;
  e.kind = 0;
  e.tid = currentTid();
  e.hr = 0;
  e.a0 = 0;
  e.a1 = 0;
  e.note = nullptr;
  e.seq = n;
}

void ringRet(unsigned code, long hr, uint32_t a0, uint32_t a1) {
  uint32_t n = g_last_head.fetch_add(1, std::memory_order_relaxed) + 1;
  LastEntry &e = g_last[n & (kLastRing - 1)];
  e.code = (uint16_t)code;
  e.kind = 1;
  e.tid = currentTid();
  e.hr = (int32_t)hr;
  e.a0 = a0;
  e.a1 = a1;
  e.note = nullptr;
  e.seq = n;
}

void ringNote(const char *what, long hr, uint32_t a0, uint32_t a1) {
  if (!g_on)
    return;
  uint32_t n = g_last_head.fetch_add(1, std::memory_order_relaxed) + 1;
  LastEntry &e = g_last[n & (kLastRing - 1)];
  e.code = 0;
  e.kind = 1;
  e.tid = currentTid();
  e.hr = (int32_t)hr;
  e.a0 = a0;
  e.a1 = a1;
  e.note = what;
  e.seq = n;
}

void dumpLastCalls(const char *why) {
  /* Re-entrancy guard, and it is not a nicety: this runs from a fault
   * handler, and it reaches Logger, which takes a mutex and writes a file.
   * If the dump itself faults -- or if the thread was already inside this
   * function -- taking that mutex a second time on the same thread is
   * undefined behaviour on a std::mutex and in practice a deadlock, which
   * would turn a crash the user can see into a freeze they cannot. Bailing
   * out keeps the failure a crash. Per-thread rather than global so two
   * threads faulting at once still both get a dump. */
  static thread_local bool in_dump = false;
  if (in_dump)
    return;

  /* Three dumps per process. A fault inside the fault handler, or an exit
   * that follows a crash, must not turn the log into the ring printed over
   * and over. */
  if (g_dumps.fetch_add(1, std::memory_order_relaxed) >= 3)
    return;

  in_dump = true;
  struct ClearOnExit {
    bool &flag;
    ~ClearOnExit() { flag = false; }
  } clear{in_dump};

  uint32_t head = g_last_head.load(std::memory_order_relaxed);
  Logger::info(
      str::format("[d3d9-last] dump (", why, ") head=", head, " ring=", kLastRing, " -- oldest first, seq is the ",
                  "global push order; a gap in seq means the slot was overwritten mid-read")
  );
  if (!head) {
    Logger::info("[d3d9-last] (empty: no D3D9 call has been made, or the census is off)");
    return;
  }

  /* Oldest first: head+1 .. head, skipping slots never written. */
  unsigned shown = 0;
  for (unsigned i = 1; i <= kLastRing; i++) {
    const LastEntry &e = g_last[(head + i) & (kLastRing - 1)];
    if (!e.seq)
      continue;
    const char *name = e.note ? e.note : (e.code < D3D9_CENSUS_COUNT ? d3d9_census_names[e.code] : "?");
    char buf[256];
    if (e.kind)
      snprintf(buf, sizeof(buf), "%6u tid=%04x %-52s -> hr 0x%08x  a0=0x%x a1=0x%x", e.seq, e.tid, name,
               (unsigned)e.hr, e.a0, e.a1);
    else
      snprintf(buf, sizeof(buf), "%6u tid=%04x %-52s", e.seq, e.tid, name);
    Logger::info(std::string("[d3d9-last] ") + buf);
    shown++;
  }
  Logger::info(str::format("[d3d9-last] end (", shown, " entries)"));
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

/* MADEIRA [d3d9-last]: the host-side entry point.
 *
 * build/ntdll-unix/signal_arm64_ios.c declares this weak and calls it from
 * the one place it reports a guest fault, so a build in which this object is
 * not linked (or whose linker did not pull it out of libdxmt_combined.a)
 * simply does nothing there. It is the NATIVE frontend's ring it prints --
 * for a title on the emulated i386 frontend the in-guest vectored handler
 * above is what produces the dump, and this one is silent-but-harmless. Both
 * write the same [d3d9-last] block, so a reader does not have to know which
 * half of the port answered. */
extern "C" __attribute__((visibility("default"))) void d3d9_dump_last_calls(void) {
  ::dxmt::census::dumpLastCalls("native-hook");
}
