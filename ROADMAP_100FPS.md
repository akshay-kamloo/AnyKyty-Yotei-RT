# AnyKyty-Yotei-RT — 100+ FPS Architectural Optimization Plan (`<= 10.0 ms/frame`)

*Living engineering roadmap — read, refer to, and update across sessions.*
*Created: 2026-11-24 | Last updated: 2026-11-25 | Branch: `AnyKyty-Yotei-Performance` (`D:\Games\AnyKyty-Perf`) | Current Build: `v34c` (`5cf39b30f8`)*

## 1. North-Star Target, Acceptance Gates & Hardware-Agnostic Design

| Metric | Intermediate Gate (60 FPS) | Final Target (100+ FPS) |
| :--- | ---: | ---: |
| **Median Frame Time (4 Scenes)** | `<= 16.0 ms` (`>= 62.5 FPS`) | **`<= 9.5 ms` (`>= 105 FPS`)** |
| **p95 / 1% Low Frame Time** | `<= 20.0 ms` (`>= 50 FPS`) | **`<= 11.5 ms` (`>= 87 FPS`, zero `> 20 ms` hitches)** |
| **CPU Critical Path** | `<= 15.5 ms` | **`<= 9.0 ms`** |
| **True GPU Time (`KYTY_TRACE_GPU_TIME=2`)** | `<= 15.5 ms` | **`<= 9.5 ms`** |
| **Sustained VRAM (30-min soak)** | `<= 88%` of `heapBudget` | **`<= 85%` of `heapBudget`** *(Achieved in `v34c`: **`68.6%`**)* |
| **Sustained Host RAM** | `<= 85%` of physical RAM | **`<= 80%` of physical RAM** |

### Hardware-Agnostic Rules (Mandatory Across All Phases)
1. **Dynamic CPU Topology**: Detect P-cores vs E-cores via Windows `GetLogicalProcessorInformationEx` (`RelationProcessorCore` + `EfficiencyClass`); never hardcode core counts or indices. Reserve pinned host cores via `Thread::ReserveCores` (`threads.cpp:488`) while leaving `>= 50%` of P-cores unreserved for guest game threads.
2. **Percentage-Based VRAM & RAM**: Always derive caps from live `VK_EXT_memory_budget` and `GlobalMemoryStatusEx` (`VK_EXT_memory_priority` 1.0/0.5 + dedicated VMA `device_buffer_pool` + adaptive `KYTY_TEXTURE_MIP_CAP=auto`).
3. **Multi-Resolution Profiles**: Selectable via `set-res-profile.ps1` (`720p`, `810p`, `900p`, `1080p`, `1440p`, `4K`); consistent benchmark profile is **`900p` (`1600x900`)**.
4. **Strict Stage-Gating**: Ship **1 phase per build** (`v35 -> v36 -> v37 -> v38 -> v39`); gate each on `diag-*-v*.log` (`us/draw`, `p50`, `p95`, `idle`, `claim`, `resolved_full`, `slot_wait`, `helper_writes=0`).

## 2. Completed Foundation (`v28a`–`v34c`): Sync, Coherence & VRAM Solved

- **VRAM solved (`v34a`–`v34c`)**: Live VRAM dropped from `9,768–11,731 MiB` (`90–107%` WDDM paging) to **`7,670–7,715 MiB` (`68.6%`, `3.51 GiB` saved by adaptive BCn mip-cap)**; VMA fragmentation cut **`-71%`** (`1,199 -> 346 MiB`); `vkAllocateMemory`/`vkFreeMemory` stalls dropped **`55.7 ms -> 0.0 ms`** (`allocs=0, frees=0`).
- **Sync & readbacks solved (`v31a`–`v33a`)**: `direct=0 ms` (`-100%`), `ring_dead -90%`, `download_to_backing -90%`, `buffer_lookup=0 ms`, `texture_cache -59%`, `writing_draws_waited -97%`.
- **Live `v34c` CPU Wall (`900p`, `-GpuTimeMode 3`, Present World `5,577` ops/f)**:
  - `draw_op=13.3 ms` + `draw_ind=2.9 ms` + `disp_op=10.8 ms` = **`27.0 ms/f`** single-threaded CPU recording wall.
  - Backpressure: `resolved_full=705,067 / 3,779 ms` (**75.6%** of every 5 s window!), `slot_wait=2,948 ms`, `idle=1,178–1,541 ms`, `execution_idle=632–1,164 ms`.
  - Recording thread: `RECORD_THREAD wait_ms=5.4–9.7 ms / 5 s` (`15.3%` busy) — confirms Secondary CBs are NOT needed until Phases 1–3 drop Execution Thread `< 15 ms/f`.
