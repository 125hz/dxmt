# Licensing of Madeira's modifications

This repository is a fork of [DXMT](https://github.com/3Shain/DXMT). **The upstream licence
is unchanged and continues to apply to all upstream code.** See `LICENSE`.

## What is licensed how

| Code | Licence |
|---|---|
| All upstream DXMT code | as in `LICENSE` (MIT) — unchanged |
| Code imported from the `v0.4-d3d9` tag of `dacevedo12/dxmt` (see below) | upstream **LGPL-2.1-or-later**, distributed here under **GPL-3.0-or-later** via LGPL-2.1 §3 |
| Modifications and new files authored for **Madeira** by Will Faust | **GPL-3.0-or-later** |

## The Direct3D 9 / DXSO import (LGPL-2.1-or-later → GPL-3.0-or-later)

The Direct3D 9 frontend and its DXSO/fixed-function shader translator were
imported from a different fork of the same upstream:

- **Origin:** `https://github.com/dacevedo12/dxmt.git`
- **Tag:** `v0.4-d3d9`, commit `e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b`
- **Upstream licence at that tag:** **LGPL-2.1-or-later**
  ("Copyright (c) 2023-2026 Feifan He for CodeWeavers"), full text in
  `COPYING.LIB`. That repository's `LICENSE.OLD` records that releases up to
  v0.80 were MIT; this fork branched in the MIT era, which is why the `LICENSE`
  here is still the MIT one and why it stays that way — it states the terms of
  the code this fork actually took from upstream.

LGPL-2.1 **§3** expressly permits a recipient to distribute a copy of the
library under the terms of the ordinary GNU GPL instead. That option is
exercised here, to GPL-3.0-or-later (`COPYING.GPL-3.0`), which is the licence
Madeira's own modifications already carry and which the combined application is
distributed under. Nothing is withdrawn from anyone: the same code remains
available from its own upstream under LGPL-2.1-or-later, and this conversion
applies only to the copy distributed as part of Madeira. This is exactly what
the main repository already did for its Wine fork (see `wine/LICENSE-MADEIRA.md`
and the root `README.md`). Every upstream copyright and licence notice in the
imported files is kept intact.

### Files imported from that tag

Whole files, unmodified except where a compile fix is noted in the source:

| Path | Files |
|---|---|
| `src/d3d9/**` | 71 (the whole directory, including `meson.build`, `d3d9.def`, `version.rc`) |
| `src/airconv/dxso_header.hpp`, `dxso_decoder.hpp`, `dxso_compile.{hpp,cpp}`, `ffp_compile.{hpp,cpp}` | 6 |

Blocks spliced into files this fork already had (the surrounding files stay
under their existing terms; the imported blocks are LGPL-2.1-or-later,
distributed under GPL-3.0-or-later as above):

| Path | What was taken |
|---|---|
| `src/airconv/airconv_public.h` | the DXSO public API: `dxso_shader_t`/`dxso_bitcode_t`, the `DXSO_*` argument structs and enums, and the five `DXSO*` entry points. The SM50 half of the reference header (its `AIRCONV_VERSION`, `SM50_BINDING_INDEX`, `SM50_SHADER_FLAG`, root-signature argument) was deliberately **not** taken — it would change the SM50 wire format this fork's d3d11 already uses. |
| `src/airconv/air_signature.{hpp,cpp}` | `air::InputPointCoord` and the `OutputPointSize` function output, with their AIR metadata arms |
| `src/airconv/nt/air_builder.{hpp,cpp}` | `AIRBuilder::FPBinOp::pow` |
| `src/winemetal/airconv_thunks.{h,c}` | the DXSO unix-call slot numbers, parameter structs (and their `*32` mirrors) and PE-side thunks |
| `src/winemetal/unix/winemetal_unix.c` | the DXSO 32-bit argument-chain converter (`dxso_compilation_argument32_convert`/`_free`) and the `thunk_DXSO*` / `thunk32_DXSO*` handlers |

Everything else in the D3D9 path — the guest-window pointer conversions, the
32-bit dispatch-table variants, the iOS build stages — is Madeira's own work
under GPL-3.0-or-later.

Where a file contains both, the file as a whole may only be distributed under
terms compatible with GPL-3.0-or-later, because the GPL-covered contributions
cannot be separated from it. The underlying upstream code remains available
under MIT **from upstream**, and nothing here withdraws that.

## Why

The intent is that derivatives of this work which are *distributed* remain open
source. MIT permits a proprietary derivative; the GPL does not. MIT is
GPL-compatible, so combining them this way is permitted.

Three limits, stated plainly rather than left implied:

- The GPL constrains **distribution**. It does not restrict private
  modification, internal use, or a separate program that merely invokes this
  one.
- **Modifications published earlier, while this repository presented itself as
  MIT, were granted under MIT. That grant cannot be revoked.** Only
  contributions made from 2026-08-28 onward are GPL-only. Anyone who already has a
  copy keeps their MIT rights to it.
- It cannot stop anyone independently reimplementing the same ideas.

## Contributing

Contributions are accepted under **GPL-3.0-or-later**. See `CONTRIBUTING.md`.
