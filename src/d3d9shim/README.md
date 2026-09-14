<!--
Copyright 2026 Will Faust
SPDX-License-Identifier: GPL-3.0-or-later
-->

# `d3d9shim` — the generated half of the native D3D9 path

`WOW64_DESIGN.md` §8 replaces the emulated i386 `d3d9.dll` with two modules:
a thin i386 **shim** that owns the guest-visible object model, and the
**native ARM64** DXMT D3D9 frontend living in `libdxmt_unix.a`. This
directory holds the single description of the boundary between them and the
generator that emits both sides of it.

Nothing here is wired into a build yet — that is steps 3 and 4.

## Files

| File | Owner | What it is |
|---|---|---|
| `d3d9_api.py` | **hand-written** | The description: 15 interfaces, 320 vtable slots, every argument's shape, every method's disposition, the five mirror structs, the shim object model. Run it (`python3 d3d9_api.py`) for a summary and a validation pass. |
| `gen_d3d9_thunks.py` | **hand-written** | The generator. |
| `d3d9shim_ops.h` | generated | Opcode enum, 320 parameter blocks, mirror structs, every `_Static_assert`. Included by **both** sides. |
| `d3d9shim_objects_gen.h` | generated | The shim object header, per-kind state, and the extern hook contract. |
| `d3d9shim_thunks.c` | generated | 15 vtables, 320 method bodies (i386 PE). |
| `../d3d9/unix/d3d9_native_hooks.h` | generated | One `d3d9_native_<Iface>_<Method>` prototype per slot. |
| `../d3d9/unix/d3d9_unix.c` | generated | 321 unix entries, the mirror conversions, the ring-replay switch. |
| `../d3d9/unix/d3d9_unix_table.c` | generated | Both dispatch tables. |

Still to be hand-written (§8.5): `d3d9shim_main.c`, `d3d9shim_object.c/.h`,
`d3d9shim_window.c`, `d3d9shim_fpu.c`, `d3d9shim_arena.c`, `d3d9shim_lock.c`,
`d3d9.def`, `meson.build`, and on the unix side `d3d9_unix_glue.h` +
`d3d9_native_glue.cpp`.

## Regenerating

```sh
python3 research/dxmt/src/d3d9shim/gen_d3d9_thunks.py            # write
python3 research/dxmt/src/d3d9shim/gen_d3d9_thunks.py --check    # CI: stale?
```

Every generated file carries a "REGENERATE, DO NOT EDIT" banner. Output is
deterministic — no timestamps, no paths, no dict iteration order — so
`--check` is a clean staleness gate for a pre-commit hook or CI.

The generator **refuses to emit anything** if `d3d9_api.validate()` or its own
self-check reports a problem. Each of these has been shown to fire:

- duplicate or non-dense slot numbers within an interface
- a slot added or dropped (the per-interface counts are checked against the
  table in `WOW64_DESIGN.md` §8.1, and the total against 320)
- a duplicate method name in one interface
- an unknown disposition, or a `defer` with no validation predicate or no
  constant return, or a `local` with no local kind, or a `sync` that does not
  flush
- a mirror whose declared 32-bit image is not exactly 4-byte packed, or whose
  field offsets do not add up to its declared `size32`
- a predicate or count expression naming something that does not resolve — an
  argument that does not exist, a `self.<field>` not in `OBJECT_STATE`, an
  unknown helper call
- an unknown identity helper, an interface with no object kind, a return type
  with no storage class, a parameter block whose size is not a multiple of 8

The two dispatch tables are emitted from one list, so they cannot differ in
length; `d3d9_unix_table.c` asserts that in C anyway, and each vtable asserts
its own length.

## The ABI rules

