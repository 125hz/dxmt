/*
 * MADEIRA (WOW64_DESIGN.md section 8.4, measurement 1): [d3d9-census].
 *
 * This file is Madeira's own work, distributed under GPL-3.0-or-later.
 * See research/dxmt/LICENSE-MADEIRA.md.
 *
 * Section 8 has to know how many D3D9 vtable calls a real workload makes per
 * frame, and which ones, before it can decide whether a synchronous 32-bit
 * shim over a native ARM64 frontend is viable: the answer is (calls/frame) x
 * (cost of one unix call, measured by unixcall-bench-x86.exe). A COM frontend
 * has no choke point to instrument, so there is one counter at the top of
 * every one of the 317 STDMETHODCALLTYPE definitions in src/d3d9.
 *
 * Cost. ONE relaxed 32-bit atomic add per call, behind a plain-bool load that
 * a branch predictor sees as never-taken-either-way. The counters are
 * uint32_t rather than uint64_t on purpose: on i386 a 64-bit atomic add is a
 * `lock cmpxchg8b` retry loop, which is exactly the kind of overhead that
 * would distort the measurement it exists to take. A single method would have
 * to be called 4.29e9 times to wrap; at the ~13k calls/frame and 20 fps
 * section 8.4 assumes, the busiest one wraps after about 4.5 hours, and the
 * summaries are printed continuously long before that.
 *
 * The call sites and d3d9_census_names.h are BOTH emitted by
 * gen_d3d9_census.py from one scan, so a code cannot name a method other than
 * the one that incremented it -- the same contract the winemetal dispatch
 * table's slot numbers have (see src/winemetal/wmt_api_census.c).
 */
#pragma once

#include "d3d9_census_names.h"

#include <atomic>
#include <cstdint>

namespace dxmt::census {

/* Written once, during this DLL's static initialisation, and only read after
 * that -- so the hot path is a plain load, not an atomic one. */
extern bool g_on;

extern std::atomic<uint32_t> g_calls[D3D9_CENSUS_COUNT];

/* Present. Counted like any other method AND used as the frame clock: the
 * summary cadence is measured in presented frames, because "calls per frame"
 * is the number section 8.4 needs and because an iOS app is killed rather
 * than exited, so there is never an atexit report to fall back on. */
void frame(unsigned code);

/* Two shape histograms the method counts alone cannot answer: how big the
 * constant uploads are (Set{Vertex,Pixel}ShaderConstantF is expected to be
 * the single busiest slot, and a shim can only batch it if the register
 * counts are small), and how big a mapped-memory Lock is (WOW64_DESIGN.md
 * section 7.5 / 8.2's guest arena is sized off this). */
void shaderConstF(unsigned vec4_count);
void lockBytes(unsigned size_bytes); /* 0 means "to the end of the buffer" */

} // namespace dxmt::census

#define D3D9_CENSUS(code)                                                                                              \
  do {                                                                                                                 \
    if (::dxmt::census::g_on)                                                                                          \
      ::dxmt::census::g_calls[(code)].fetch_add(1, std::memory_order_relaxed);                                          \
  } while (0)

/* Present only. Separate macro so the frame clock lives in exactly one place;
 * gen_d3d9_census.py emits it for MTLD3D9SwapChain::Present alone, because
 * MTLD3D9Device::Present and ::PresentEx both forward to it and ticking all
 * three would count one frame as three. */
#define D3D9_CENSUS_FRAME(code)                                                                                        \
  do {                                                                                                                 \
    if (::dxmt::census::g_on)                                                                                          \
      ::dxmt::census::frame((code));                                                                                    \
  } while (0)
