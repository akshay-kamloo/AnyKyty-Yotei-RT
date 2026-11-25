#include "kernel/memory.h"

#include "MemoryShim.hpp"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/perfCounters.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <cinttypes>
#include <cstring>

namespace Libs::LibKernel::Memory {

static Graphics::RenderContext* g_gpu_resources = nullptr;

static Graphics::RenderContext& GetGpuResources() {
	if (g_gpu_resources == nullptr) Shim::Fail("KytyShim memory: a GPU memory call came before InstallGpuResources\n");
	return *g_gpu_resources;
}

static bool IsGpuAddressRange(uint64_t vaddr, uint64_t size) {
	return Graphics::GuestRange {vaddr, size}.Valid();
}

static void MapGpuRange(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) return;
	g_gpu_resources->MapMemory(vaddr, size);
}

static void UnmapGpuRange(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) return;
	g_gpu_resources->UnmapMemory(vaddr, size);
}

static bool OrdinaryReadHookImpl(uint64_t vaddr, void* data, uint64_t size) {
	if (g_gpu_resources == nullptr || !Graphics::GuestGpu::IsGpuThread()) return false;
	bool cp_overlay = false;
	if (IsGpuAddressRange(vaddr, size)) {
		auto& buffers = g_gpu_resources->GetBufferCache();
		if (buffers.HasGpuDirtyBytes(vaddr, size)) {
			Graphics::PerfReasonScope reason(Graphics::PerfReason::SrtWalker);
			buffers.ReadMemory(vaddr, size);
		}
		cp_overlay = buffers.WaitPendingReadbacks(vaddr, size);
	}
	if (!TryReadBacking(vaddr, data, size)) return false;
	if (cp_overlay) {
		g_gpu_resources->GetBufferCache().ApplyDirectCpWrites(vaddr, data, size);
	}
	return true;
}

static Graphics::ShaderRecompiler::IR::AheadReadStatus AheadReadHookImpl(uint64_t vaddr, void* data, uint64_t size) {
	using Status = Graphics::ShaderRecompiler::IR::AheadReadStatus;
	if (g_gpu_resources == nullptr) return Status::Backing;
	if (IsGpuAddressRange(vaddr, size)) {
		auto& buffers = g_gpu_resources->GetBufferCache();
		if (buffers.HasGpuDirtyBytesShared(vaddr, size)) return Status::GpuDirty;
	}
	return TryReadBacking(vaddr, data, size) ? Status::Ok : Status::Backing;
}

static Graphics::ShaderRecompiler::IR::FastVerifyResult FastVerifyHookImpl(
    const Graphics::ShaderRecompiler::IR::ReadLog& log, size_t begin, size_t& stop,
    Graphics::ShaderRecompiler::IR::FastAdoptStats& stats) {
	using Result = Graphics::ShaderRecompiler::IR::FastVerifyResult;
	if (g_gpu_resources == nullptr || !Graphics::GuestGpu::IsGpuThread()) {
		stop = begin;
		return Result::NeedsReader;
	}
	auto& buffers = g_gpu_resources->GetBufferCache();
	static const bool use_page_cache = [] {
		const char* v = std::getenv("KYTY_FAST_VERIFY_CACHE");
		return !(v != nullptr && v[0] == '0' && v[1] == '\0');
	}();
	struct CleanPageSlot {
		uint64_t       page    = ~uint64_t {0};
		const uint8_t* backing = nullptr;
		uint64_t       gen     = 0;
	};
	thread_local CleanPageSlot t_cache[64] {};
	thread_local uint64_t      t_cache_gen       = 1;
	thread_local uint64_t      t_last_ranges_gen = ~uint64_t {0};
	thread_local uint64_t      t_last_gpu_clock  = ~uint64_t {0};
	const uint64_t cur_ranges_gen = RangesGeneration();
	const uint64_t cur_gpu_clock  = Graphics::g_gpu_dirty_clock.load(std::memory_order_acquire);
	const bool     direct_empty   = buffers.DirectRangeQuiet(0, 0);
	if (!direct_empty || cur_ranges_gen != t_last_ranges_gen || cur_gpu_clock != t_last_gpu_clock) {
		++t_cache_gen;
		if (t_cache_gen == 0) {
			for (auto& s: t_cache) s.gen = 0;
			t_cache_gen = 1;
		}
		t_last_ranges_gen = cur_ranges_gen;
		t_last_gpu_clock  = cur_gpu_clock;
	}
	const uint64_t active_gen = t_cache_gen;
	const uint8_t* log_bytes  = log.bytes.data();
	for (size_t i = begin; i < log.entries.size(); ++i) {
		const auto& entry = log.entries[i];
		if (!entry.ok || entry.size == 0) {
			stop = i;
			return Result::NeedsReader;
		}
		const uint64_t first_page = entry.address & ~uint64_t {0xfff};
		const uint64_t last_page  = (entry.address + entry.size - 1u) & ~uint64_t {0xfff};
		if (use_page_cache && first_page == last_page) {
			auto& slot = t_cache[(first_page >> 12u) & 63u];
			if (slot.gen == active_gen && slot.page == first_page) {
				if (std::memcmp(slot.backing + (entry.address & 0xfffu), log_bytes + entry.first, entry.size) != 0) {
					stop = i;
					return Result::Changed;
				}
				stats.entries_fast++;
				stats.runs++;
				continue;
			}
			if (IsGpuAddressRange(first_page, 0x1000) &&
			    !buffers.IsRegionGpuModified(first_page, 0x1000) &&
			    !buffers.HasPendingReadback(first_page, 0x1000) &&
			    (direct_empty || buffers.DirectRangeQuiet(first_page, 0x1000))) {
				if (const uint8_t* backing = TryGetBackingPage(first_page); backing != nullptr) {
					slot = {first_page, backing, active_gen};
					if (std::memcmp(backing + (entry.address & 0xfffu), log_bytes + entry.first, entry.size) != 0) {
						stop = i;
						return Result::Changed;
					}
					stats.entries_fast++;
					stats.runs++;
					continue;
				}
			}
		}
		if (!IsGpuAddressRange(entry.address, entry.size) ||
		    buffers.HasGpuDirtyBytes(entry.address, entry.size) ||
		    buffers.HasPendingReadback(entry.address, entry.size) ||
		    !buffers.DirectRangeQuiet(entry.address, entry.size)) {
			stop = i;
			return Result::NeedsReader;
		}
		const int cmp = CompareBacking(entry.address, log_bytes + entry.first, entry.size);
		if (cmp == 1) {
			stop = i;
			return Result::Changed;
		}
		if (cmp != 0) {
			stop = i;
			return Result::NeedsReader;
		}
		stats.entries_fast++;
		stats.runs++;
	}
	stop = log.entries.size();
	return Result::Unchanged;
}

