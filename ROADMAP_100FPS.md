# AnyKyty-Yotei-RT â€” 100+ FPS Architectural Optimization Plan (`<= 10.0 ms/frame`)

*Living engineering roadmap â€” read, refer to, and update across sessions.*
*Created: 2026-11-24 | Last updated: 2026-11-25 | Current Baseline: `v27-kytyps5-gta-merge` (`c175b705ad`)*

## 1. North-Star Target, Acceptance Gates & Hardware-Agnostic Design

### 1.1 Performance & Stability Gates (Across All 4 Scenes)
Every milestone is verified across a fixed **4-scene benchmark suite** (**Open World traversal**, **Heavy Combat / particles**, **Dense Town / Village**, and **Cutscenes**) rather than a single static spawn point:

| Metric | Intermediate Gate (60 FPS) | Final Target (100+ FPS) |
| :--- | ---: | ---: |
| **Median Frame Time** | `<= 16.0 ms` (`>= 62.5 FPS`) | **`<= 9.5 ms` (`>= 105 FPS`)** |
| **p95 / 1% Low Frame Time** | `<= 20.0 ms` (`>= 50 FPS`) | **`<= 11.5 ms` (`>= 87 FPS`, zero `> 20 ms` hitches)** |
| **CPU Critical Path** | `<= 15.5 ms` | **`<= 9.0 ms`** |
| **True GPU Time (`KYTY_TRACE_GPU_TIME=2`)** | `<= 15.5 ms` | **`<= 9.5 ms`** |
| **Sustained VRAM (30-min soak)** | `<= 88%` of live `heapBudget` | **`<= 85%` of live `heapBudget` (15% headroom)** |
| **Sustained Host RAM (caches/pools)** | `<= 85%` of physical RAM | **`<= 80%` of physical RAM (20% headroom)** |

*(Because CPU frame `N+1` overlaps GPU frame `N`, **both** the CPU critical path and true GPU execution time must stay `<= 9.5 ms` at the chosen resolution profile to hold stable 100+ FPS throughout the game.)*

### 1.2 Hardware-Agnostic Engineering Principles (Mandatory Across All Pillars)
1. **Dynamic CPU Topology & Maximum Utilization (No Hardcoded Core Counts):**
   - Detect CPU topology at runtime via Windows `GetLogicalProcessorInformationEx` (`RelationProcessorCore`, `EfficiencyClass`, SMT sibling pairs, and L3 cache / CCD groups).
   - Scale worker counts (Resolve Helpers, parallel Secondary-CB / `RecordPackets` workers, shader precompile, `TableCompileWorkers`, `BarCopyPool`) automatically from detected physical cores and P-core/E-core split â€” working seamlessly on 6-core, 8-core, 12-core, 16-core, hybrid Intel P+E, and AMD single/dual-CCD CPUs.
   - Always reserve pinned GPU/recording/helper cores through `ReserveCores` so guest game threads never preempt them; route background workloads (precompile, async variants, `BarCopyPool`, GC) to E-cores (when present) or non-reserved cores.
2. **Percentage-Based VRAM & System RAM Headroom (No Hardcoded GiB Caps):**
   - Query live Vulkan `VK_EXT_memory_budget` (`heapBudget` / `heapUsage` on `DEVICE_LOCAL` heaps) and Windows `GlobalMemoryStatusEx` (`ullTotalPhys` / `ullAvailPhys`).
   - Enforce dynamic percentage watermarks across any GPU/RAM size (8 GB, 12 GB, 16 GB, 24 GB+ VRAM; 16 GB, 32 GB, 64 GB+ RAM):
     - **VRAM:** Soft-park/evict unused images & staging buffers at **`85%`** of `heapBudget`; aggressive GC at **`92%`** (preserving **`15%`** WDDM headroom to prevent driver spillover cliffs).
     - **RT Acceleration-Structure Arena (`KYTY_RT_ARENA_MB` auto):** Percentage of `heapBudget` (default `20â€“25%`, auto-scaled).
     - **Host RAM:** Cap shader-reuse / plan / staging pools proportionally so total process working set leaves **`>= 15â€“20%`** system RAM headroom.
