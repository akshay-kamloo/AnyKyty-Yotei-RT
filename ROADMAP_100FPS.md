# AnyKyty-Yotei-RT — 100+ FPS Architectural Optimization Plan (`<= 10.0 ms/frame`)

*Living engineering roadmap — read, refer to, and update across sessions.*
*Created: 2026-11-24 | Last updated: 2026-11-24 | Current Baseline: `v27-kytyps5-gta-merge` (`c175b705ad`)*

## 1. North-Star Target & Hardware Envelope
- **Target**: **100+ FPS (`<= 10.0 ms/frame`)** in *Ghost of Yōtei* (`PPSA26344`, v02.002.000, 900p `1600x900` + Native `VK_KHR_ray_query` RT). Intermediate gate: **60 FPS (`<= 16.67 ms/frame`)**.
- **Scope**: Full architectural redesigns are explicitly authorized (parallel Vulkan command recording, shadow-page coherence, `VK_EXT_descriptor_buffer`, dedicated transfer/async-compute queues, `GetWriteWatch` / software dirty tracking, SPIR-V shader optimization).
- **Host Rig**: Windows 11 (26300), **Intel Core Ultra 7 270K Plus** (24 threads: 8P + 16E, non-SMT), 31 GiB RAM, **NVIDIA GeForce RTX 5070** (12 GB VRAM, driver 621.01).
- **Build & Test Commands**:
  - Build ThinLTO + PGO DLL: `.\build-erlou-dll.ps1 -PgoMode USE -PgoProfile D:\Games\AnyKyty\build-erlou-dll\kyty-dll-pgo\yotei-v3.profdata`
  - Diagnostic benchmark: `D:\Games\AnyKyty\run-PPSA26344\run-diag-ingame.ps1` -> `run-ingame-diag.log`
  - Clean play test: `D:\Games\AnyKyty\run-PPSA26344\run-play.ps1`
  - Emergency rollback to `v23b`: `D:\Games\AnyKyty\run-PPSA26344\rollback-to-v23b.ps1`

## 2. Baseline (`v27`) vs. 100+ FPS Frame Budget
Measured on `diag-20261124-210552-v27-kytyps5-gta-merge.log` (`1,756` frames, `draws >= 1000`, `2,901` med draws/f, `1,998` med dispatches/f):

| Subsystem / Bucket | `v27` Med ms (Calls/f) | `v27` Key Bottleneck Metric | 60 FPS Budget | 100+ FPS Budget |
|---|---:|---|---:|---:|
| **Total CPU Critical Path** | **`47.00 ms`** (`21.3 FPS`) | `p10=39.1 ms`, `p90=60.8 ms` | `<= 16.0 ms` | **`<= 9.0 ms`** |
| **Total GPU (`vgpu_clock busy`)** | **`~26.2 ms`** (`55.6%` busy) | `9,318` CBs / 5 s (~88 submits/f WDDM bubbles) | `<= 15.5 ms` | **`<= 9.5 ms`** |
| `disp_op` + `draw_op` + `draw_ind` | `29.90 ms` (`4,919` ops) | 100% serial on 1 P-core; 20+ cores idle | `<= 12.0 ms` | **`<= 4.5 ms`** (5–6 P-cores) |
| `rebind_buf` + `prep_today` | `13.50 ms` (`6,598` each) | `obtain_memo` 55.4% miss / `write_memo` 53.8% miss (`3.54M` range misses/5s) | `<= 4.0 ms` | **`<= 1.2 ms`** |
| `commit` + `bindless` + `rebind_img` + `bda` | `8.30 ms` | Per-draw `vkUpdateDescriptorSets` (~6,600/f) | `<= 3.0 ms` | **`<= 1.3 ms`** |
| `fence` + `download` + `commands` | `6.8` / `5.9` / `7.1 ms` | `invalidate` (`1,035` dls, `1,916 ms` wait) + `fault_game` (`441` dls, `1,250 ms` wait) + `5,012` `other` submits/5s | `<= 2.5 ms` | **`<= 0.8 ms`** (2–4 submits/f) |
| `idle` (GPU thread starved) | `4.60 ms` (`5.37` avg) | Game CPU threads stalled `3.17 s / 5 s` | `<= 1.0 ms` | **`<= 0.2 ms`** |
| `reads` (SRT walker) | `2.80 ms` (`4,878`) | Long SRT walkers + per-load probing | `<= 1.2 ms` | **`<= 0.6 ms`** |
| `protect` + `sync_buf` + `targets` | `4.40 ms` | ~600 `VirtualProtect`/f (24-core TLB shootdowns) + `163k` lock waits/5s | `<= 1.5 ms` | **`<= 0.6 ms`** |

