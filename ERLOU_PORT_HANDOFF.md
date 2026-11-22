# AnyKyty + Erlou KytyPS5-Yotei GPU Backend Port — Session Handoff

*Last updated: 2026-11-21 22:10 EST*

## 1. Project Goal, Guardrails & Current State
- **Goal**: Optimize and stabilize **Ghost of Yōtei (`PPSA26344`, v02.002.000)** on Windows **AnyKyty + Erlou KytyPS5-Yotei GPU backend port** (`D:\Games\AnyKyty`) with native hardware Ray Tracing (`VK_KHR_ray_query`), real Big-O CPU/GPU optimizations, async shader variant compilation, and GPU-driven indirect compute dispatch.
- **Current Repo Snapshot**:
  - Committed as **source code only** to `https://github.com/akshay-kamloo/AnyKyty-Yotei-RT` (branch `AnyKyty`).
  - Current tree includes all phases through **`native-rt-v9-async-variants-hints`** (`v8` shutdown race fix + `v9` `HasGpu()` clean-exit guard, async graphics shader variants `KYTY_ASYNC_GRAPHICS_VARIANTS=1`, and `.kyhints` import/export & headless precompile).
  - Verified clean exit (`exitCode: 0`, `393.1 s` gameplay session on 2026-11-21 21:56 EST) with `_PipelineCache` updated (`gfx.journal`, `cs.journal`, `PPSA26344.bin`).
- **Local Backup, Git & Repo Guardrails**:
  - Never overwrite `build\`, `*-backup*` folders/files, or `app.exe.backup` / `app.exe.1080p-drs-only`.
  - Always back up the previous working `kyty_gpu.prx` in `build-erlou-dll\` before deploying a new build.
  - Keep all builds, caches, game runtime files (`run-PPSA26344/`), toolchains (`_tools/`), `.kyhints` files (`*.kyhints`), and local backups (`core/Backup/`) out of git via `.gitignore`.
  - **Every GitHub push must also update `README.md`**; never commit `.kyhints` files to the repo (direct users to `https://github.com/Erlou93/KytyPS5-Yotei/releases`).
  - Never stage `3rdparty/ZArchive` (keep its submodule state untouched).

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
- `kyty_gpu.native-rt-v9-async-variants-hints.prx.bak` (`208,009,728` B, currently deployed to `run-PPSA26344\libs\kyty_gpu.prx`) — `v8` shutdown race fix + `v9` `HasGpu()` clean-exit guard, async graphics shader variants (`KYTY_ASYNC_GRAPHICS_VARIANTS=1`), and `.kyhints` import/export & headless precompile.

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
| `native-rt-v9-async-variants-hints` | Open world / Cutscene (900p) | 15.2–18.8 (open world) | 53–66 | Verified clean exit (`393.1 s`, `exitCode: 0`); async gfx variants + `.kyhints` precompile active |

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

### Phase 8: Shutdown Race Fix, Async Graphics Variants & `.kyhints` Precompilation (`v8`–`v9`)
- **`v8` (`aec635b16c`)**: Eliminated `graphicsRun.cpp:670` (`!m_accepting`) crash on shutdown by unhooking the fault chain/write filter before `ShutdownGpu()` in `EntryGpu.cpp`, adding `GuestGpu::TryPostCommand` in `graphicsRun.h/.cpp`, and guarding `ReadMemory` with `IsStopping()` in `bufferCache.cpp`.
- **`v9` (`native-rt-v9-async-variants-hints`)**:
  - **Clean-exit `HasGpu()` guard**: Added `RenderContext::HasGpu()` (`renderContext.h`) and guarded `bufferCache.cpp` caller-wait checks so closing the window no longer hits `renderContext.cpp:94` (`m_gpu == nullptr`).
  - **Async graphics shader variant compilation (`KYTY_ASYNC_GRAPHICS_VARIANTS`, default `1`)**: Offloads first-time non-writing graphics stage permutation compiles (`TranslateProgram` + `CompileProgram` + `vkCreateShaderModule`) onto `TableCompileWorkers` in `pipelineCache.cpp`, skipping only those draws in `RenderExecutor::PrepareDrawRenderState` (`renderDraw.cpp`) until ready (eliminating 100–465 ms `SlowFrame::Variant` freezes in brand-new open-world areas).
  - **`.kyhints` import/export & upfront precompile-only mode**: Wired `KYTY_IMPORT_HINTS`, `KYTY_EXPORT_HINTS`, `KYTY_GAME_DIR` (auto-detects `./app0`), `KYTY_PRECOMPILE_ONLY`, `KYTY_PRECOMPILE_THREADS`, and `KYTY_SHADER_PRECOMPILE_WAIT_SEC` in `KytyShim/src/Config.cpp`, `EntryGpu.cpp`, and `pipelineCache.cpp`.