3. **Multi-Resolution Support (User-Selectable Profiles):**
   - Never hardcode a single internal resolution (`900p` or otherwise). Support clean user-selectable resolution profiles (`720p`, `810p`, `900p`, `1080p`, `1440p`, `4K`) via `set-res-profile.ps1` / `APS5_OUTPUT_RESOLUTION`, plus optional host-side upscaling (`ANYPS5_UPSCALE` FSR/NIS) and optional RTGI budget scaling, so players can choose resolution vs. GPU headroom on any GPU while hitting `<= 9.5 ms` GPU time.

### 1.3 Reference Dev Rig & Commands
- **Reference Benchmark Rig**: Windows 11 (26300), Intel Core Ultra 7 270K Plus (8P + 16E, 24 threads, non-SMT), 31 GiB RAM, NVIDIA GeForce RTX 5070 (12 GB VRAM, driver 621.01).
- **Build ThinLTO + PGO DLL**: `.\build-erlou-dll.ps1 -PgoMode USE -PgoProfile D:\Games\AnyKyty\build-erlou-dll\kyty-dll-pgo\yotei-v3.profdata`
- **Diagnostic Benchmark (CPU + Per-Stage)**: `D:\Games\AnyKyty-Perf\run-PPSA26344\run-diag-ingame.ps1 -Label <tag>`
- **True GPU Time / Per-Shader Profile**: `D:\Games\AnyKyty-Perf\run-PPSA26344\run-diag-ingame.ps1 -Label <tag> -GpuTimeMode 2` (true busy/gap) and `-GpuTimeMode 1` (`GPUTIME_TOP` per-shader ranking)
- **Clean Play Test**: `D:\Games\AnyKyty-Perf\run-PPSA26344\run-play.ps1`
- **Emergency Rollback**: `D:\Games\AnyKyty-Perf\run-PPSA26344\rollback-to-v23b.ps1`

## 2. Baseline (`v27`) vs. 100+ FPS Frame Budget

Measured on `v27-kytyps5-gta-merge` (`run-ingame-diag.log`, `1,756` steady 3D frames, `2,901` med draws/f, `1,998` med dispatches/f):

| Subsystem / Bucket | `v27` Med ms (Calls/f) | `v27` Key Bottleneck Metric | 60 FPS Budget | 100+ FPS Budget |
| :--- | ---: | :--- | ---: | ---: |
| **Total CPU Critical Path** | **`47.00 ms`** (`21.3 FPS`) | `p10=39.1 ms`, `p90=60.8 ms`, spikes `73.1 ms` | `<= 15.5 ms` | **`<= 9.0 ms`** |
| **Total GPU (`vgpu_clock busy`)** | **`~27.0 ms`** (`57%` busy) | `8,049` CBs / 5 s (~76â€“88 submits/f WDDM bubbles) | `<= 15.5 ms` | **`<= 9.5 ms`** |
| `disp_op` + `draw_op` + `draw_ind` | `29.90 ms` (`4,919` ops) | 100% serial on 1 core; `resolved_full=1,949 ms/5 s` | `<= 11.5 ms` | **`<= 4.5 ms`** (parallel CBs) |
| `rebind_buf` + `prep_today` | `13.50 ms` (`6,850` each) | `obtain_memo` 54.7% miss / `write_memo` 55.0% miss (`7.25M` range misses/5 s) | `<= 3.8 ms` | **`<= 1.2 ms`** |
| `commit` + `bindless` + `rebind_img` + `bda` | `8.30 ms` | Per-draw `vkUpdateDescriptorSets` (~6,600/f) | `<= 2.8 ms` | **`<= 1.1 ms`** |
| `fence` + `download` + `commands` | `6.8` / `5.9` / `7.1 ms` | `invalidate` (`1,286` dls, `2,361 ms/5 s` wait) + `fault_game` (`525 ms/5 s`) + `submit other=5,582/5 s` | `<= 2.2 ms` | **`<= 0.8 ms`** (`2â€“4` submits/f) |
| `idle` (GPU thread starved) | `4.60 ms` (spikes `37.8 ms`) | Game CPU threads stalled `2.89 s / 5 s` on `invalidate` + `fault_game` | `<= 0.8 ms` | **`<= 0.2 ms`** |
| `reads` (SRT walker) | `2.80 ms` (`5,050`) | Long SRT walks + per-load probing | `<= 1.0 ms` | **`<= 0.5 ms`** |
| `protect` + `sync_buf` + `targets` | `4.40 ms` | ~600 `VirtualProtect`/f (`270 ms` protect lock) + `931` image creates/5 s (`depth_overlap=678`, `overlap=343`, `163 ms` TC lock) | `<= 1.4 ms` | **`<= 0.5 ms`** |