## 3. Single-Threaded CPU Wall Execution Plan (`v35`–`v39`)

| Phase | Build | Live Target in `v34c` (`900p`) | Core Change (Hardware-Agnostic) | Est. Savings | Effort |
| :--- | :--- | :--- | :--- | ---: | :--- |
| **Phase 1: Kill Ping-Pong & Preemption** | **`v35`** | `idle` (`4.3–7.4 ms/f`, `1,178–1,541 ms/5s`), `resolved_full` (`3,779 ms/5s`), `slot_wait` (`2,948 ms/5s`) | Widen `GpuPipeline` resolved ring (`graphicsRun.cpp:1560`) so bursts never stall the PM4 producer; dynamically detect P-cores vs E-cores (`GetLogicalProcessorInformationEx`) and reserve dedicated P-cores via `Thread::ReserveCores` (`threads.cpp:488`, excluded from guest) for Execution Thread + PM4 Front-End + 2 ResolveHelpers with raised host priority. | **`-5 to -8 ms/f`** | 1–2 days |
| **Phase 2: Async Host-Copy & `VirtualProtect` Thread** | **`v36`** | `reads` (`3.0 ms/f`) + `protect` (`1.6–1.8 ms/f`) = **`4.6–4.8 ms/f`** | Offload guest-to-staging `memcpy` uploads and Windows `VirtualProtect` TLB-shootdown re-arming onto a dedicated host copy thread (pattern from `KytyPS5-GTA@d8680c3`, `c3139ef`, `030f2fd`), synced via an atomic copy epoch before `vkQueueSubmit`. | **`-3 to -4 ms/f`** | 3–5 days |
| **Phase 3: Parallel Read-Only Prepare on `ResolveHelper` (Step 1A + 1B + 1C)** | **`v37`** | `rebind_buf` (`6.3`) + `rebind_img` (`1.5`) + `bindless` (`3.2`) + `bda` (`1.8`) + `commit` (`3.0`) = **`15.8 ms/f`** | Reuse existing `v30c`/`v31c`/`v33a` RW-spinlocks (`m_lookup_rw`, `TextureCache::LockPeekRead`, shared-lock `TableReplayStage`) inside `ResolveTaskInto()` (`renderDraw.cpp`) across 2–4 helpers to pre-resolve read-only `FindTextureReplay` views, clean BDA `indirect_ranges`, and flat `VkDescriptorBufferInfo`/`VkDescriptorImageInfo` arrays — validated at Execution Thread time by `g_image_page_global_epoch` + CPU-read write-watch checks (AnyPS5 `#7895`). | **`-8 to -11 ms/f`** | 1–2 weeks |
| **Phase 4: Delete Per-Draw Binding Overhead (`VK_EXT_descriptor_buffer` + Pass-Level Epochs)** | **`v38`** | Remaining `commit` + `rebind_*` + `commands` (`2.7–4.3 ms/f`) | Validate `BufferCache`/`TextureCache` epochs once per render-pass/barrier boundary instead of `~5,000x/f`, and replace `vkUpdateDescriptorSets` with direct memory-mapped `VK_EXT_descriptor_buffer` writes (automatic `DescriptorHeap` fallback). | **`-4 to -6 ms/f`** | 1–2 weeks |
| **Phase 5: Fresh PGO (`yotei-v4`) + Conditional Parallel `RecordThread` / Async Compute** | **`v39`** | Full stack (`v15`–`v38` codegen) + `disp_op` (`10.8–12.7 ms/f`) | Rebuild with fresh PGO (`yotei-v4.profdata`) over `v38` (+4–8% CPU). Only add `VK_COMMAND_BUFFER_LEVEL_SECONDARY` recording if `RECORD_THREAD wait_ms` climbs `> 100 ms/5 s` (only `5.4–9.7 ms/5 s` today), and evaluate a dedicated `VK_QUEUE_COMPUTE_BIT` queue for async compute passes (`0x20`/`0x28`/`0x30`/`0x38`). | **`-3 to -6 ms/f`** | 1–2 weeks |