*(Because CPU frame `N+1` overlaps GPU frame `N`, **both** CPU critical path and GPU execution time must be `<= 10.0 ms` to sustain 100+ FPS.)*
## 3. Five Architectural Pillars

### Pillar 1 — Zero-Stall Coherence & Submit Batching (`~17 ms -> < 1.0 ms`)
- **Files**: `core/kyty/graphics/host_gpu/renderer/cache/bufferCache.{h,cpp}`, `commandScheduler.{h,cpp}`, `memoryTracker.{h,cpp}`
- **Plan**:
  1. **Hot-invalidate eager prefetch + submit coalescing (`v28`)**: Extend `RecordHotRead()` (`bufferCache.cpp:1388`) / `PrefetchEagerReadbacks()` to hot write-fault pages; audit `submit other=5,012` and `periodic=2,022` (~88 submits/f).
  2. **Shadow-page CPU write coherence (`v30`)**: On a CPU write fault (`invalidate`) to a GPU-modified page (not dead-ring), let the CPU write to a shadow copy, track dirty byte/cacheline masks, and merge-upload only written bytes on GPU before the next read. Eliminates synchronous `invalidate` GPU->CPU downloads.
  3. **Dedicated Vulkan Transfer Queue (`VK_QUEUE_TRANSFER_BIT`) + 2–4 submits/frame**: Route unavoidable async readbacks/uploads to the hardware copy engine; batch graphics/compute CBs to 2–4 WDDM submits/frame.

### Pillar 2 — Bindless / `VK_EXT_descriptor_buffer` & Pass-Level Validation (`21.8 ms -> < 2.5 ms`)
- **Files**: `core/kyty/graphics/host_gpu/renderer/pipeline/descriptors.cpp`, `descriptorHeap.{h,cpp}`, `bindlessTable.{h,cpp}`, `cache/bufferCache.cpp`
- **Plan**:
  1. **4-way set-associative `g_obtain_memo` & `g_write_memo` (`v28`)**: Replace 1-way direct-mapped `g_obtain_memo[8192]` & `g_write_memo[8192]` (`bufferCache.cpp:2999, 3102`) with 4-way set-associative static tables to crush `3.54M / 5 s` range collisions. *(Keep static/out-of-line — never bloat `RenderExecutor`.)*
  2. **Pass-level epoch validation (`v31`)**: Validate `BufferCache`/`TextureCache` epochs once per render-pass / memory-barrier boundary instead of `6,600` times/frame.
  3. **`VK_EXT_descriptor_buffer` / BDA-direct binding (`v31`)**: Replace per-draw `vkUpdateDescriptorSets` with native RTX 5070 `VK_EXT_descriptor_buffer` or persistent BDA 64-bit pointer push constants.

### Pillar 3 — Multi-Core Parallel Vulkan Command Recording (`29.9 ms -> <= 4.5 ms`)
- **Files**: `core/kyty/graphics/guest_gpu/graphicsRun.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/render.{h,cpp}`, `renderDraw.cpp`, `renderCompute.cpp`
- **Plan**:
  1. **Decouple PM4 parsing from Vulkan recording (`v32`)**: Front-end thread emits a flat, lock-free **Operation IR Ring**, chunked at render-pass / barrier / sync boundaries.
  2. **4–6 P-Core Secondary CB Workers (`v32`)**: Fan out chunks across 4–6 reserved P-cores (`ReserveCores`), each with its own `VkCommandPool` (`VK_COMMAND_BUFFER_LEVEL_SECONDARY`) and thread-local `DescriptorHeap` / descriptor-buffer slice, reading lock-free cache snapshots.
  3. **Ordered Primary Stitch**: Primary execution thread runs `vkCmdExecuteCommands()` in exact guest order.
### Pillar 4 — True Async Compute Queue + RTX 5070 GPU Budget (`~26 ms -> <= 9.5 ms`)
- **Files**: `core/kyty/graphics/presentation/window/vulkanWindow.cpp`, `commandScheduler.{h,cpp}`, `core/kyty/graphics/shader/recompiler/backend/spirv/spirvEmitter*.cpp`
- **Plan**:
  1. **`KYTY_GPU_TIME` per-shader profiler (`v28`)**: Port `Celegans12@7ea5479c` to measure exact GPU microseconds per shader/marker (separating real RTX 5070 shader time from WDDM submit bubbles).
  2. **Real `VK_QUEUE_COMPUTE_BIT` queue (`v33`)**: Route PS5 async-compute passes (cloth, particles, BVH/RT prep, culling) to a second hardware compute queue with timeline-semaphore sync to overlap with rasterization.
  3. **Targeted SPIR-V optimization (`v34`)**: Optimize top 15–20 heaviest GPU shaders from `KYTY_GPU_TIME` (wave32 lowering, fast-math/NaN pruning, barrier elision, optional RTGI/upscaler tuning).