## 3. Dynamic CPU Core & Thread Allocation Strategy

Instead of hardcoding core IDs, `core/kyty/graphics/host_gpu/renderer/` + `KytyShim` queries host topology once at startup and builds a **dynamic core budget** from `P` (high-efficiency/P-cores, or all physical cores on uniform CPUs) and `E` (efficiency cores / remaining physical cores):

- **Game Simulation & Render-Submit Threads (Guest):** Left unreserved on `max(2, P * 0.35)` high-performance cores so game logic never starves the GPU pipeline (`idle -> <= 0.2 ms`).
- **PM4 Front-End + GPU Execution Thread:** 1 dedicated reserved P-core (`ReserveCores`).
- **Parallel Secondary-CB / `RecordPackets` Workers:** `clamp(P_remaining - 2, 1, 4)` dedicated reserved P-cores (`ReserveCores`), each owning its own `VkCommandPool` (`VK_COMMAND_BUFFER_LEVEL_SECONDARY`) and thread-local `DescriptorHeap` / descriptor-buffer slice.
- **Resolve Helpers:** `clamp(P_remaining_after_recorders, 1, 2)` reserved P-cores (never more than 2 on shared `BufferCache` until lock-free snapshots land, avoiding `v18` L3 contention).
- **Background / Async Workers (`TableCompileWorkers`, Shader Precompile, `BarCopyPool`, Deferred GC):** Scheduled on E-cores (if present) or unpinned low-priority threads on uniform CPUs, capped to `min(total_logical_threads, 32)`.

*(Example on 8P+16E: 2â€“3 P-cores for guest game, 1 P-core Execution, 2â€“3 P-cores Secondary-CB Recording, 1â€“2 P-cores Resolve Helpers, all background pools on the 16 E-cores. Example on 8-core uniform AMD X3D: 3 cores guest, 1 Execution, 2 Recording, 2 Resolve Helpers, background workers on low priority.)*

## 4. Six Architectural Pillars