**The slot number is the ABI.** Slot order comes from the SDK header
`include/native/directx/d3d9.h`, which is also what `src/d3d9/*.hpp` declares
`override` against — so the description and the implementation cannot drift
without a C++ compile error. A line inserted or dropped silently sends every
later call to the wrong function, exactly as `gen_remote_guard.py` warns about
the winemetal table. All 320 slot names were cross-checked against the
implementing classes; only `AddRef`/`Release` on two interfaces are not
textually present, because they come from `ComObject`.

**Parameter blocks are fixed width.** Guest pointers are `uint32_t`, handles
and native object references `uint64_t`, scalars `uint32_t` (or `float` where
the C type is float — 4 bytes with the same representation on both). The
64-bit fields are emitted first, so each lands on a multiple of 8 whatever the
target's alignment rule for 64-bit types happens to be. One struct definition
is therefore correct on i386 and on LP64 and there is **no `*_params32`
mirror**: this is the `WMTMemoryPointer` trick (`winemetal.h:132-200`) applied
to the whole block.

**That is also why both unix tables point at the same functions.** A block's
pointer fields are `uint32_t`, so a block can only have been written by a
32-bit caller and there is exactly one correct way to read one — a `_32`
variant would have nothing to do differently. The 64-bit table exists because
`load_builtin_unixlib` binds a *pair* of equal length (§7.4 rule 3);
`_d3d9_init` refuses any caller whose `sizeof(void *)` is not 4, so the
64-bit table can be entered but never lies about a result (§7.4 rule 4).

**Pointers.** Every embedded pointer is a GUEST address and is converted with
`ios_wow_host_ptr()` semantics, NULL-preserving, before any dereference; every
converted pointer is range-checked before the frontend sees it, so a bad guest
pointer is `D3DERR_INVALIDCALL` and not a host fault the application's SEH can
never catch (§8.9-4). Exactly one pointer is ever written back: `pBits`, via
`ios_wow_guest_ptr32()`. Sizes, enums, `HWND`/`HMONITOR`/`HDC`/`HANDLE` and
native handles are never offset (invariant 4). Nesting is at most two levels:
`pSharedHandle`'s user-memory idiom, `DrawIndexedPrimitiveUP`'s two buffers,
`CheckResourceResidency`'s array of guest interface pointers, and `pBits`.

**Structs.** Measured with `i686-w64-mingw32-clang` and with a 64-bit `gcc`
against the same headers, not assumed:

- **five mirrors** (i386 and LP64 layouts differ):
  `D3DPRESENT_PARAMETERS` 56/64, `D3DDEVICE_CREATION_PARAMETERS` 16/24,
  `D3DLOCKED_RECT` 8/16, `D3DLOCKED_BOX` 12/16, and — **not in §8.2(c)** —
  `D3DPRESENTSTATS` 28/32, where `LARGE_INTEGER` takes 4-byte alignment on
  i386 and 8 on LP64, moving `SyncQPCTime` from +12 to +16. Reached only by
  `IDirect3DSwapChain9Ex::GetPresentStats`.
- **26 layout-identical** structs, pointed at in place after one `+B`.
- **one padding-only** struct, `D3DADAPTER_IDENTIFIER9`: every field at the
  same offset, but `sizeof()` is 1100 on i386 and 1104 on LP64. It is still
  pointed at in place, its window check uses `D3D9SHIM_SIZE32_*`, and **the
  native hook must not write past offset 1100**.

The mirror wire form is the i386 *memory image*, so every mirror field is
4 bytes wide and the two `LARGE_INTEGER`s cross as explicit lo/hi halves; a
`uint64_t` field would silently re-align the mirror and break it.

**Handshake.** `d3d9_api.py` hashes its own canonical form — slot order,
names, shapes, dispositions, predicates, block layouts, mirrors, but not
comments or formatting — into `D3D9SHIM_API_HASH`, compiled into both halves.
`_d3d9_init` (unix slot 0) refuses a mismatch with `STATUS_REVISION_MISMATCH`
and reports the native side's own hash so the log says which is stale.

## Dispositions

