/*
 * d3d9shim_lock.c -- the D3DCREATE_MULTITHREADED device lock (8.2(d))
 *
 * The lock moves out of the frontend and into the shim, because it serializes
 * APPLICATION threads and those only exist on the guest side; the native
 * device is constructed is_protected=false.  Same semantics as
 * d3d9_multithread.hpp D9RecursiveSpinlock: recursive, keyed by
 * GetCurrentThreadId, a bounded pause-spin and then SwitchToThread, because
 * the holder can be parked on a GPU fence for milliseconds.  A device created
 * without D3DCREATE_MULTITHREADED takes no lock at all -- the application has
 * promised single-threaded use, which is what the native runtime and DXVK
 * assume too.
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

#define CINTERFACE
#define COBJMACROS

#include "d3d9shim_object.h"

#if defined(__i386__) || defined(__x86_64__)
#define D3D9SHIM_YIELD_PROCESSOR() __asm__ __volatile__("pause" ::: "memory")
#else
#define D3D9SHIM_YIELD_PROCESSOR() __asm__ __volatile__("" ::: "memory")
#endif

static int
try_lock(struct d3d9shim_device_extra *extra, LONG self)
{
    LONG previous = InterlockedCompareExchange(&extra->lock_owner, self, 0);

    if (previous == 0)
        return 1;
    if (previous != self)
        return 0;
    extra->lock_depth += 1;
    return 1;
}

void
d3d9shim_lock(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra;
    LONG self;
    unsigned int i;

    /* Called with a NULL device by every body on an object that has none
     * (IDirect3D9Ex), and by every body on a single-threaded device. */
    if (!dev || !dev->multithreaded)
        return;
    extra = d3d9shim_extra(dev);
    if (!extra)
        return;
    self = (LONG)GetCurrentThreadId();

    while (!try_lock(extra, self)) {
        for (i = 0; i < 2000; i++) {
            D3D9SHIM_YIELD_PROCESSOR();
            if (try_lock(extra, self))
                return;
        }
        SwitchToThread();
    }
}

void
d3d9shim_unlock(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra;

    if (!dev || !dev->multithreaded)
        return;
    extra = d3d9shim_extra(dev);
    if (!extra)
        return;
    if (extra->lock_depth == 0)
        InterlockedExchange(&extra->lock_owner, 0);
    else
        extra->lock_depth -= 1;
}