### Pillar 0 â€” Unlock 100+ FPS Ceiling, Multi-Resolution Profiles & True GPU Baseline (`v28a`)
- **Files**: `run-PPSA26344/run-play.ps1`, `run-PPSA26344/run-diag-ingame.ps1`, `run-PPSA26344/set-res-profile.ps1`, `core/kyty/graphics/host_gpu/renderer/gpuTimer.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/commandScheduler.cpp`
- **Plan**:
  1. **Uncap presentation & verify game simulation scaling**: Configure `ANYPS5_VBLANK_HZ=240` (or user monitor refresh rate) and `KYTY_PRESENT_MODE=mailbox` (with `immediate` option). Verify whether *Ghost of YÅtei*'s Performance Mode self-caps at 60 FPS; if so, add a clean, toggleable game-side frame-cap unlock so the engine emits `> 60` frames/sec without physics/animation speedups.
  2. **Multi-resolution profile switcher**: Ensure `set-res-profile.ps1` cleanly supports `720p`, `810p`, `900p`, `1080p`, `1440p`, and `4K` (+ optional `ANYPS5_UPSCALE` FSR/NIS), scaling RTGI/VRAM budgets dynamically by percentage rather than fixed MiB constants.
  3. **True GPU floor measurement (already built)**: Run existing `KYTY_TRACE_GPU_TIME=2` (command-buffer busy vs. gap without per-draw timestamp serialization) and `KYTY_TRACE_GPU_TIME=1` (`GPUTIME_TOP` top-25 shaders) across the 4 benchmark scenes to separate real shader/RT/tiler time from WDDM submit bubbles.
  4. **Submit & `invalidate` caller audit**: Instrument the top callers of `submit other=5,582/5 s` and the top guest virtual address ranges causing `invalidate` (`2,361 ms/5 s`) and `fault_game` (`525 ms/5 s`).

### Pillar 1 â€” Zero-Stall Coherence, DirectRanges Expansion & Submit Batching (`~17 ms -> <= 0.8 ms`)
- **Files**: `core/kyty/graphics/host_gpu/renderer/cache/bufferCache.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/commandScheduler.{h,cpp}`, `core/kyty/graphics/host_gpu/memoryTracker.{h,cpp}`, `core/kyty/graphics/presentation/window/vulkanWindow.cpp`
- **Plan**:
  1. **Expand `DirectRanges` (`VK_EXT_external_memory_host`)**: `PERF direct_ranges` already achieves **0 waits and 0 flushes** on `0x2010000000â€“0x2015e00000`. Promote the hottest CPU-written / `invalidate`-heavy guest ring ranges from `v28a` into `DirectRanges` (or eager hot-write prefetch) to eliminate the majority of synchronous `invalidate` downloads immediately.
  2. **Submit coalescing (`~80/f -> <= 8/f`, then `2â€“4/f`)**: Eliminate unnecessary mid-frame `Flush()` triggers (`other=5,582/5 s`) and raise/adapt the `NoteGpuCommand()` periodic submit threshold (`128` -> dynamic/higher) so WDDM submits drop to `2â€“4` per frame.
  3. **Shadow-page CPU write coherence**: For remaining non-DirectRange GPU-modified pages hit by CPU write faults (`invalidate`), let the CPU write to a shadow copy, track dirty cacheline/byte masks, and merge-upload only written bytes on GPU before the next read â€” removing synchronous GPU->CPU downloads from the CPU critical path.
  4. **Dedicated Vulkan Transfer Queue (`VK_QUEUE_TRANSFER_BIT`)**: Allocate a dedicated hardware DMA transfer queue (when exposed by the physical device, with graceful fallback on GPUs without one) and timeline-semaphore sync to offload unavoidable async readbacks/uploads off the graphics queue.

