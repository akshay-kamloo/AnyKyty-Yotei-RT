#include "prx/libSceAgcDriver/KytyShim/src/Runtime.hpp"
#include "prx/libSceAgcDriver/KytyShim/src/NativeRt.hpp"
#include "prx/libSceAgcDriver/KytyShim/src/ShaderCachePolicy.hpp"
#include "prx/libSceVideoOut/include/VblankSchedule.hpp"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <magic_enum.hpp>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace KytyShim {
namespace {

using Options = Config::ConfigOptions;

std::atomic<NativeRt::Mode> nativeRtMode{NativeRt::Mode::Auto};

struct Flag {
    const char* variable;
    bool Options::* field;
};

constexpr Flag Flags[] = {
    {"KYTY_HDR", &Options::hdr_enabled},
    {"KYTY_STRETCH", &Options::stretch_enabled},
    {"KYTY_TRINITY", &Options::pro_enabled},
    {"KYTY_VULKAN_VALIDATION", &Options::vulkan_validation_enabled},
    {"KYTY_GPU_ASSISTED_VALIDATION", &Options::gpu_assisted_validation_enabled},
    {"KYTY_SHADER_VALIDATION", &Options::shader_validation_enabled},
    {"KYTY_SHADER_CACHE", &Options::shader_cache_enabled},
    {"KYTY_TESSELLATION", &Options::tessellation_enabled},
    {"KYTY_OCCLUSION_QUERIES", &Options::occlusion_queries_enabled},
    {"KYTY_COMMAND_BUFFER_DUMP", &Options::command_buffer_dump_enabled},
    {"KYTY_GRAPHICS_DEBUG_DUMP", &Options::graphics_debug_dump_enabled},
    {"KYTY_PROFILE", &Options::profiler_enabled},
    {"KYTY_SPIRV_DEBUG_PRINTF", &Options::spirv_debug_printf_enabled},
    {"KYTY_READBACK_LINEAR_IMAGES", &Options::readback_linear_images},
    {"KYTY_READBACK_TILED_IMAGES", &Options::readback_tiled_images},
    {"KYTY_SYNC_RAW_IMAGE_BUFFERS", &Options::sync_raw_image_buffers},
    {"KYTY_PERF_OVERLAY", &Options::perf_overlay_enabled},
    {"KYTY_RT_STATS", &Options::rt_stats_enabled},
};

struct Number {
    const char* variable;
    std::uint32_t Options::* field;
    std::uint32_t minimum;
    std::uint32_t maximum;
};

constexpr Number Numbers[] = {
    {"KYTY_VBLANK_FREQUENCY", &Options::vblank_frequency, 0, 0xffffffffu},
    {"KYTY_RT_CHECK", &Options::rt_check_period, 0, 0xffffffffu},
    {"KYTY_RT_CHECK_PPM", &Options::rt_check_ppm, 0, 1000000u},
    {"KYTY_RT_CHECK_STRIKES", &Options::rt_check_strikes, 1, 1000u},
};

struct Path {
    const char* variable;
    std::filesystem::path Options::* field;
};

constexpr Path Paths[] = {
    {"KYTY_SHADER_LOG_FOLDER", &Options::shader_log_folder},
    {"KYTY_COMMAND_BUFFER_DUMP_FOLDER", &Options::command_buffer_dump_folder},
    {"KYTY_PRINTF_OUTPUT_FILE", &Options::printf_output_file},
};

const char* Read(const char* variable, std::string& applied) {
    const char* value = std::getenv(variable);
    if (value == nullptr || *value == '\0') return nullptr;
    applied += applied.empty() ? "" : " ";
    applied += variable;
    applied += '=';
    applied += value;
    return value;
}

template <typename Integer>
bool ParseInteger(std::string_view text, Integer& out) {
    Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return false;
    out = value;
    return true;
}

template <typename Enum>
void ReadEnum(const char* variable, Enum& out, std::string& applied) {
    const char* value = Read(variable, applied);
    if (value == nullptr) return;
    const auto parsed = magic_enum::enum_cast<Enum>(std::string_view(value), magic_enum::case_insensitive);
    if (!parsed.has_value()) {
        std::string expected;
        for (const auto name : magic_enum::enum_names<Enum>()) {
            expected += expected.empty() ? "" : ", ";
            expected += name;
        }
        EXIT("KytyShim: %s=%s is not one of %s\n", variable, value, expected.c_str());
    }
    out = *parsed;
}

Options OptionsFromEnvironment(std::string& applied) {
    Options options;
    options.screen_width = 1920;
    options.screen_height = 1080;
    options.present_mode = Config::PresentMode::Mailbox;
    options.tessellation_enabled = true;
    options.readback_linear_images = true;
    options.shader_optimization_type = Config::ShaderOptimizationType::Performance;
    for (const auto& flag : Flags) {
        const char* value = Read(flag.variable, applied);
        if (value == nullptr) continue;
        const std::string_view text(value);
        if (text != "0" && text != "1") EXIT("KytyShim: %s=%s is neither 0 nor 1\n", flag.variable, value);
        options.*flag.field = text == "1";
    }
    options.shader_cache_enabled = ResolveShaderCacheEnabled(options.shader_cache_enabled, Read("ANYPS5_NO_SHADER_CACHE", applied));
    for (const auto& number : Numbers) {
        const char* value = Read(number.variable, applied);
        if (value == nullptr) continue;
        std::uint32_t parsed = 0;
        if (!ParseInteger(value, parsed) || parsed < number.minimum || parsed > number.maximum) {
            EXIT("KytyShim: %s=%s is not a whole number from %u to %u\n", number.variable, value, number.minimum, number.maximum);
        }
        options.*number.field = parsed;
    }
    try {
        if (const auto hertz = VideoOutVblank::ConfiguredVblankHz()) options.vblank_frequency = *hertz;
    } catch (const std::runtime_error& error) {
        EXIT("KytyShim: %s\n", error.what());
    }
    for (const auto& path : Paths) {
        if (const char* value = Read(path.variable, applied)) options.*path.field = std::filesystem::path(value);
    }
    if (const char* value = Read("KYTY_GPU", applied); value != nullptr && !ParseInteger(value, options.gpu_index)) {
        EXIT("KytyShim: KYTY_GPU=%s is not a GPU index\n", value);
    }
    ReadEnum("KYTY_PRESENT_MODE", options.present_mode, applied);
    ReadEnum("KYTY_SHADER_LOG_DIRECTION", options.shader_log_direction, applied);
    ReadEnum("KYTY_PRINTF_DIRECTION", options.printf_direction, applied);
    const char* nativeRt = Read("KYTY_NATIVE_RT", applied);
    const auto mode = NativeRt::ParseMode(nativeRt);
    if (!mode.has_value()) EXIT("KytyShim: KYTY_NATIVE_RT=%s is neither 0 nor 1\n", nativeRt);
    options.native_rt = *mode != NativeRt::Mode::Off;
    nativeRtMode.store(*mode, std::memory_order_relaxed);
    const char* oracle = Read("KYTY_RT_ORACLE", applied);
    const auto oracleMode = NativeRt::ParseOracle(oracle);
    if (!oracleMode.has_value()) EXIT("KytyShim: KYTY_RT_ORACLE=%s is not a whole number from 0 to 15\n", oracle);
    options.rt_oracle = *oracleMode;
    if (const char* results = Read("KYTY_RT_RESULTS", applied)) {
        const std::string_view text(results);
        if (text != "0" && text != "1") EXIT("KytyShim: KYTY_RT_RESULTS=%s is neither 0 nor 1\n", results);
        if (text == "1" && (options.rt_oracle & 7u) != 0) options.rt_oracle |= 32u;
    }
    if (options.gpu_assisted_validation_enabled) options.vulkan_validation_enabled = true;
    return options;
}

}

void StartRuntime() {
    static std::once_flag started;
    std::call_once(started, [] {
        std::string applied;
        const auto options = OptionsFromEnvironment(applied);
        Config::Initialize();
        Config::Load(options);
        Log::Initialize();
        std::fprintf(stderr, "[kyty-erlou] runtime started, settings: %s\n", applied.empty() ? "Erlou Yotei defaults" : applied.c_str());
    });
}

void ReportNativeRayTracing(const Libs::Graphics::GraphicContext& graphics) {
    std::fprintf(stderr, "[kyty-erlou] Erlou KytyPS5-Yotei GPU backend active (native_rt=%s, ray_query=%s, rt_oracle=%u)\n",
                 graphics.native_ray_tracing_enabled ? "ON" : "OFF",
                 graphics.ray_query_enabled ? "ON" : "OFF",
                 Config::RayTracingOracle());
}

void RouteInterrupts(Libs::Graphics::RenderContext& renderer) {
    renderer.AddInterruptEq(InterruptRoute, GraphicsInterruptEvent);
    for (int event = FirstComputeInterruptEvent; event < FirstComputeInterruptEvent + ComputeInterruptEvents; ++event) {
        renderer.AddInterruptEq(InterruptRoute, event);
    }
}

}