### Codebase Mapping & Guardrails (`v35`–`v39`)
- **Dynamic Thread Topology** (example on 8P+16E Core Ultra 7 270K Plus; auto-scales on any CPU):
  - **Reserved P-cores (`ReserveCores`)**: `1` P-core Execution Thread, `1` P-core PM4 Front-End / `RecordThread`, `2` P-cores `ResolveHelper` (bump to `3–4` only if `claim < 0.8 ms` in Phase 3).
  - **Guest Game Threads**: Remaining `>= 50%` of P-cores (4 P-cores on 8P+16E) so game simulation/render-submit threads never starve `GpuPipeline`.
  - **E-cores / Overflow**: Async Host-Copy / `VirtualProtect` (`v36`), `BarCopyPool`, `TableCompileWorkers`, shader precompile.
- **Strict Phase 3 Correctness**: Helpers stay 100% read-only (`helper_writes=0`); `AppendScope`, `UploadBatchScope`, and `InvalidateMemoryFromGPU` stay in guest order on Execution Thread; adoption validity checks only guard CPU-consumed reads (keeping adoption survival `>= 85%`, AnyPS5 `#7895`).
- **No Early Secondary CBs**: Skip Pillar 3 Secondary CBs until Phases 1–3 cut Execution Thread `< 15 ms/f` and `RECORD_THREAD wait_ms > 100 ms/5 s`.

## 4. Hard Guardrails
1. Never bloat `RenderExecutor` inline (`v24` L1d regression); keep `ShaderVertexInputBuffer`/`ShaderVertexDestination` default-initialized.
2. Never pin host threads to P-cores without excluding them via `ReserveCores` (`v21`); keep parallel workers lock-free on `BufferCache`.
3. Never hardcode CPU core counts, fixed VRAM/RAM GiB caps, or a single internal resolution.
4. Never touch `D:\Games\AnyKyty\run-PPSA26344`, `build\`, `*-backup*`, `app.exe.*`, or existing `*.prx.bak`; always back up `kyty_gpu.prx` before deploy; update `README.md` + this file on every GitHub push; never commit `*.kyhints`; never stage `3rdparty/ZArchive`.

## 5. Version Progress Log

| Version | Commit | Profile | Med ms (FPS) | Key Delta |
| :--- | :--- | :--- | ---: | :--- |
| `v23b` | `3de11d9156` | `900p` | `56.7 ms` (`17.6`) | 3-layer shader compile fix |
| `v27` | `c175b705ad` | `900p` | `47.0 ms` (`21.3`) | 6 `KytyPS5-GTA` GPU opts |
| `v31b` | `0cf7c6a656` | `900p` | `44.9 ms` (`22.3`) | Dynamic compile workers, `O(1)` `DirectRangeQuiet`, `TableSet` cap `16->64` |
| `v31c` | `4eccc2c9ea` | `900p` | `44.9 ms` (`22.3`) | RW-spinlock `TextureCache` (`-59%` GPU lock wait) + bindless fast-reject |
| `v32a` | `33d266ac56` | `900p` | `40.9 ms` (`24.5`) | Skip CP-only `WaitRegMem` `GpuDrain::DirectWrites` (`878 -> 0 ms`) |
| `v32b` | `fb4ca50737` | `900p` | `41.7 ms` (`24.0`) | Coalesce 64 dead-ring pages (`ring_dead -90%`, `invalidate -75%`) |
| `v32d` | `6e6c1e07ab` | `900p` | `41.1 ms` (`24.3`) | Eager `invalidate` readback prefetch (`download_to_backing -90%`, `wait_cur -90%`) |
| `v32e` | `c82a29a324` | `720p/900p/1440p` | `39.4 / 44.9 / 51.2 ms` | Eager-aware `KYTY_SLICE_COALESCE=1` + async visual draw pipelines (`writing_draws_waited -97%`) |
| `v33a` | `3d3222a74c` | `900p` | `37.5 ms` Past / `45 ms` Present | Shared-lock `TableReplayStage`/`TableTexturesReady` |
| `v34a` | `f2e318dda1` | `900p` | `40.1–42.1 ms` Present (`24–25 FPS`) | `VK_EXT_memory_priority` (1.0/0.5) + dedicated VMA `device_buffer_pool` (`-739 MiB` frag, `-9 ms/f` GPU) |
| `v34c` | `5cf39b30f8` | `900p` | `41.6–52.4 ms` Present (`19–24 FPS`) | Adaptive BCn mip-cap (`-3.51 GiB` VRAM -> **`68.6%`**) + Recycler `32->256` (`0.0 ms` allocs/frees) |
| `v35` | *next* | `900p` | *target: `-5 to -8 ms/f`* | **Phase 1**: Widen `GpuPipeline` resolved ring + dynamic P-core `ReserveCores` topology |