### Pillar 2 â€” Image Alias Reuse, Memo Associativity & `VK_EXT_descriptor_buffer` (`~22 ms -> <= 2.3 ms`)
- **Files**: `core/kyty/graphics/host_gpu/renderer/cache/bufferCache.cpp`, `core/kyty/graphics/host_gpu/renderer/cache/textureCache.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/image/image.cpp`, `core/kyty/graphics/host_gpu/renderer/pipeline/descriptors.cpp`, `descriptorHeap.{h,cpp}`, `bindlessTable.{h,cpp}`
- **Plan**:
  1. **4-way set-associative `g_obtain_memo` & `g_write_memo` (`v28b`)**: Replace 1-way direct-mapped `g_obtain_memo` / `g_write_memo` (`bufferCache.cpp:3000, 3103`) with 4-way set-associative static/out-of-line tables (strictly outside `RenderExecutor` per `v24` guardrail) to crush `7.25M / 5 s` range collisions.
  2. **Eliminate per-frame `depth_overlap` / `overlap` image thrashing (`v28b`)**: `PERF image_unregister` shows `depth_overlap=678` and `overlap=343` per 5 s (every frame on depth/stencil/R32Sfloat and half-res `R16G16B16A16Sfloat` targets, causing `931` creates / `910` deletes / `163 ms` `texture_cache` lock contention per 5 s). Reuse persistent compatible multi-view/aliased images for these overlapping formats instead of unregistering and recreating them every frame.
  3. **Pass-level epoch validation (`v31`)**: Validate `BufferCache`/`TextureCache` epochs once per render-pass / memory-barrier boundary rather than `~6,850` times/frame.
  4. **`VK_EXT_descriptor_buffer` / persistent BDA push-constant rings (`v31`)**: When the host GPU supports `VK_EXT_descriptor_buffer` (with automatic fallback to `DescriptorHeap` on unsupported GPUs), replace ~6,600/f `vkUpdateDescriptorSets` calls with direct memory-mapped descriptor writes and BDA 64-bit pointers.

### Pillar 3 â€” Multi-Core Vulkan Command Recording on `CommandRecorder` / `RecordPackets` (`~29.9 ms -> <= 4.5 ms`)
- **Files**: `core/kyty/graphics/guest_gpu/graphicsRun.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/commandRecorder.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/recordPackets.{h,cpp}`, `core/kyty/graphics/host_gpu/renderer/renderDraw.cpp`, `core/kyty/graphics/host_gpu/renderer/renderCompute.cpp`
- **Plan**:
  1. **Benchmark & harden single-worker `RecordPackets` (`v29a`)**: Measure and stabilize the existing `CommandRecorder`/`RecordPackets` recording thread so all Vulkan driver calls, descriptor commits, bindless writes, and dynamic state filtering run asynchronously off the GPU execution thread.
  2. **Dynamic multi-worker `VK_COMMAND_BUFFER_LEVEL_SECONDARY` fan-out (`v32`)**: Partition the `RecordPackets` stream at render-pass / barrier / sync boundaries and dispatch chunks across `1â€“4` dynamically scaled P-core workers (Section 3), each with its own `VkCommandPool` and thread-local `DescriptorHeap` / descriptor-buffer slice, reading lock-free cache snapshots.
  3. **Ordered Primary Stitch**: Primary execution/submit thread stitches completed secondary CBs in exact guest order via `vkCmdExecuteCommands()`.

### Pillar 4 â€” True Async Compute Queue + GPU `<= 9.5 ms` Floor (`~27 ms -> <= 9.5 ms`)
- **Files**: `core/kyty/graphics/presentation/window/vulkanWindow.cpp`, `core/kyty/graphics/host_gpu/renderer/commandScheduler.{h,cpp}`, `core/kyty/graphics/shader/recompiler/backend/spirv/spirvEmitter*.cpp`
- **Plan**:
  1. **Real `VK_QUEUE_COMPUTE_BIT` async compute queue (`v33`)**: Request a dedicated compute queue family in `vulkanWindow.cpp` (with automatic single-queue fallback if unavailable) and route PS5 async-compute passes (cloth, particles, culling, BVH/RT prep) onto it using timeline-semaphore synchronization to overlap with graphics rasterization.
  2. **Targeted SPIR-V optimization (`v34`)**: Using `GPUTIME_TOP` (`KYTY_TRACE_GPU_TIME=1`), optimize the top 20â€“25 heaviest GPU shaders: wave32 lowering where beneficial on NVIDIA/RDNA GPUs, fast-math/NaN-check elision where safe, redundant workgroup/memory barrier removal, and tighter `VK_KHR_ray_query` traversal/payload codegen.
  3. **Resolution & upscaler scaling**: Pair the optimized GPU pipeline with the selectable resolution profiles (`720p`â€“`4K` + optional host FSR/NIS upscaling) so every GPU tier can dial into `<= 9.5 ms` GPU time with full visual stability.