bool TryPrepareHostWrite(uint64_t vaddr, uint64_t size) {
	return g_gpu_resources != nullptr && IsGpuAddressRange(vaddr, size) && g_gpu_resources->InvalidateMemory(vaddr, size);
}

bool TryPrepareHostOverwrite(uint64_t vaddr, uint64_t size) {
	return TryPrepareHostWrite(vaddr, size);
}

void NoteEmulatorWrite(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources != nullptr && IsGpuAddressRange(vaddr, size)) {
		(void)g_gpu_resources->InvalidateMemory(vaddr, size);
	}
}

bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
	const bool written = Shim::TryWriteBackedRuns(vaddr, data, size);
	if (written) NoteEmulatorWrite(vaddr, size);
	return written;
}

bool TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size) {
	bool cp_overlay = false;
	if (g_gpu_resources != nullptr && IsGpuAddressRange(vaddr, size)) {
		auto& buffers = GetGpuResources().GetBufferCache();
		if (Graphics::GuestGpu::IsGpuThread()) {
			if (buffers.HasGpuDirtyBytes(vaddr, size)) {
				buffers.ReadMemory(vaddr, size);
			}
			cp_overlay = buffers.WaitPendingReadbacks(vaddr, size);
		} else {
			if (buffers.HasGpuDirtyBytesShared(vaddr, size)) return false;
		}
	}
	if (!TryReadBacking(vaddr, data, size)) return false;
	if (cp_overlay) {
		GetGpuResources().GetBufferCache().ApplyDirectCpWrites(vaddr, data, size);
	}
	return true;
}

bool GuestImageReadArmed(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) return false;
	return g_gpu_resources->GetPageManager().ImageReadArmedInRange(vaddr, size);
}

bool GuestRangeWatched(uint64_t vaddr, uint64_t size) {
	if (g_gpu_resources == nullptr || !IsGpuAddressRange(vaddr, size)) return false;
	return g_gpu_resources->GetBufferCache().IsRegionGpuModified(vaddr, size);
}

void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept {
	if (!Shim::TryWriteBackedRuns(vaddr, data, size)) {
		Shim::Fail("KytyShim memory: required backing write failed, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
	}
}

void InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (size == 0) return;
	(void)GetGpuResources().InvalidateMemory(vaddr, size);
}

void InstallGpuResources(Graphics::RenderContext* resources) noexcept {
	if (resources != nullptr && g_gpu_resources != nullptr) Shim::Fail("KytyShim memory: InstallGpuResources called twice\n");
	if (resources == nullptr) {
		Graphics::ShaderRecompiler::IR::SetFastVerifyHook(nullptr);
		Graphics::ShaderRecompiler::IR::SetAheadReadHook(nullptr);
		Graphics::ShaderRecompiler::IR::SetOrdinaryReadHook(nullptr);
		Graphics::ShaderRecompiler::IR::SetPlainReadHook(nullptr);
		Shim::SetGpuRangeHooks(nullptr, nullptr);
	}
	g_gpu_resources = resources;
	if (resources != nullptr) {
		Shim::SetGpuRangeHooks(&MapGpuRange, &UnmapGpuRange);
		Graphics::ShaderRecompiler::IR::SetPlainReadHook(&ReadGuestPlain);
		Graphics::ShaderRecompiler::IR::SetOrdinaryReadHook(&OrdinaryReadHookImpl);
		Graphics::ShaderRecompiler::IR::SetAheadReadHook(&AheadReadHookImpl);
		Graphics::ShaderRecompiler::IR::SetFastVerifyHook(&FastVerifyHookImpl);
	}
}

bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	return g_gpu_resources != nullptr && g_gpu_resources->HandleFault(access, fault_vaddr);
}

}
