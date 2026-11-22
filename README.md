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

Tested on **Windows 11**, **Intel Core Ultra 7 270K Plus**, **32 GB RAM**, **NVIDIA GeForce RTX 5070 (12 GB VRAM)** with *Ghost of Yōtei* (`PPSA26344`, v2.002):

| Build / Scenario | Resolution | Measured FPS | Notes |
| :--- | :--- | :--- | :--- |
| Village (Big-O v1 + PGO v2) | 1080p | **23.26 FPS** avg (26–32 FPS steady) | 43.0 ms/f avg |
| Open World (`native-rt-v6`) | 900p (`1600×900`) | **19.4–25.3 FPS** | Stable across 9,518 frames |
| Heavy Post-Fight Cutscene (`native-rt-v6`) | 900p (`1600×900`) | **8.1–11.1 FPS** | `fault_gpu = 0 ms` (up from 3.3–3.6 FPS at 1080p) |
| Heavy Post-Fight Cutscene / Open World (`native-rt-v9`, current HEAD) | 900p (`1600×900`) | **15.2–18.8 FPS** open world (53–66 ms/f) | `v8` shutdown fix + `v9` async graphics variants + `.kyhints` import |

> **v9 Highlights:** Includes the `v8`/`v9` clean-exit fixes (eliminating `graphicsRun.cpp:670` and `renderContext.cpp:94`), **async graphics shader variant compilation** (`KYTY_ASYNC_GRAPHICS_VARIANTS=1` by default), and **`.kyhints` upfront shader precompilation** (`KYTY_IMPORT_HINTS` / `KYTY_PRECOMPILE_ONLY=1`).

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

# Ensure diagnostic/trace overhead is off
foreach ($v in @(
  'KYTY_TRACE_FRAMETIME','KYTY_TRACE_SLOW_FRAMES','KYTY_TRACE_PERF',
  'KYTY_TRACE_GPU_TIME','KYTY_TRACE_GPU_READ_WAITS','KYTY_TRACE_CALLER_WAITS',
  'KYTY_VRAM_BUDGET_MB','APS5_PROFILE_DRAW','APS5_LOG_TIMESTAMPS'
)) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }

.\app.exe
```

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
| `KYTY_PGO_WRITE_FRAME=<N>` | Flushes PGO `.profraw` data at frame `N` in `-PgoMode GEN` builds. |

## Known Issues & Troubleshooting

- **First launch is slow / stutters:** Cold starts compile shaders and populate `_PipelineCache`. Subsequent launches reuse the cache and load much faster.
- **Checkered or pixelated shadows after a stutter/cutscene:** Draws whose pipelines are still compiling are skipped for a few frames, which can temporarily corrupt temporal shadow/lighting buffers. **Workaround:** Quit to the main menu and reload your save (no full restart needed).
- **VRAM grows over long sessions:** Watch **Vulkan budget** in `Shift+F2`. If it approaches your GPU's VRAM limit and FPS drops, save, quit, and restart.
- **`Insufficient system resources` on boot:** Increase your Windows page file to **32–64 GB** so Windows can commit the game's 13.5 GiB direct memory allocation.
- **Upfront `.kyhints` precompilation (zero first-visit stutter):** Drop a matching `v02.002.000` `.kyhints` file into your game folder and run once with `='PPSA26344.kyhints'; ='1'; ='12'; .\app.exe` to precompile all known shaders/pipelines into `_PipelineCache` before playing.

## Upstream Projects & Credits

This repository builds on the work of the upstream PS5 emulation community:
- **[boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5)** & **[Ghost of Yōtei Bring-Up Discussion #675](https://github.com/boykopovar/AnyPS5/discussions/675)**
- **[simnJS/AnyPS5 (`AnyKyty` branch)](https://github.com/simnJS/AnyPS5)**
- **[Erlou93/KytyPS5-Yotei](https://github.com/Erlou93/KytyPS5-Yotei)** ([`README-YOTEI.md`](https://github.com/Erlou93/KytyPS5-Yotei/blob/main/README-YOTEI.md) and KytyPS5 Yōtei GPU backend)
- **[Celegans12/AnyPS5-Ghost-of-Yotei (`yotei/best`)](https://github.com/Celegans12/AnyPS5-Ghost-of-Yotei/tree/yotei/best)**
- **Original Kyty** by InoriRus (MIT License, notice preserved in `core/kyty/LICENSES/Kyty-MIT.txt`)

## License

Licensed under the **GNU General Public License v2.0 only (`GPL-2.0-only`)** — see [`LICENSE`](LICENSE). Third-party components in `3rdparty/` and submodules remain under their respective licenses.