### Pillar 5 â€” Eliminate `VirtualProtect` TLB Shootdowns & Percentage-Based Memory Hygiene (`~4.4 ms -> <= 0.5 ms`)
- **Files**: `core/kyty/graphics/host_gpu/pageManager.{h,cpp}`, `core/libs/prx/libSceAgcDriver/KytyShim/src/MemoryGpu.cpp`, `core/libs/prx/libSceAgcDriver/KytyShim/src/MemoryBacking.cpp`, `core/kyty/graphics/host_gpu/renderer/cache/textureCache.cpp`
- **Plan**:
  1. **Eliminate hot-ring `VirtualProtect` cycles (`v33`)**: Replace ~600/f `VirtualProtect` calls (multi-core TLB shootdowns + `270 ms/5 s` `protect` lock contention) on hot ring buffers with Windows `MEM_WRITE_WATCH` / `GetWriteWatch` or software dirty-epoch tracking.
  2. **Percentage-based VRAM & RAM budget enforcement (`v35`)**: Replace fixed GiB thresholds with live `VK_EXT_memory_budget` (`85%` soft-evict / `92%` aggressive GC) and `GlobalMemoryStatusEx` (`80%` soft cap) policies so 30+ minute sessions stay completely free of WDDM eviction cliffs on 8 GB, 12 GB, 16 GB, or 24 GB+ GPUs.

## 5. Phased Execution Checklist (`v28a`â€“`v35`)

Every phase requires: (1) backup of working `kyty_gpu.prx`, (2) env-var A/B toggle for risky changes, (3) 4-scene benchmark verification, and (4) visual regression check (shadows, cloth, RT, cutscenes).

### Phase 0 â€” Uncap, Multi-Res Profiles & True GPU Floor (`v28a` | Gate: Verified Uncap + Exact GPU ms Baseline)
- [ ] **`v28a-1`**: Verify `ANYPS5_VBLANK_HZ=240` + `KYTY_PRESENT_MODE=mailbox` and confirm whether *Ghost of YÅtei* unlocks past 60 FPS cleanly (add toggleable frame-cap unlock if game-capped).
- [ ] **`v28a-2`**: Run `KYTY_TRACE_GPU_TIME=2` and `=1` across all 4 scenes to record true GPU ms, CB gaps, and top-25 `GPUTIME_TOP` shaders.
- [ ] **`v28a-3`**: Audit `submit other=5,582/5 s` callers and top `invalidate` / `fault_game` guest address ranges; confirm multi-resolution profile switching (`720p`â€“`4K`).

### Milestone A â€” Fast CPU/Cache Wins & Recording Thread (`v28b`â€“`v29` | Gate: `32â€“40 FPS` / `25â€“31 ms` med)
- [ ] **`v28b`**: 4-way set-associative static `g_obtain_memo` & `g_write_memo` (`bufferCache.cpp`) + fix per-frame `depth_overlap`/`overlap` image unregister thrash (`textureCache.cpp`/`image.cpp`).
- [ ] **`v29a`**: Port `Celegans12` SRT walker stack (`71585e5a`, `38f78397`, `15f8686b`) + benchmark & harden `CommandRecorder`/`RecordPackets` thread + dynamic CPU topology detector (`GetLogicalProcessorInformationEx`).
- [ ] **`v29b`**: Capture fresh 15-min `yotei-v5.profdata` PGO profile across all 4 scenes.

### Milestone B â€” Zero-Stall Coherence & Descriptor Overhaul (`v30`â€“`v31` | Gate: `55â€“70 FPS` / `14â€“18 ms` med, `<= 22 ms` p95)
- [ ] **`v30`**: DirectRanges expansion for hot `invalidate` rings + submit coalescing (`~80 -> <= 8/f`, target `2â€“4/f`) + dedicated `VK_QUEUE_TRANSFER_BIT` queue + shadow-page CPU write coherence.
- [ ] **`v31`**: `VK_EXT_descriptor_buffer` (with automatic fallback) + pass-level epoch validation.