### Pillar 5 — Eliminate `VirtualProtect` TLB Shootdowns (`1.5 ms` + lock contention -> `< 0.2 ms`)
- **Files**: `core/kyty/graphics/host_gpu/pageManager.{h,cpp}`, `core/libs/prx/libSceAgcDriver/KytyShim/src/MemoryGpu.cpp`
- **Plan**: Replace hot-ring OS `VirtualProtect` cycles (~600/f, 24-core TLB shootdowns) with Windows `MEM_WRITE_WATCH` / `GetWriteWatch` or software epoch dirty-bit tracking.

## 4. Phased Execution Checklist
### Milestone A — Fast Wins & GPU Visibility (`v28–v29` | Gate: `30–35 FPS` / `28–33 ms`)
- [ ] **`v28a`**: 4-way set-associative `g_obtain_memo` & `g_write_memo` (`bufferCache.cpp`).
- [ ] **`v28b`**: Port `KYTY_GPU_TIME` (`Celegans12@7ea5479c`) + audit `submit other=5,012` / `periodic=2,022` & hot `invalidate` addresses.
- [ ] **`v29a`**: Port `Celegans12` SRT walker stack (`71585e5a`, `38f78397`, `15f8686b`).
- [ ] **`v29b`**: Capture 15-min `yotei-v5.profdata` PGO profile (recover 500B+ discarded `yotei-v3` counts).

### Milestone B — Coherence & Descriptor Overhaul (`v30–v31` | Gate: `55–65 FPS` / `15–18 ms`)
- [ ] **`v30`**: Shadow-page CPU write coherence + submit batching (88 -> 2–4/f) + dedicated transfer queue.
- [ ] **`v31`**: `VK_EXT_descriptor_buffer` (or persistent BDA ring) + pass-level epoch validation.

### Milestone C — Parallel Recording & GPU Floor (`v32–v34` | Gate: `85–110+ FPS` / `<= 10.0 ms`)
- [ ] **`v32`**: 4–6 P-core parallel `VK_COMMAND_BUFFER_LEVEL_SECONDARY` Vulkan recording.
- [ ] **`v33`**: Real Vulkan `VK_QUEUE_COMPUTE_BIT` async compute queue + `GetWriteWatch` / software dirty tracking.
- [ ] **`v34`**: `KYTY_GPU_TIME`-guided SPIR-V shader optimization + final 15-min PGO retrain.

## 5. Hard Guardrails & Known Traps
1. Never bloat `RenderExecutor` with large inline arrays/vectors (`v24` L1d regression); keep tables static/out-of-line.
2. Keep `ShaderVertexInputBuffer` & `ShaderVertexDestination` initialized (`KytyPS5@99202b5d` caused cutscene character freeze).
3. Never pin helper threads to P-cores without `ReserveCores` (`v21`), and keep parallel workers lock-free on `BufferCache` (`v18`).
4. Never overwrite `build\`, `*-backup*`, `app.exe.*`, or existing `*.prx.bak`; always back up `kyty_gpu.prx` before deploy; on every push update `README.md` + this file, never commit `*.kyhints`, never stage `3rdparty/ZArchive`.

## 6. Version Progress Log (Update After Every Benchmarked Build)
| Version | Commit | Med ms (FPS) | p10 ms (FPS) | p90 ms | Key Delta |
|---|---|---:|---:|---:|---|
| `v23b` | `3de11d9156` | `56.7 ms` (`17.6`) | `40.0 ms` (`25.0`) | `79.3 ms` | 3-layer shader compile fix (zero 53 s stalls) |
| `v26` | `e258524937` | `49.2 ms` (`20.3`) | `38.9 ms` (`25.7`) | `63.9 ms` | `256 KiB` read window (`-92.8%` dl), dirty-only `ImageHeap`, upstream KytyPS5/Celegans12 |
| `v27` | `c175b705ad` | **`47.0 ms` (`21.3`)** | **`39.1 ms` (`25.6`)** | **`60.8 ms`** | 6 `KytyPS5-GTA` GPU opts (`+4.7%` med FPS, `fence 6.8 ms`, `dl 5.9 ms`) |
| `v28` | *pending* | — | — | — | 4-way `obtain_memo`/`write_memo` + `KYTY_GPU_TIME` + `invalidate`/submit cuts |