| | slots | |
|---|---:|---|
| `local` | 70 | never crosses: 15 `QueryInterface`, 15 `AddRef`, 7 `GetType`, 30 identity getters, `RegisterSoftwareDevice`, `SetCursorPosition`, `ShowCursor` |
| `sync` | 175 | flush the ring, call, wait |
| `defer` | 75 | ring-append and return a constant, if the predicate holds (~40 distinct op names; the count is higher because the resource ops repeat across six interfaces and every final `Release` is one) |

`Release` is `defer` because only the **final** release crosses; the
hand-written `d3d9shim_obj_release()` owns the decrement and builds the
`D3D9OP_*_Release` record itself.

Phase 1 (§8.5) is synchronous-everything. The generated deferred bodies carry
**both** arms, selected by `-DD3D9SHIM_PHASE=2`; both compile.

## The hook contract

Everything the generated code calls, and nothing else. Full declarations are
in `d3d9shim_objects_gen.h` (shim) and the prologue of `d3d9_unix.c` (unix).

### Shim side — implemented by the hand-written `d3d9shim_*.c`

Transport:

| Hook | Contract |
|---|---|
| `uint32_t d3d9shim_native_call(slot, block, size)` | 0 on success; any non-zero is a transport failure and the thunk returns `E_FAIL` without touching the block's out-parameters. |
| `int d3d9shim_ring_append(dev, op, block, size)` | non-zero if it fit; 0 means the caller flushes and calls synchronously. |
| `HRESULT d3d9shim_flush(dev)` | replay everything queued. |
| `void d3d9shim_shadow_apply(dev, op, block)` | update the shim's shadow of whatever the deferred op changes, so the `local` getters stay correct while the record is queued. One hook, switching on the opcode. |

Objects:

`d3d9shim_obj_from_native(kind, handle)` (create-or-find the guest wrapper),
`d3d9shim_obj_native(iface)`, `d3d9shim_obj_addref`, `d3d9shim_obj_release`,
`d3d9shim_obj_query_interface`, `d3d9shim_device_of`, `d3d9shim_same_device`,
`d3d9shim_has_usage`, `d3d9_transform_index`.

Identity helpers (return a **borrowed** reference or NULL; the generated body
does the `AddRef` and the store): `d3d9shim_device_back_buffer`,
`d3d9shim_device_texture` (applies `texture_stage_to_slot`),
`d3d9shim_device_stream_source` (also fills `offset`/`stride`),
`d3d9shim_swapchain_back_buffer`, `d3d9shim_texture_sublevel`,
`d3d9shim_cube_surface`, `d3d9shim_container`.

Arena (§7.5 / §8.2(c)): `d3d9shim_arena_alloc`, `d3d9shim_arena_free`,
`d3d9shim_arena_grow`. Lock (§8.2(d)): `d3d9shim_lock` / `d3d9shim_unlock`,
recursive, keyed by thread id; empty for a device without
`D3DCREATE_MULTITHREADED`. Diagnostics: `d3d9shim_log_once`.

Shim-local bodies, because they are user32/gdi32 work that must run on the
guest's own thread: `d3d9shim_shim_cursor_set_position`,
`d3d9shim_shim_cursor_show`.

Custom bodies, 11 slots whose guest-side half is not mechanical — `setupFpu`,
the focus-window hook, fullscreen styles, the cursor bitmap, `GetDC`'s
`D3DKMTCreateDCFromMemory`, the per-HWND client-size cache:
`CreateDevice`, `CreateDeviceEx`, `Reset`, `ResetEx`, `Present`, `PresentEx`,
`SwapChain9Ex::Present`, `SetCursorProperties`, `SetDialogBoxMode`,
`Surface9::GetDC`, `Surface9::ReleaseDC`. Each is
`d3d9shim_custom_<Iface>_<Method>` with the method's own signature. A `local`
method may not also be `custom` — the generator refuses it, because a local
body that needs hand-written help already has a `shim:` hook.

