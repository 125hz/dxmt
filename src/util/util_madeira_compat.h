/*
 * util_madeira_compat.h -- Win32 compatibility for the NATIVE Madeira build.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.2(d)).  New file, GPL-3.0-or-later; see
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * In the `dxmt_madeira_native` mode the D3D9 frontend and its DXMT substrate
 * are compiled as iOS-arm64 unix code that lives *below* the Win32 boundary:
 * the 32-bit shim owns every guest-visible Win32 object and the frontend
 * never sees one.  util_win32_compat.h is the wrong header here -- every one
 * of its stubs warns and fails, which would turn a thread-priority request
 * into a log line and a recursive spinlock into a livelock.
 *
 * So: real implementations where the call has a faithful POSIX meaning
 * (thread id, yield, priority, process id), and explicit sentinels where it
 * does not, so that a caller cannot mistake a stub's return for a handle.
 *
 * Copyright 2026 Will Faust
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(DXMT_MADEIRA)

#include <windows.h>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cstdint>

/* ---- types the minimal native windows.h does not carry ------------------ */

/* include/native/windows/windows_base.h is a COM-shaped subset written for
 * the D3D10/11 headers; the D3D9 ones use a few more of the classic integer
 * typedefs.  Added here rather than there so the shared header keeps serving
 * the frontends that do not need them. */
#ifndef __MADEIRA_COMPAT_INTEGER_TYPES
#define __MADEIRA_COMPAT_INTEGER_TYPES
typedef ULONG_PTR DWORD_PTR;
typedef LONG_PTR LRESULT;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef BYTE BOOLEAN;
#endif

/* ---- thread priority --------------------------------------------------- */

#ifndef THREAD_PRIORITY_TIME_CRITICAL
#define THREAD_PRIORITY_TIME_CRITICAL 15
#endif
#ifndef THREAD_PRIORITY_HIGHEST
#define THREAD_PRIORITY_HIGHEST 2
#endif
#ifndef THREAD_PRIORITY_NORMAL
#define THREAD_PRIORITY_NORMAL 0
#endif
#ifndef THREAD_PRIORITY_LOWEST
#define THREAD_PRIORITY_LOWEST (-2)
#endif

/* Sentinels.  These are NOT handles and must never be dereferenced, closed or
 * handed across the boundary; they exist only so the pseudo-handle idiom
 * `SetThreadPriority(GetCurrentThread(), ...)` keeps working and so a caller
 * that checks for NULL sees a "this succeeded" value.  Distinct values, so a
 * mix-up shows up in a debugger rather than silently aliasing. */
#define MADEIRA_PSEUDO_HANDLE_CURRENT_THREAD ((HANDLE)(ULONG_PTR)0xfffffffe)
#define MADEIRA_PSEUDO_HANDLE_CURRENT_PROCESS ((HANDLE)(ULONG_PTR)0xffffffff)

static inline HANDLE
GetCurrentThread() {
  return MADEIRA_PSEUDO_HANDLE_CURRENT_THREAD;
}

static inline HANDLE
GetCurrentProcess() {
  return MADEIRA_PSEUDO_HANDLE_CURRENT_PROCESS;
}

/* Real: the only two threads that ask are the encode and finish threads
 * (dxmt_command_queue.cpp, dxmt_tasks.hpp), which ask for TIME_CRITICAL
 * because a stall there stalls the GPU.  SCHED_FIFO at a mid band is the
 * closest Darwin equivalent; anything below TIME_CRITICAL stays SCHED_OTHER,
 * which is what pthreads already gives them. */
static inline BOOL
SetThreadPriority(HANDLE thread, int priority) {
  if (thread != MADEIRA_PSEUDO_HANDLE_CURRENT_THREAD)
    return FALSE; /* no thread handles exist here; refuse rather than lie */

  struct sched_param param = {};
  int policy = SCHED_OTHER;

  if (priority >= THREAD_PRIORITY_TIME_CRITICAL) {
    policy = SCHED_FIFO;
    int lo = sched_get_priority_min(SCHED_FIFO);
    int hi = sched_get_priority_max(SCHED_FIFO);
    param.sched_priority = lo + ((hi - lo) * 3) / 4;
  } else {
    param.sched_priority = sched_get_priority_min(SCHED_OTHER);
  }

  return ::pthread_setschedparam(::pthread_self(), policy, &param) == 0;
}

/* ---- thread and process identity --------------------------------------- */

/* Real: pthread_threadid_np is the kernel's own 64-bit thread id, stable for
 * the life of the thread and unique process-wide.  D9RecursiveSpinlock keys
 * ownership on the low 32 bits, and 0 is its "unowned" sentinel, so fold a
 * zero low half away rather than handing out an id that reads as unowned. */
static inline DWORD
GetCurrentThreadId() {
  uint64_t tid = 0;
  if (::pthread_threadid_np(nullptr, &tid) != 0 || tid == 0)
    return (DWORD)(ULONG_PTR)::pthread_self();
  DWORD id = (DWORD)(tid ^ (tid >> 32));
  return id ? id : 1u;
}

static inline DWORD
GetCurrentProcessId() {
  return (DWORD)::getpid();
}

static inline DWORD
GetProcessId(HANDLE process) {
  return process == MADEIRA_PSEUDO_HANDLE_CURRENT_PROCESS ? GetCurrentProcessId() : 0;
}

/* One iOS app, one session.  Answering 0 is the truth here, not a stub. */
static inline BOOL
ProcessIdToSessionId(DWORD pid, DWORD *session) {
  (void)pid;
  if (!session)
    return FALSE;
  *session = 0;
  return TRUE;
}

/* ---- yielding ----------------------------------------------------------- */

static inline BOOL
SwitchToThread() {
  return ::sched_yield() == 0;
}

#ifndef YieldProcessor
#if defined(__aarch64__) || defined(__arm64__)
#define YieldProcessor() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define YieldProcessor() __asm__ __volatile__("pause" ::: "memory")
#else
#define YieldProcessor() ((void)0)
#endif
#endif

/* ---- things that genuinely do not exist below the boundary -------------- */

/* No module table on this side: the frontend is a slice of the host binary,
 * not a PE.  NULL is the honest answer and every caller in this tree tests
 * for it. */
static inline HMODULE
GetModuleHandleA(LPCSTR name) {
  (void)name;
  return nullptr;
}

static inline HANDLE
GetModuleHandle(LPCSTR name) {
  (void)name;
  return nullptr;
}

static inline void *
GetProcAddress(HMODULE module, LPCSTR name) {
  (void)module;
  (void)name;
  return nullptr;
}

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))
#endif

#endif /* DXMT_MADEIRA */