## 6. Upstream & Community References (Checked 2026-11-21 Evening)
- **`boykopovar/AnyPS5` Discussion #675**: https://github.com/boykopovar/AnyPS5/discussions/675 (RTX 3080 Laptop ~9.5 FPS, RTX 4090 + i9-13900K ~15 FPS at 1080p on `yotei/best`; ray tracing not served upstream yet).
- **`Celegans12/AnyPS5-Ghost-of-Yotei` (`yotei/best`)**: https://github.com/Celegans12/AnyPS5-Ghost-of-Yotei/tree/yotei/best (commit `655282b091` on 2026-11-21 evening, based on `AnyKyty 70f1d69fb6c` + 19 Yōtei commits: cloth wave64 fix, stencil storage, helper lanes, idle image write-back, in-place label deferral).
- **`simnJS/AnyPS5` (`AnyKyty` branch)**: https://github.com/simnJS/AnyPS5 (`origin/HEAD` at `5e6610c64a` — 2,339 commits ahead of our base `de0fa047bb`).
- **`Erlou93/KytyPS5-Yotei` (v0.1.6)**: https://github.com/Erlou93/KytyPS5-Yotei/releases and https://github.com/Erlou93/KytyPS5-Yotei/blob/main/README-YOTEI.md (19–20 FPS town / 26–27 FPS open world on 9800X3D + RTX 5070 Ti 16 GB).

## 7. Next Session Priorities
1. **Diagnose Missing Character Voices in Cutscenes**:
   - Launch with `.\run-diag-ingame.ps1 -Label v9-cutscene` after setting `$env:APS5_TRACE_AUDIOOUT2 = '1'` (and `$env:APS5_TRACE_AJM = '1'` if needed), play ~15–20 s of the cutscene, then inspect `run-ingame-diag.log`.
   - Also test switching in-game **Audio Settings** output mode to **Headphones** or **TV Speakers (Stereo)** to isolate whether dialogue is lost in `libSceAudioOut` (`sceAudioOut` vs. `sceAudioOut2` port routing/attributes) or `libSceAjm.native` (AT9/Opus/AAC decoding).
2. **Profile & Optimize Cutscene Low FPS**:
   - Inspect `SLOW_FRAME` metrics from the same `v9-cutscene` diagnostic log (`run-play.ps1` does not write logs) to identify the primary cutscene bottleneck (`draw_op`, `disp_op`, `fence`, `rebind_buf`, `variant`, or VRAM budget pressure).
3. **Cherry-Pick Upstream Stability & Rendering Fixes**:
   - Evaluate pulling the cloth wave64 fix, stencil storage, helper lanes, and idle image write-back from `simnJS/AnyPS5` (`AnyKyty`) / `Celegans12` `yotei/best`.
4. **Re-train PGO**:
   - Capture a fresh PGO profile covering both cutscenes and open world, then rebuild `kyty_gpu.prx` with `-PgoMode USE`.
5. **Repo Hygiene Reminder**:
   - Always update `README.md` on every GitHub push, never commit `.kyhints` files (link users to `https://github.com/Erlou93/KytyPS5-Yotei/releases`), and keep `3rdparty/ZArchive` unstaged.