### Unix side — implemented by `d3d9_unix_glue.h` + `d3d9_native_glue.cpp`

Macros `d3d9_unix.c` requires:

| Macro | Contract |
|---|---|
| `D3D9_HOST_PTR(u32)` | `ios_wow_host_ptr()`: `+B`, NULL-preserving. |
| `D3D9_GUEST_PTR32(void *)` | `ios_wow_guest_ptr32()`. |
| `D3D9_IN_WINDOW(p, bytes)` | false for anything not wholly inside `[B, B+4G)`; must still validate the base address when `bytes` is 0. |
| `D3D9_DEREF32(p)` | the `ULONG` at an already-converted pointer; **NULL- and window-safe**, because a size-inout count is read before that argument's own validation runs. |
| `D3D9_SHARED_IN(slot, pool)` / `D3D9_SHARED_OUT(slot, h, pool)` | the `pSharedHandle` two-level rule: convert only for `D3DPOOL_SYSTEMMEM` with a non-NULL target (the user-memory idiom); otherwise opaque. `pool` is `D3D9_NO_POOL` for the four create paths that have no pool argument. |
| `D3D9_LOG(msg)` | one-line diagnostic. |
| `NTSTATUS`, `STATUS_*` | as ntdll. |

Variable-length input scanners (each walks GUEST memory, so each must
window-check as it walks and return 0 for anything it cannot follow):
`d3d9_decl_element_count`, `d3d9_shader_token_count`, `d3d9_up_vertex_bytes`,
`d3d9_up_index_bytes`.

Lifecycle: `int d3d9_native_init(void)` (called from `_d3d9_init` after the
hash check) and `void d3d9_native_process_teardown(void *peb)` — called from
`ios_wow_reclaim_dead_windows()` **before** the `PROT_NONE` replace and before
`ios_jit_purge_window()`, because native objects hold host pointers *into* the
arena (§8.9-5).

Per-method: `d3d9_native_<Iface>_<Method>(...)`, one per slot including the
`local` ones, so a later reclassification is a one-line change in
`d3d9_api.py`. The receiver and any interface argument arrive as
`d3d9_native_handle` (a `uint64_t` index+generation, never a host pointer);
struct and array arguments arrive as converted, validated host pointers.

Ring replay: `NTSTATUS d3d9_ring_replay(base, bytes, seq_io)` walks
`{u16 op; u16 len; u32 seq;}` records and calls the *same* `d3d9_call_*` the
synchronous entry does — the ring is a transport, not a re-implementation. It
refuses a non-monotonic `seq`, a misaligned base or length, a record whose
length does not match its opcode's block, and any opcode that is not
`defer`-classified.

## Host validation

`i686-w64-mingw32-clang` from `.xtool/toolchains/llvm-mingw`, against the real
mingw-w64 `d3d9.h`:

```
i686-w64-mingw32-clang -fsyntax-only -std=c11 -Wall -Wextra \
    -I src/d3d9shim src/d3d9shim/d3d9shim_thunks.c            # and -DD3D9SHIM_PHASE=2
```

and the unix side, LP64, against DXMT's own native headers (plus the
hand-written `d3d9_unix_glue.h`, which step 4 supplies):

```
gcc -fsyntax-only -std=c11 -Wall -Wextra \
    -I research/dxmt/include/native/windows \
    -I research/dxmt/include/native/directx \
    -I src/d3d9shim -I src/d3d9/unix src/d3d9/unix/d3d9_unix.c
```

Both pass clean at `-Wall -Wextra`, with no warnings at all from generated
code — including no `-Wunused-parameter`. That compile is also what
proves the mirrors: `d3d9shim_ops.h`'s `_Static_assert`s check the i386 image
against the SDK typedef on the 32-bit build and the host layout on the 64-bit
one, so a header change on either side is a build failure rather than a
silently wrong frame.
