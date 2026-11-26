# AnyKyty-Yotei-RT

**Windows AnyPS5 + Erlou KytyPS5-Yotei GPU backend port with native hardware Ray Tracing (`VK_KHR_ray_query`) and Big-O CPU/GPU optimizations for *Ghost of Yōtei* (`PPSA26344`).**

> **Source Code Only / Legal Notice:** This repository contains **source code only** — no compiled binaries, shader caches, game files, firmware, cryptographic keys, or copyrighted Sony/Sucker Punch material. You must supply your own legally obtained dump of the game. This project is not affiliated with or endorsed by Sony Interactive Entertainment or Sucker Punch Productions.

## Overview & Key Changes

This fork pairs [AnyPS5](https://github.com/boykopovar/AnyPS5) / [AnyKyty](https://github.com/simnJS/AnyPS5) (relinker, `libkernel`, system libraries, input, audio, `libSceAgc`) with the [Erlou93/KytyPS5-Yotei](https://github.com/Erlou93/KytyPS5-Yotei) Vulkan GPU backend (`core/kyty/` + `core/libs/prx/libSceAgcDriver/KytyShim/`) built as a ThinLTO + PGO `kyty_gpu.prx` DLL, plus:

- **Native Hardware Ray Tracing (`VK_KHR_ray_query`)** with BVH `MappedPrefix` handling.
- **GPU Indirect Thread-Dimension Dispatch** (`core/kyty/graphics/host_gpu/shaders/dispatch_thread_dims.comp`): offloads `COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS` indirect compute dispatches to the GPU, eliminating `fault_gpu` stalls (from ~2,200–2,470 ms/5 s down to **0 ms**).
- **Big-O CPU & Memory Optimizations**:
  - 64-page hierarchical backed bitmap in `MemoryBacking.cpp` (`O(P/64)` `IsBacked()`).
  - `O(1)` open-addressed epoch hash index in `RenderExecutor::PlanFlags()` and removal of hot `thread_local` scratch vectors.
  - Precomputed `fusable_read` slots and memo-first `SrtTapeWalker::Eval()` in `SrtTape.cpp`.
  - 2-level chunked B-tree `RangeSet` (`BlockCap=64`) with `O(1)` bounding-box fast-reject.
  - `O(1)` hash indexing and bounding-box pruning in `BufferCache`.

For full engineering history, benchmark logs, and next steps, see [`ERLOU_PORT_HANDOFF.md`](ERLOU_PORT_HANDOFF.md).

## Current Status & Benchmarks

> **Performance Target:** **100+ FPS (`<= 10.0 ms/frame`)** (intermediate gate: 60 FPS) in *Ghost of Yōtei* (`PPSA26344`) — full architectural roadmap tracked in [`ROADMAP_100FPS.md`](ROADMAP_100FPS.md).

Tested on **Windows 11**, **Intel Core Ultra 7 270K Plus**, **32 GB RAM**, **NVIDIA GeForce RTX 5070 (12 GB VRAM)** with *Ghost of Yōtei* (`PPSA26344`, v2.002):

| Build / Scenario | Resolution | Measured FPS | Notes |
| :--- | :--- | :--- | :--- |
| Village (Big-O v1 + PGO v2) | 1080p | **23.26 FPS** avg (26–32 FPS steady) | 43.0 ms/f avg |
| Open World (`native-rt-v6`) | 900p (`1600×900`) | **19.4–25.3 FPS** | Stable across 9,518 frames |
| Heavy Post-Fight Cutscene (`native-rt-v6`) | 900p (`1600×900`) | **8.1–11.1 FPS** | `fault_gpu = 0 ms` (up from 3.3–3.6 FPS at 1080p) |
| Heavy Post-Fight Cutscene / Open World (`native-rt-v9`) | 900p (`1600×900`) | **15.2–18.8 FPS** open world (53–66 ms/f) | `v8` shutdown fix + `v9` async graphics variants + `.kyhints` import |
| Open World / Combat (`native-rt-v12-pgo-v3`) | 900p (`1600×900`) | **21.8 FPS** avg (**29.1 FPS** steady / 13.4 FPS heavy) | `v10`–`v12` Big-O/pruning + fresh 19-snapshot `yotei-v3.profdata` PGO (`74.7 ms/f` heavy vs `80.9 ms/f` in `v9`) |
| Open World / Combat (`native-rt-v13-parallel-v1`) | 900p (`1600×900`) | **23.9 FPS** avg (**26.4 FPS** steady, only 13.7% heavy) | `BarDisableMbps=100` (deterministic GPU DMA copies on Windows WDDM, GPU ~70%), 4-thread `BarCopyPool`, 256-bit `m_pending_page_bloom` (`download 10.9 ms/f`) |
| Open World / Combat (`native-rt-v14-compute-peekahead`) | 900p (`1600×900`) | **23.5 FPS** avg (**25.2 FPS** steady across 73.1% of frames / 13.4 FPS heavy) | Off-thread `PeekComputeAhead` on Resolve Helpers (`92.5%` of all GPU tasks planned off-thread; `164.9M` bindings answered in `O(1)`, `disp_op -36.0%`, 9,017 frames) |
| Open World / Combat (`native-rt-v17-pgo3`) | 900p (`1600×900`) | **23.8 FPS** steady 3D (**42.01 ms/f** across 78.0% of `draws>=1000` frames; **19.3 FPS** avg incl. heavy spikes) | `v15` `O(1)` compute pipeline & bindless table caches + throttled `SaveIfDue()`; `v16` pre-flush `PrefetchEagerReadbacks()` & batched direct barrier (`download -31.5%`, `fence -20.0%`); `v17` single-lock `O(1)` `m_commands` batch-drain (`commands -22.6%`, `idle -13.8%`, `PIPELINE_TIME=0`, 4,840 3D frames) |
| Open World / Combat (`native-rt-v20-global-epoch-everywhere`) | 900p (`1600×900`) | **23.8 FPS** steady 3D (**42.06 ms/f** across 73.6% of automated open-world `draws>=1000` frames; GPU **61–64%**) | `v19` `O(1)` `g_image_page_global_epoch` guard in `PeekProofFail`/`ObtainMemo` + 64-slot `NativeUpload` dedup cache (`<=256 B`); `v20` extends `O(1)` global epoch guard to `TableSet`, `ImageReplaySlot`, `BindlessTable::TableSlotInfo`, & `TargetReplaySlot` (`disp_op -10.1%`, `draw_op -5.4%`, `rebind_buf -5.9%`, `fence -13.7%` vs `v17`, `PIPELINE_TIME=0`) |
| Open World / Combat (`native-rt-v21-wddm-pure-vram`) | 900p (`1600×900`) | **24.0 FPS** steady 3D (**41.75 ms/f** across 73.6% of `draws>=1000` frames) | Windows WDDM pure `DEVICE_LOCAL` VRAM for guest buffers `<256 MiB` (skips persistently-mapped BAR alloc unless `KYTY_BAR_READS=1`; `idle -17.7%`, zero startup BAR probe stalls, `PIPELINE_TIME=0`) |
| Open World / Combat (`native-rt-v23b-3layer-zero-inval`) | 900p (`1600×900`) | **17.08 µs/draw** across 4,770 heavy 3D frames (**3,454 draws/f**, vs `18.95 µs/draw` in `v21b`) | 3-layer compute shader compile optimization: RT precompile-mismatch fix, longest-first compute precompile sort + 32-thread cap, opt-in `KYTY_PIPELINE_BINARY_CACHE=1` (`VK_KHR_pipeline_binary` 3-step zero-invalidation cache), opt-in `KYTY_SPIRV_DONTINLINE_LARGE=1` — zero 53 s stalls, zero `SHADER_PRECOMPILE_MISMATCH` |
| Cutscene + Open World (`native-rt-v26-readwindow-imageheap`) | 900p (`1600×900`) | **20.3 FPS** median (**49.2 ms/f**, `p10 = 38.9 ms` / **25.7 FPS**, `p90 = 63.9 ms`, **17.1 FPS** avg across 2,045 frames at 2,900 med draws/f) | Upstream `KytyPS5` perf ports (`02063de5` timeline refresh skip, `61331721` 4 KiB stack `memcmp` color metadata, `c2c34344` division-free tiling) + `Celegans12` `InsertTickRange` `std::map` node recycling + tuned `256 KiB` `DEVICE_LOCAL` readback window (`KYTY_READ_WINDOW_KB`, PCIe download volume `-92.8%`: `2.27 GB -> 163 MB / 5 s`) + dirty-pages-only `ImageHeap::Refresh()` (cutscene `fence` `42.8 -> 4.4 ms`, `download` `39.2 -> 4.6 ms`, `PerfReason::Other` `1,008 -> 4 ms / 5 s`) |
| Open World (`native-rt-v27-kytyps5-gta-merge`) | 900p (`1600×900`) | **21.32 FPS** median (**46.90 ms/f**, **20.43 FPS** steady mean, `p90 = 59.40 ms`, **16.88 µs/draw** across 1,732 steady 3D frames at 2,901 draws/f) | Ported 6 GPU optimizations from `simnJS/KytyPS5-GTA` (`635169a` `AttachmentKeepsShaderRead` depth-stencil barrier skip, `cad73ff` grow-only `m_descriptor_images`, `f573e6e` 2-operand `std::min`, `f3c9d4f` 256-slot same-tick `m_recent` write-tick skip, `bd356cc` DRS 3D volume depth expansion, opt-in `4c4eaac` `KYTY76_LDS_SYNC=1`) — `idle -17.5%`, `download -13.1%`, `fence -12.4%`, `commands -9.3%`, `µs/draw -5.1%` vs `v26` |

| Open World (`v34a`–`v34c`, VRAM Solved) | 900p (`1600×900`) | **19–25 FPS** (`40.1–52.4 ms/f`) | `VK_EXT_memory_priority` (1.0/0.5) + dedicated VMA `device_buffer_pool` (`-71%` frag) + adaptive BCn mip-cap (`-3.51 GiB` VRAM → **68.6%**, `0.0 ms` device allocs/frees, zero WDDM freezes) |
| **Open World (`v35`–`v40b`, current HEAD)** | 900p (`1600×900`) | **25.5 FPS** steady (`39.2 ms/f`, `p95=44.3 ms`), **16.6 FPS** heavy Present (`60.3 ms/f`, **8.58–8.76 µs/cmd**, **6.84 µs/cmd** active) | 5-phase CPU-wall stack (`v35` widened rings + priority, `v36` `AsyncHostCopy`+`VirtualProtect` worker `-97%` `protect_batch`, `v37b` `FastVerify` page cache + BDA/SRT `PlanBuffer` `89.3%` hits, `v38b` `TableSet` tick cache + cross-draw `FastVerify`, `v39` fresh 6-min `yotei-v5.profdata` PGO, `v40b` gated `Commit::OverlapScan` `-55%` `commit`, `-79%` `slot_wait`/`resolved_full`, `-22%` µs/cmd vs `v35`) |

> **v34a–v40b Highlights:** Eliminates both VRAM WDDM paging freezes and major CPU pipeline bottlenecks on 12 GB GPUs: (1) `VK_EXT_memory_priority` (`1.0`/`0.5`), dedicated VMA `device_buffer_pool` (`-71%` fragmentation), adaptive read-only BC1–BC7 mip-cap (`KYTY_TEXTURE_MIP_CAP=auto`, `-3.51 GiB` VRAM → `68.6%`), and expanded image recycler (`32→256`, `0.0 ms` allocs/frees); (2) `v35`–`v40b` CPU-wall stack — widened `GpuPipeline` rings (`256→2048`), dedicated `AsyncHostCopy` + `VirtualProtect` worker synced via `copy_epoch` (`protect_batch.commit_ms 161→4 ms/5s`), `FastVerify` 64-entry cross-draw backing-page cache + `ResolveHelper` BDA/SRT `PlanBuffer` (`99.6%` adoption, `89.3%` buffer hits, `helper_writes=0`), `TableSet` tick-scoped epoch cache, fresh 6-min PGO (`yotei-v5.profdata`, hash mismatches `31+→1`), and gated `Commit::OverlapScan` (`commit -38% to -55%`). Total normalized command cost dropped **-22%** (`11.23 → 8.58–8.76 µs/cmd`, breaking `< 7.0 µs/cmd` active at `6.84 µs/cmd`), with `slot_wait` and `resolved_full` down **-79%**.

> **v27-kytyps5-gta-merge Highlights:** Merges 6 audited GPU optimizations from `simnJS/KytyPS5-GTA` while keeping `AnyKyty`'s superior RT (`rayTracingSlots.cpp`), meta-clear (`renderCompute.cpp`), and `DrawRepeat`/`O(1)` global-epoch implementations: (1) `AttachmentKeepsShaderRead` (`635169a`) in `image.cpp` to eliminate redundant depth-stencil barriers and render-pass breaks; (2) grow-only `m_descriptor_images` (`cad73ff`) and inline 2-operand `std::min` (`f573e6e`) in `descriptors.cpp`; (3) 256-slot direct-mapped `m_recent_write_ticks`/`m_recent_write_frames` fast-skip (`f3c9d4f`) combined with `v26`'s node-recycling `InsertTickRange()` in `bufferCache.cpp`; (4) 3D volume depth expansion across DRS width changes (`bd356cc`) in `textureCache.cpp`; and (5) opt-in `KYTY76_LDS_SYNC=1` (`4c4eaac`, default off) in `spirvEmitterMemory.cpp` for RTX 30/40 wave64 LDS ordering. Across 1,732 steady 3D frames (`2,901` draws/f): **21.32 FPS median (`46.90 ms/f`, +4.7% vs `v26`)**, **20.43 FPS steady mean (`48.96 ms/f`, +3.6%)**, **`p90 = 59.40 ms`** (`-4.7%`), and **`16.88 µs/draw`** (`-5.1%`), with `idle -17.5%`, `download -13.1%`, `fence -12.4%`, and `commands -9.3%`.

> **v26-readwindow-imageheap Highlights:** Combines four upstream GPU hot-path wins (`KytyPS5@02063de5` conditional timeline semaphore refresh in `PopPendingOperations()`, `KytyPS5@61331721` zero-alloc 4 KiB `memcmp` color metadata expansion, `KytyPS5@c2c34344` division-free tiled texture layout math, and `Celegans12` `std::map` node recycling in `BufferCache::InsertTickRange()`) with two Windows WDDM `DEVICE_LOCAL` stall fixes: (1) shrinks `BufferCache::ReadRange()`'s non-BAR staging readback window from `4 MiB` to **`256 KiB`** (tunable via `KYTY_READ_WINDOW_KB`), slashing GPU->CPU readback volume **-92.8%** (`2.27 GB -> 163 MB / 5 s`); and (2) restricts `ImageHeap::Refresh()` to CPU-dirty and GPU-dirty pages instead of syncing the entire bindless heap table, cutting cutscene `PerfReason::Other` stalls **-99.6%** (`1,008 -> 4 ms / 5 s`). Across 2,045 3D frames (`2,900` med draws/f): **20.33 FPS median (`49.2 ms/f`)**, `p10 = 38.9 ms` (**25.7 FPS**), `p90 = 63.9 ms` (down from `107.3 ms` in `v25b` and `79.3 ms` in `v23b`), with cutscene frames jumping from `9.8 FPS` (`102 ms`, `fence 42.8 ms`, `dl 39.2 ms`) to **`18.5 FPS` (`54.2 ms`, `fence 4.4 ms`, `dl 4.6 ms`)**.

> **v23b-3layer-zero-inval Highlights:** Eliminates live 53 s NVIDIA compute shader compile stalls without invalidating existing warm `PPSA26344.bin` caches: (1) fixes `SHADER_PRECOMPILE_MISMATCH` for RT shaders in `shaderPrecompile.cpp`, sorts compute precompile jobs longest-first by guest `code.size()`, and raises the `KYTY_SHADER_PRECOMPILE_THREADS` cap from 16 to 32; (2) adds opt-in **`KYTY_PIPELINE_BINARY_CACHE=1`** (`VK_KHR_pipeline_binary`, auto-disabled on unsupported GPUs/drivers) using a 3-step zero-invalidation probe (`_PipelineCache/binaries/*.vkbin` -> warm `VkPipelineCache` probe via `eFailOnPipelineCompileRequired` -> cold compile with `eCaptureDataKHR` + atomic `.vkbin` save for `>=200 ms` shaders); and (3) adds opt-in **`KYTY_SPIRV_DONTINLINE_LARGE=1`** (`DontInline` on raw loads & Trinity BVH helpers for `>8,000`-instruction shaders). Verified across 4,770 heavy 3D open-world frames (`3,454` draws/f): `17.08 µs/draw` (`-9.9%` vs `v21b`'s `18.95 µs/draw`), zero 53 s stalls, zero `SHADER_PRECOMPILE_MISMATCH`, and `1` saved `.vkbin` (`2.1 MB`, `3 ms` save overhead).

> **v21-wddm-pure-vram Highlights:** On Windows WDDM (`KYTY_PLATFORM_WINDOWS`), skips persistently-mapped `DEVICE_LOCAL | HOST_VISIBLE` Resizable BAR allocation for guest `DeviceLocal` buffers `<256 MiB` unless `KYTY_BAR_READS=1`, routing 100% of guest buffers into pure `DEVICE_LOCAL` VRAM. Cuts steady `idle` by **-17.7%** (`6.83 -> 5.62 ms/f`) and lifts steady 3D frame time to **41.75 ms/f (24.0 FPS)** across 73.6% of `draws>=1000` frames with zero startup BAR probe stalls and `PIPELINE_TIME=0`.

> **v19–v20 Highlights:** Adds a single global `DrawRepeat::g_image_page_global_epoch` `O(1)` fast-path guard across `PeekProofFail`, `ObtainMemo`, `TableSet`, `ImageReplaySlot`, `BindlessTable::TableSlotInfo`, and `TargetReplaySlot` (replacing repeated `O(size / 1 MiB)` `g_image_page_epochs[]` scans with one L1-cached atomic load whenever no image page changed), plus a 64-slot per-tick `NativeUpload` content-dedup cache (`<=256 B`, exact CPU-RAM `memcmp`, `StreamBuffer::WrapEpoch()` guarded). On the same automated 3D open-world spawn spot (`1600×900`, `draws>=1000`, `PIPELINE_TIME=0`), steady 3D frame time drops `43.62 -> 42.06 ms/f` (**23.8 FPS** across 73.6% of frames), cutting `disp_op` **-10.1%**, `draw_op` **-5.4%**, `rebind_buf` **-5.9%**, and `fence` **-13.7%** while lifting RTX 5070 utilization to **61–64%** (`84–87.6 W`).

> **v17-pgo3 Highlights:** Combines `v15` (`SaveIfDue()` clock throttle, 256-slot `m_compute_fast`, 64-slot `m_table_fast`, 1-field bindless fast path), `v16` (pre-flush `PrefetchEagerReadbacks()` + `<=64 KiB` hot-read tracking + batched direct-chunk `vkCmdPipelineBarrier`), and `v17` (`O(1)` `std::deque::swap` batch-drain of `m_commands` under one lock), compiled with `yotei-v3.profdata` PGO. Across 4,840 3D open-world frames (`draws>=1000`), steady 3D frame time drops `43.37 -> 42.01 ms/f` (**23.8 FPS** across 78.0% of 3D frames), cutting `download` **-31.5%** (`9.93 -> 6.80 ms`), `fence` **-20.0%** (`9.77 -> 7.82 ms`), `commands` **-22.6%** (`10.42 -> 8.07 ms`), and `idle` **-13.8%** (`6.53 -> 5.63 ms`) with `PIPELINE_TIME=0`.

> **v14-compute-peekahead Highlights:** Extends Resolve Helper `PlanBuffer` pre-resolution to compute dispatches (`DispatchDirect` & `DispatchIndirect`), lifting off-thread plan coverage from `44.5%` → **`92.5%`** (`bad=0`, `helper_writes=0`), cutting heavy `disp_op` by **-36.0%** (`34.80 → 22.28 ms/f`), heavy `draw_op` by **-23.5%**, and keeping **73.1%** of frames in the steady 20–50 ms band while feeding the RTX 5070 at **~70%** utilization. Builds on `v13`'s `BarDisableMbps=100` GPU DMA copies, 4-thread `BarCopyPool`, 256-bit `m_pending_page_bloom`, and `B`/`V` Swing/Hammer motion controls.

## Requirements

- **OS:** Windows 10 (1803+) or Windows 11, 64-bit.
- **CPU:** x86-64 with AVX2 support.
- **GPU:** Vulkan 1.3 capable GPU with current drivers (`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, and `VK_KHR_deferred_host_operations` required for native RT). Developed and tested on NVIDIA RTX; AMD/Intel GPUs are untested.
- **RAM & Page File:** **32 GB RAM recommended** plus a **32–64 GB Windows page file** (the title allocates ~13.5 GiB of direct memory; a small automatic page file can hit Windows commit-limit errors).
- **Disk Space:** Several GB free for shader and pipeline caches (`_PipelineCache`).

## Setup & Building from Source (Windows)

### 1. Clone with Submodules
```powershell
git clone --recursive https://github.com/akshay-kamloo/AnyKyty-Yotei-RT.git
cd AnyKyty-Yotei-RT
```

### 2. Toolchain Prerequisites
The provided PowerShell build scripts expect portable toolchains under `_tools\` (or edit the paths at the top of `build-erlou-dll.ps1` / `build-erlou.ps1` to match your system installations):
- **CMake 4.x + Ninja**: `_tools\cmake\cmake-4.5.1-windows-x86_64\bin\`
- **MinGW-w64 GCC 15.x**: `_tools\winlibs\mingw64\bin\` (for AnyPS5 host/PRX libraries)
- **llvm-mingw (Clang + `libc++` + `lld` + `llvm-profdata`)**: `_tools\llvm-mingw\bin\` (for the `kyty_gpu.prx` DLL build)

### 3. Build `kyty_gpu.prx` + System PRX Libraries (Recommended: ThinLTO DLL)

> **Note:** Inherited upstream CI workflows (.github/workflows/) and Dependabot are removed on this fork; build locally on Windows using the scripts below.
```powershell
# Standard Release + ThinLTO build (no PGO)
.\build-erlou-dll.ps1 -Reconfigure

# Optional: PGO Generation build (collects .profraw while playing)
.\build-erlou-dll.ps1 -Reconfigure -PgoMode GEN

# Optional: PGO Use build (after merging .profraw -> .profdata with llvm-profdata)
.\build-erlou-dll.ps1 -Reconfigure -PgoMode USE -PgoProfile "D:\path\to\yotei.profdata"
```

Non-DLL fallback build (links Kyty statically into `libSceAgcDriver.prx`):
```powershell
.\build-erlou.ps1 -Reconfigure
```

### 4. Deploy to Your Game Run Directory
Copy the built libraries from `build-erlou-dll\core\libs\prx\...` into your relinked game folder's `libs\` directory (alongside your legally dumped & relinked `app.exe` and `app0\` data):
- `libs\kyty_gpu.prx`
- `libs\libSceAgcDriver.prx`
- `libs\libkernel.prx` (and updated PRX targets from `libs`)

## Running on Different Settings & Resolutions

### Recommended Presets by GPU VRAM
- **12 GB VRAM GPUs (e.g., RTX 5070):** Run at **900p (`1600×900`)** with DRS locked and `+48 MiB` RTGI pool headroom to avoid VRAM spillover during cutscenes and dense open-world areas, or **1080p (`1920×1080`)** with `+32 MiB` RTGI pool in lighter areas.
- **16 GB+ VRAM GPUs (e.g., RTX 5070 Ti / 4080 / 4090 / 5080):** **1080p (`1920×1080`)** or **1440p (`2560×1440`)** works comfortably without hitting the Vulkan budget ceiling as quickly.
- **In-Game Graphics Mode:** Use the game's **Performance** mode. Quality mode aggressively changes internal resolution and can cause flicker unless resolution/DRS is locked.

### Clean Play Launch (Zero Tracing Overhead)
Run from your game directory in PowerShell:
```powershell
$env:APS5_OUTPUT_RESOLUTION = "1600x900"   # or "1920x1080", "2560x1440"
$env:ANYPS5_MOUSE_TOUCHPAD  = "1"          # enables mouse touchpad swipe support

# Ensure diagnostic/trace and one-time precompile flags are off
foreach ($v in @(
  'KYTY_TRACE_FRAMETIME','KYTY_TRACE_SLOW_FRAMES','KYTY_TRACE_PERF',
  'KYTY_TRACE_GPU_TIME','KYTY_TRACE_GPU_READ_WAITS','KYTY_TRACE_CALLER_WAITS',
  'KYTY_VRAM_BUDGET_MB','APS5_PROFILE_DRAW','APS5_LOG_TIMESTAMPS',
  'APS5_TRACE_AUDIOOUT2','APS5_TRACE_AJM',
  'KYTY_IMPORT_HINTS','KYTY_EXPORT_HINTS','KYTY_PRECOMPILE_ONLY','KYTY_PRECOMPILE_THREADS'
)) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }

.\app.exe
```

### Upfront Shader Precompilation (`.kyhints`)
To avoid first-visit shader compilation stutters when entering new areas, you can precompile known shaders/pipelines into `_PipelineCache` once before playing.

> **Note:** `.kyhints` files are **not** bundled in this repository. Download the matching `v02.002.000` `.kyhints` file (e.g., `PPSA26344.kyhints`) from **[Erlou93/KytyPS5-Yotei Releases](https://github.com/Erlou93/KytyPS5-Yotei/releases)** and place it in your game run directory (next to `app.exe`).

1. **Run the one-time headless precompile** (populates `_PipelineCache` and exits automatically):
```powershell
$env:KYTY_IMPORT_HINTS       = "PPSA26344.kyhints"
$env:KYTY_PRECOMPILE_ONLY    = "1"
$env:KYTY_PRECOMPILE_THREADS = "12"   # tune to your CPU core count
.\app.exe
```

2. **Launch normally afterward:** Open a fresh PowerShell window (or clear `KYTY_IMPORT_HINTS`, `KYTY_PRECOMPILE_ONLY`, and `KYTY_PRECOMPILE_THREADS` as shown in *Clean Play Launch* above) and start `.\app.exe` (or `.\run-play.ps1`).

### Diagnostic / Benchmarking Launch
Enable frame-time and GPU-time telemetry when measuring changes:
```powershell
$env:APS5_OUTPUT_RESOLUTION = "1600x900"
$env:KYTY_TRACE_FRAMETIME   = "1"
$env:KYTY_TRACE_PERF        = "1"
$env:KYTY_TRACE_GPU_TIME    = "1"
.\app.exe
```

### Controls, Input & Hotkeys
- **Performance Overlay:** Press **`F2`** to toggle the performance panel; **`Shift+F2`** shows detailed breakdown (frame time, CPU/GPU waits, draw calls, shader compiles, and **Vulkan VRAM budget**).
- **Touch Pad ("Swipe up" prompts):** A DualSense controller's touchpad works natively. With keyboard + mouse (when `ANYPS5_MOUSE_TOUCHPAD=1`), press **`F6`**, move the mouse to swipe, and press **`F6`** again to exit touchpad mode.
- **Motion Controls (Keyboard):** Hold **`X`** for side-to-side shake (`Shake`), **`B`** to lift/pitch up (`SwingUp`), and **`V`** to swing/strike down (`SwingDown` — e.g., forging/hammering prompts).
- **Keyboard & Controller Mapping:** SDL-mapped controllers work out of the box; keyboard/mouse binds can be customized via `anyps5-input.ini` (see [`docs/user/INPUT_MAPPING.md`](docs/user/INPUT_MAPPING.md)).

## Important Environment Variables & Toggles

Code defaults live in `core/kyty/common/envDefaults.h`. Any variable set in your shell overrides the default (`=0` disables it). Setting `KYTY_ENV_DEFAULTS=0` disables all built-in defaults for strict testing.

### Display, Frame Pacing & Upscaling
| Variable | Default | Description |
| :--- | :--- | :--- |
| `APS5_OUTPUT_RESOLUTION` | *(auto)* | Output window/presentation resolution, e.g. `1600x900`, `1920x1080`, `2560x1440`. |
| `ANYPS5_VBLANK_HZ` | `60` | Overrides virtual VBlank refresh rate (`30`–`360`; e.g. `240` lifts 60 FPS vblank caps). |
| `KYTY_VBLANK_FREQUENCY` | *(auto)* | Kyty-side VBlank frequency override. |
| `KYTY_PRESENT_MODE` | *(auto)* | Overrides Vulkan swapchain present mode. |
| `ANYPS5_UPSCALE` | *(off)* | Enables host-side presentation upscaling (e.g. FSR). |
| `ANYPS5_FSR_SHARPNESS` | *(default)* | Sharpness value when FSR upscaling is enabled. |
| `ANYPS5_MOUSE_TOUCHPAD` | `0` | Set to `1` to enable mouse-driven PS5 touchpad emulation (`F6`). |
| `KYTY_BORDERLESS` | `0` | Set to `1` for borderless window mode. |

### Performance, GPU & Ray Tracing
| Variable | Default | Description |
| :--- | :--- | :--- |
| `KYTY_NATIVE_RT` | `1` *(auto)* | Native Vulkan `VK_KHR_ray_query` hardware RT (`0` = emulate all RT, `1` = require native RT). |
| `KYTY_RT_ARENA_MB` | `512`–`4096` | Overrides acceleration-structure VRAM arena limit (default: up to 1/4 of VRAM). |
| `KYTY_RT_ORACLE` | `0` | Bitmask for RT oracle options (bit `8` enables pixel-shader native BVH walks). |
| `KYTY_RT_STATS` | `0` | Set to `1` to print a `[RT]` summary every 10 seconds. |
| `KYTY_SHADER_PRECOMPILE` | `1` | Precompiles known shaders on startup to reduce in-game stutter. |
| `KYTY_PIPELINE_BINARY_CACHE` | `0` | Opt-in `VK_KHR_pipeline_binary` atomic per-pipeline disk cache (`_PipelineCache/binaries/*.vkbin`) for slow (`>=200 ms`) compute shaders. Uses a 3-step zero-invalidation probe and auto-disables cleanly on drivers/GPUs without `VK_KHR_pipeline_binary`. |
| `KYTY_SPIRV_DONTINLINE_LARGE` | `0` | Opt-in `DontInline` guard on indirect raw loads and Trinity BVH helpers for huge (`>8,000`-instruction) shaders to shrink cold NVIDIA compile times (off by default to preserve existing warm `PPSA26344.bin` hashes). |
| `KYTY76_LDS_SYNC` | `0` | Opt-in subgroup workgroup-memory barrier before LDS loads/stores in wave64-halves compute shaders (`KytyPS5-GTA@4c4eaac`, fixes Ziva torn meshes on RTX 30/40; off by default to preserve RTX 50 perf and warm `PPSA26344.bin` hashes). |
| `KYTY_SHADER_PRECOMPILE_THREADS` | `8` | Worker thread count for shader precompilation (tune to your CPU core count). |
| `KYTY_ASYNC_GRAPHICS_VARIANTS` | `1` | Offloads first-time non-writing graphics shader variant compiles to background workers so new areas don't freeze (set `0` for sync). |
| `KYTY_IMPORT_HINTS` | *(unset)* | Path to a `.kyhints` file (e.g., from Erlou's release) to import shader records against `app0` before precompiling. |
| `KYTY_EXPORT_HINTS` | *(unset)* | Path to export your local shader journals as a shareable `.kyhints` file (requires `KYTY_PRECOMPILE_ONLY=1`). |
| `KYTY_PRECOMPILE_ONLY` | `0` | Set to `1` to run headless upfront shader/pipeline precompilation, save `_PipelineCache`, and exit without starting the game. |
| `KYTY_PRECOMPILE_THREADS` | `0` | Thread count for `KYTY_PRECOMPILE_ONLY=1` (`0` uses `KYTY_SHADER_PRECOMPILE_THREADS`; max `64`). |
| `KYTY_GAME_DIR` | `./app0` | Game directory scanned during `KYTY_IMPORT_HINTS` (auto-detects `./app0` if unset). |
| `KYTY_SHADER_PRECOMPILE_WAIT_SEC` | `180` | Max seconds to wait on startup precompile during a normal launch (when `KYTY_SHADER_PRECOMPILE_WAIT=1`). |
| `KYTY_GPU_THREAD_CPUS` | *(auto)* | Pins the 4 GPU pipeline threads to separate physical CPU cores (`off` disables pinning). |
| `KYTY_BINDLESS` | `1` | Enables bindless texture heap translation (required for terrain/foliage materials). |
| `KYTY_DRAW_TEMPLATES` | `2` | Draw template caching mode (AnyKyty default `2`). |
| `KYTY_DRAW_BUFFERS` | `1` | Grouped draw buffers optimization. |
| `KYTY_VRAM_BUDGET` | `1` | Enables Vulkan VRAM budget tracking and eviction. |
| `KYTY_VRAM_BUDGET_MB` | *(auto)* | Manually caps VRAM budget in MiB (e.g. `10240` on 12 GB cards if needed). |
| `KYTY_VRAM_GC_FIX` | `1` | Enables VRAM garbage-collection fix for long sessions. |
| `KYTY_PERF_BDA_PROMOTE_KB` | `2048` | Window size (KiB) promoted on a BDA fallback miss (`0` disables window promotion). |
| `KYTY_WRITE_BACK_IMAGES` | `1` | Copies GPU-written images back at GL2 write-backs (`0` restores stock KytyPS5 behaviour). |

### Compatibility & Workarounds (`envDefaults.h`)
| Variable | Default | Description |
| :--- | :--- | :--- |
| `KYTY_ENV_DEFAULTS` | `1` | Master switch for `envDefaults.h`. Set `=0` to disable all built-in Yōtei defaults. |
| `KYTY_PIPELINE_CACHE_ANY_REVISION` | `1` | Reuses pipeline caches across minor build revisions. |
| `KYTY_FIX_TEXTURE_CLAMP_LAST_LEVEL` | `1` | Clamps texture mip sampling to last valid level. |
| `KYTY_FIX_RT_11_11_10_FLOAT` | `1` | Float `R11G11B10` render target handling. |
| `KYTY_WORKAROUND_ALIAS_SYNC` | `1` | Synchronizes aliased image memory (`D4/D5`). |
| `KYTY_WORKAROUND_STENCIL_PLANE_SYNC` | `2` | Stencil plane synchronization workaround (`D8`). |
| `KYTY_WORKAROUND_DCC_FAST_CLEAR` | `1` | DCC fast-clear handling (`A5`). |
| `KYTY_WORKAROUND_WATERFALL_POINTER_RECORD0` | `1` | Terrain shader waterfall pointer workaround (`M42`). |
| `KYTY_SHADER_LOOP_LIMIT` | `65536` | Global shader loop watchdog preventing GPU hangs. |

### Diagnostics & Profiling (Keep Unset for Gameplay)
| Variable | Description |
| :--- | :--- |
| `KYTY_TRACE_FRAMETIME=1` | Logs per-frame CPU/GPU timings. |
| `KYTY_TRACE_PERF=1` | Prints 5-second periodic stage breakdown (`draw_op`, `disp_op`, `fence`, `fault_gpu`, etc.). |
| `KYTY_TRACE_GPU_TIME=1` | Enables per-shader GPU timer telemetry. |
| `KYTY_TRACE_SLOW_FRAMES=1` | Logs detailed breakdown for frames exceeding threshold. |
| `KYTY_TRACE_CALLER_WAITS=1` | Traces CPU caller-wait / BAR readback stalls. |
| `APS5_TRACE_AUDIOOUT2=1` | Logs `libSceAudioOut` (`AudioOut2`) port creation, channel formats, attributes, and mix stats. |
| `APS5_TRACE_AJM=1` | Logs `libSceAjm` audio decoder jobs (AT9/Opus/AAC/MP3/PCM). |
| `KYTY_PGO_WRITE_FRAME=<N>` | Flushes PGO `.profraw` data at frame `N` in `-PgoMode GEN` builds. |

## Known Issues & Troubleshooting

- **First launch is slow / stutters:** Cold starts compile shaders and populate `_PipelineCache`. Subsequent launches reuse the cache and load much faster. For upfront precompilation, see **[Upfront Shader Precompilation (`.kyhints`)](#upfront-shader-precompilation-kyhints)** above (download `.kyhints` from [Erlou93/KytyPS5-Yotei Releases](https://github.com/Erlou93/KytyPS5-Yotei/releases) — not included in this repo).
- **Checkered or pixelated shadows after a stutter/cutscene:** Draws whose pipelines are still compiling are skipped for a few frames, which can temporarily corrupt temporal shadow/lighting buffers. **Workaround:** Quit to the main menu and reload your save (no full restart needed).
- **VRAM grows over long sessions:** Watch **Vulkan budget** in `Shift+F2`. If it approaches your GPU's VRAM limit and FPS drops, save, quit, and restart.
- **`Insufficient system resources` on boot:** Increase your Windows page file to **32–64 GB** so Windows can commit the game's 13.5 GiB direct memory allocation.
- **Missing character voices in cutscenes (under investigation):** Try switching the in-game **Audio Settings** output mode to **Headphones** or **TV Speakers (Stereo)**. When reporting or debugging audio issues, run with `$env:APS5_TRACE_AUDIOOUT2 = "1"` to capture audio port/channel logs.

## Upstream Projects & Credits

This repository builds on the work of the upstream PS5 emulation community:
- **[boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5)** & **[Ghost of Yōtei Bring-Up Discussion #675](https://github.com/boykopovar/AnyPS5/discussions/675)**
- **[simnJS/AnyPS5 (`AnyKyty` branch)](https://github.com/simnJS/AnyPS5)**
- **[Erlou93/KytyPS5-Yotei](https://github.com/Erlou93/KytyPS5-Yotei)** ([`README-YOTEI.md`](https://github.com/Erlou93/KytyPS5-Yotei/blob/main/README-YOTEI.md) and KytyPS5 Yōtei GPU backend)
- **[Celegans12/AnyPS5-Ghost-of-Yotei (`yotei/best`)](https://github.com/Celegans12/AnyPS5-Ghost-of-Yotei/tree/yotei/best)**
- **Original Kyty** by InoriRus (MIT License, notice preserved in `core/kyty/LICENSES/Kyty-MIT.txt`)

## License

Licensed under the **GNU General Public License v2.0 only (`GPL-2.0-only`)** — see [`LICENSE`](LICENSE). Third-party components in `3rdparty/` and submodules remain under their respective licenses.
