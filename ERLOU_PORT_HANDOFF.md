# AnyKyty + Erlou KytyPS5-Yotei GPU Backend Port — Session Handoff

*Last updated: 2026-11-21 20:25 EST*

## 1. Project Goal, Guardrails & Current State
- **Goal**: Optimize and stabilize **Ghost of Yōtei (`PPSA26344`, v02.002.000)** on Windows **AnyKyty + Erlou KytyPS5-Yotei GPU backend port** (`D:\Games\AnyKyty`) with native hardware Ray Tracing (`VK_KHR_ray_query`), real Big-O CPU/GPU optimizations, and GPU-driven indirect compute dispatch.
- **Current Repo Snapshot**:
  - Committed as **source code only** to `https://github.com/akshay-kamloo/AnyKyty-Yotei-RT` (branch `AnyKyty`).
  - Current tree includes all phases through **`native-rt-v7-smart-bar-callerwait`** (see Section 5 for the known `graphicsRun.cpp:670` assert in `v7` vs. stable `v6`).
- **Local Backup & Safety Guardrails**:
  - Never overwrite `build\`, `*-backup*` folders/files, or `app.exe.backup` / `app.exe.1080p-drs-only`.
  - Always back up the previous working `kyty_gpu.prx` in `build-erlou-dll\` before deploying a new build.
  - Keep all builds, caches, game runtime files (`run-PPSA26344/`), toolchains (`_tools/`), and local backups (`core/Backup/`) out of git via `.gitignore`.

## 2. Host Environment & Toolchain
- **OS / Hardware**: Windows 11 (build 26300), Intel Core Ultra 7 270K Plus (24 threads), 31 GiB RAM, NVIDIA GeForce RTX 5070 (driver 621.01, 12,227 MiB VRAM, 11,943 MiB `device_local`).
- **Repo root**: `D:\Games\AnyKyty` (branch `AnyKyty`, base `simnJS/AnyPS5` commit `de0fa047bb` + Erlou `Erlou93/KytyPS5-Yotei` GPU backend port).
- **Active build dir**: `D:\Games\AnyKyty\build-erlou-dll\` (`ANYPS5_KYTY_DLL=ON`, ThinLTO, PGO).
- **Toolchain** (local, excluded from git):
  - Portable `llvm-mingw` (Clang 23.1.6 + `libc++` + `lld` + `llvm-profdata`): `_tools\llvm-mingw\bin\`
  - MinGW-w64 GCC 15.2.0: `_tools\winlibs\mingw64\bin\`
  - CMake 4.5.1 + Ninja: `_tools\cmake\cmake-4.5.1-windows-x86_64\bin\`
- **Run directory**: `D:\Games\AnyKyty\run-PPSA26344\` (currently configured to **900p** via `set-res-profile.ps1 -Profile 900p`: `1600x900`, RTGI pool `+48 MiB`, `LockDRS=True`).

## 3. Local Binary Backups (`build-erlou-dll\`, excluded from git)
- `kyty_gpu.thinlto.prx.bak` — pre-PGO ThinLTO DLL.
- `kyty_gpu.pgo-v1.prx.bak` — ThinLTO + PGO v1 DLL (pre-Big-O).
- `kyty_gpu.big-o-v1-pgo-v2.prx.bak` — Big-O v1 + PGO v2 DLL (**23.26 FPS / 43.0 ms/f** in village @ 1080p).
- `kyty_gpu.native-rt-v4-bigo-prune.prx.bak` — Native RT + `BufferCache` O(1) hash indexing & bounding-box pruning + `FAULT_GPU_RIP` sampler.
- `kyty_gpu.native-rt-v5-gpu-indirect-td.prx.bak` — First GPU indirect thread-dimension dispatch build.
- `kyty_gpu.native-rt-v6-gpu-indirect-td-fix.prx.bak` — **Last verified stable build** (ran 9,518 frames cleanly into open world at **19.4–25.3 FPS @ 900p** and **8.1–11.1 FPS** in heavy post-fight cutscene, `fault_gpu = 0 ms`).
- `kyty_gpu.native-rt-v7-smart-bar-callerwait.prx.bak` — Adaptive BAR cap + page-level `PlanCallerWait` fallback (faster cutscene frames at 71–77 ms, `fence` 0–6 ms, but hit fatal assert `condition (!m_accepting) is true in graphicsRun.cpp:670` at frame 4,433).

## 4. Verified Performance Progression
| Milestone / Build | Scene / Resolution | FPS | ms/f | Key Notes |
| :--- | :--- | ---: | ---: | :--- |
| `village-pre-hooks` | Village (1080p) | 9.31 | 107.4 | Initial baseline (`ref_backing=3.6M`, `fault_gpu=2,858`) |
| `ab-dll-trace` (ThinLTO) | Village (1080p) | 17.73 | 56.4 | `kyty_gpu.prx` DLL + ThinLTO (+17.2% vs non-DLL) |
| `true-1080p-pgo` | Village (1080p) | 18.62 | 53.7 | 5-site 1080p patch + PGO v1, VRAM `-860 MiB` (`9,135 MiB`) |
| `big-o-pgo-v2` | Village (1080p) | **23.26** | **43.0** | Big-O v1 + PGO v2 (steady frames 26–32 FPS) |
| `native-rt-v4-bigo-prune` | Post-fight cutscene (1080p) | 3.3–3.6 | ~285 | `fault_gpu` 2,208–2,472 ms/5s from indirect thread-dim dispatches |
| `native-rt-v6-gpu-indirect-td-fix` | Post-fight cutscene (1080p) | 5.6–6.1 | ~170 | GPU indirect thread-dims (`dispatch_thread_dims.comp`); **`fault_gpu = 0 ms`** |
| `native-rt-v6-gpu-indirect-td-fix` | Post-fight cutscene (900p) | 8.1–11.1 | 90–123 | Switched to 900p (`1600x900`) to stay inside 12 GB VRAM budget |
| `native-rt-v6-gpu-indirect-td-fix` | **Open world (900p)** | **19.4–25.3** | **39.5–51.5** | **Stable across 9,518 frames** |
| `native-rt-v7-smart-bar-callerwait` | Post-fight cutscene (900p) | ~13.0–14.1 | 71–77 | `fence` dropped to 0–6 ms, but asserted at frame 4,433 (`graphicsRun.cpp:670`) |

## 5. Completed Optimizations Summary
### Phases 1–3: Erlou Backend Port & Native RT
- Ported Erlou `graphics/` + `common/` Windows backend into `core/kyty/` behind `core/libs/prx/libSceAgcDriver/KytyShim/` (`KYTY_DLL=ON`, Clang + `libc++` + `lld` + ThinLTO + PGO).
- Added native hardware Ray Tracing (`VK_KHR_ray_query`) and BVH `MappedPrefix` fix.
- Windows `VirtualProtect` coalescing (`MemoryBacking.cpp`), 16 MiB Windows thread stacks, `WriteBacking` non-reentrant fix, and 4 `ResourceMaterialization` hooks + 4 KiB page cache.

### Phases 4–5: Big-O v1 & v2
- `MemoryBacking.cpp`: 64-page hierarchical backed bitmap (`O(P/64)` `IsBacked()`).
- `descriptors.cpp` / `render.h`: `O(1)` 128-slot open-addressed epoch hash index in `PlanFlags()`; removed 5 `thread_local` vectors from `RenderExecutor` to eliminate dynamic TLS guard overhead.
- `SrtTape.h` / `SrtTape.cpp`: Precomputed `fusable_read` per slot and memo-first `SrtTapeWalker::Eval()`.
- `rangeSet.h`: 2-level chunked B-tree (`BlockCap=64`) with compact `m_block_max_end` array and `O(1)` bounding-box fast-reject.

### Phase 6: Native RT Big-O Pruning & GPU Indirect Thread-Dimension Dispatch (`v4`–`v6`)
- **`native-rt-v4-bigo-prune`**: Added `O(1)` hash indexing and `O(1)` bounding-box pruning in `BufferCache`, plus `FAULT_GPU_RIP` sampler (`Faults.cpp`).
- **`native-rt-v5` / `v6` (`dispatch_thread_dims.comp`)**: Moved `COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS` indirect compute dispatches onto the GPU via `core/kyty/graphics/host_gpu/shaders/dispatch_thread_dims.comp` + `RenderExecutor::DispatchIndirect` + `ShaderData` UBO patching + `pipelineCache.cpp` `PushData::NoStart` fix. Eliminated `fault_gpu` stalls from **2,208–2,472 ms/5 s → 0 ms**.

### Phase 7: Adaptive BAR Cap & Caller-Wait Fallback (`v7` — Experimental)
- **`bufferCache.cpp`**: Added adaptive BAR cap (`EffectiveBarMaxBytes`, 64 KiB cap at 30–200 MB/s) and page-level fallback in `PlanCallerWait`.
- **Known Regression**: Hits `Error: condition (!m_accepting) is true in core/kyty/graphics/guest_gpu/graphicsRun.cpp:670` during long runs (frame 4,433). Needs root-cause fix or revert of `bufferCache.cpp` back to `v6` for stability.

## 6. Upstream & Community References (Checked 2026-11-21 Evening)
- **`boykopovar/AnyPS5` Discussion #675**: https://github.com/boykopovar/AnyPS5/discussions/675 (RTX 3080 Laptop ~9.5 FPS, RTX 4090 + i9-13900K ~15 FPS at 1080p on `yotei/best`; ray tracing not served upstream yet).
- **`Celegans12/AnyPS5-Ghost-of-Yotei` (`yotei/best`)**: https://github.com/Celegans12/AnyPS5-Ghost-of-Yotei/tree/yotei/best (commit `655282b091` on 2026-11-21 evening, based on `AnyKyty 70f1d69fb6c` + 19 Yōtei commits: cloth wave64 fix, stencil storage, helper lanes, idle image write-back, in-place label deferral).
- **`simnJS/AnyPS5` (`AnyKyty` branch)**: https://github.com/simnJS/AnyPS5 (`origin/HEAD` at `5e6610c64a` — 2,339 commits ahead of our base `de0fa047bb`).
- **`Erlou93/KytyPS5-Yotei` (v0.1.6)**: https://github.com/Erlou93/KytyPS5-Yotei/releases and https://github.com/Erlou93/KytyPS5-Yotei/blob/main/README-YOTEI.md (19–20 FPS town / 26–27 FPS open world on 9800X3D + RTX 5070 Ti 16 GB).

## 7. Next Priorities
1. **Stability First**: Either diagnose and fix the `graphicsRun.cpp:670` (`!m_accepting`) assertion triggered under `v7`'s `bufferCache.cpp` change, or revert `v7` back to stable `v6` (`kyty_gpu.native-rt-v6-gpu-indirect-td-fix.prx.bak`).
2. **Cherry-Pick Upstream Stability Fixes**: Evaluate pulling the cloth wave64 fix, stencil storage, helper lanes, and idle image write-back from `simnJS/AnyPS5` (`AnyKyty`) / `Celegans12` `yotei/best`.
3. **Re-train PGO**: Once stability is locked in, capture a fresh PGO profile covering both cutscenes and open world and rebuild with `-PgoMode USE`.
