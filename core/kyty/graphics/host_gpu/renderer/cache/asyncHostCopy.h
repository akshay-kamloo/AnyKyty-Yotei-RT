#pragma once

#include <cstdint>

namespace Libs::Graphics::AsyncHostCopy {

// Phase 2 (v36): Async Host-Copy & VirtualProtect Thread.
// Kill-switch: KYTY_ASYNC_HOST_COPY=0 (default ON).
[[nodiscard]] bool     Enabled() noexcept;
[[nodiscard]] uint64_t PublishedEpoch() noexcept;
void                   WaitForEpoch(uint64_t epoch) noexcept;
void                   Drain() noexcept;

} // namespace Libs::Graphics::AsyncHostCopy