### Milestone C â€” Parallel Recording, Async Compute & 100+ FPS Lock (`v32`â€“`v35` | Gate: `<= 9.5 ms` med, `<= 11.5 ms` p95 across all 4 scenes)
- [ ] **`v32`**: Dynamic multi-core `VK_COMMAND_BUFFER_LEVEL_SECONDARY` fan-out built on `RecordPackets` (scaling across detected P-cores with `ReserveCores`).
- [ ] **`v33`**: Real `VK_QUEUE_COMPUTE_BIT` async compute queue + `MEM_WRITE_WATCH` / `GetWriteWatch` or software dirty-epoch tracking (eliminating hot `VirtualProtect` TLB shootdowns).
- [ ] **`v34`**: `GPUTIME_TOP`-guided SPIR-V shader optimization (wave32 lowering, barrier elision, RT query tightening) + multi-resolution / upscaler tuning verification.
- [ ] **`v35`**: Dynamic percentage-based VRAM (`<= 85%` `heapBudget`) & RAM (`<= 80%`) budget hardening, 30-min 4-scene soak test, and final 15-min `yotei-v6.profdata` PGO retrain.

## 6. Hard Guardrails & Known Traps
1. Never bloat `RenderExecutor` with large inline arrays/vectors (`v24` L1d regression); keep caches static/out-of-line.
2. Keep `ShaderVertexInputBuffer` & `ShaderVertexDestination` initialized (`KytyPS5@99202b5d` caused cutscene character freeze).
3. Never pin helper/recording threads to P-cores without excluding them via `ReserveCores` (`v21`), and keep parallel workers lock-free on `BufferCache` (`v18`).
4. Never hardcode CPU core counts, fixed VRAM/RAM GiB caps, or a single internal resolution â€” always derive from live host topology, `VK_EXT_memory_budget` / `GlobalMemoryStatusEx` percentages, and user resolution profiles.
5. Never overwrite `build\`, `*-backup*`, `app.exe.*`, or existing `*.prx.bak`; always back up `kyty_gpu.prx` before deploy; on every push update `README.md` + this file, never commit `*.kyhints`, never stage `3rdparty/ZArchive`.

## 7. Version Progress Log (Update After Every Benchmarked Build)

| Version | Commit | Med ms (FPS) | p10 ms (FPS) | p90 / p95 ms | Key Delta |
| :--- | :--- | ---: | ---: | ---: | :--- |
| `v23b` | `3de11d9156` | `56.7 ms` (`17.6`) | `40.0 ms` (`25.0`) | `79.3 ms` (p90) | 3-layer shader compile fix (zero 53 s stalls) |
| `v26` | `e258524937` | `49.2 ms` (`20.3`) | `38.9 ms` (`25.7`) | `63.9 ms` (p90) | `256 KiB` read window (`-92.8%` dl), dirty-only `ImageHeap`, upstream KytyPS5/Celegans12 |
| `v27` | `c175b705ad` | **`47.0 ms` (`21.3`)** | **`39.1 ms` (`25.6`)** | **`60.8 ms`** (p90) | 6 `KytyPS5-GTA` GPU opts (`+4.7%` med FPS, `fence 6.8 ms`, `dl 5.9 ms`) |
| `v28a` | *pending* | â€” | â€” | â€” | Uncap (`VBLANK_HZ=240` + Mailbox) + `KYTY_TRACE_GPU_TIME=2/1` baseline + `submit other`/`invalidate` audit |
| `v28b` | *pending* | â€” | â€” | â€” | 4-way `obtain_memo`/`write_memo` + fix per-frame `depth_overlap`/`overlap` image thrashing |
