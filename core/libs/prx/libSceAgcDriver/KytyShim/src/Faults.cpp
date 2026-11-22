#include "prx/libSceAgcDriver/KytyShim/src/Faults.hpp"
#include "prx/libSceAgcDriver/KytyShim/src/MemoryShim.hpp"

#include "common/debugCounters.h"
#include "common/envRead.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include <atomic>
#include <cstdio>
#include "graphics/host_gpu/pageManager.h"
#include "kernel/memory.h"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"

#include <cstddef>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <ucontext.h>
#endif

#if !defined(ANYPS5_KYTY)
#error "KytyShim is built only with ANYPS5_GPU_BACKEND=kyty"
#endif

namespace KytyShim {
namespace {

using GpuAccess = Libs::Graphics::PageFaultAccess;

#ifdef _WIN32
void RearmGuardPage(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page)) != sizeof(page) || page.State != MEM_COMMIT || (page.Protect & PAGE_GUARD) != 0) return;
    DWORD previous = 0;
    VirtualProtect(page.BaseAddress, 1, page.Protect | PAGE_GUARD, &previous);
}
#else
constexpr std::uint64_t PageFaultWrite = 1u << 1;
constexpr std::uint64_t PageFaultFetch = 1u << 4;

GpuAccess DecodeAccess(const ucontext_t& context) {
    const auto error = static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_ERR]);
    if ((error & PageFaultFetch) != 0) return GpuAccess::Execute;
    if ((error & PageFaultWrite) != 0) return GpuAccess::Write;
    return GpuAccess::Read;
}
#endif

bool PrepareHostWrite(std::uintptr_t address, std::size_t bytes) {
    return Libs::LibKernel::Memory::TryPrepareHostWrite(address, bytes);
}

bool ResolveFault(GpuAccess access, std::uint64_t address) {
    Libs::Graphics::RetriedFaultScope retried_fault;
    if (Libs::LibKernel::Memory::HandleGpuFault(access, address)) {
        Common::DebugCounters::Add(Common::DebugCounters::Counter::GpuFaults);
        return true;
    }
    return false;
}

}

#ifdef _WIN32
bool HandleHostFault(void* exceptionPointers) {
    auto* exception = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    if (exception == nullptr || exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr) return false;
    const auto& record = *exception->ExceptionRecord;
    const bool guardPage = record.ExceptionCode == STATUS_GUARD_PAGE_VIOLATION;
    if ((record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION && !guardPage) || record.NumberParameters < 2) return false;
    const auto access = record.ExceptionInformation[0] == 8 ? GpuAccess::Execute : record.ExceptionInformation[0] == 1 ? GpuAccess::Write : GpuAccess::Read;
    const DWORD lastError = GetLastError();
    if (access == GpuAccess::Read && Libs::Graphics::GuestGpu::IsGpuThread() && Common::EnvRead::PerfLines()) {
        static std::atomic_uint gpu_rip_samples {0};
        const std::uint32_t     n = gpu_rip_samples.fetch_add(1, std::memory_order_relaxed);
        if ((n & 0x7fu) == 0u && n < 8192u) {
            static HMODULE self_mod = [] {
                HMODULE m = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&HandleHostFault), &m);
                return m;
            }();
            const auto base = reinterpret_cast<std::uintptr_t>(self_mod);
            const auto rip  = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
            const auto rva  = (rip >= base && rip - base < 0x10000000ull) ? (rip - base) : rip;
            std::fprintf(stderr, "FAULT_GPU_RIP n=%u addr=0x%llx rva=0x%llx\n", n,
                         static_cast<unsigned long long>(record.ExceptionInformation[1]),
                         static_cast<unsigned long long>(rva));
        }
    }
    const bool handled = ResolveFault(access, static_cast<std::uint64_t>(record.ExceptionInformation[1]));
    if (handled && guardPage) RearmGuardPage(static_cast<std::uintptr_t>(record.ExceptionInformation[1]));
    SetLastError(lastError);
    return handled;
}

void InstallFaultChain() {
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(&PrepareHostWrite);
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(&HandleHostFault);
}

void RemoveFaultChain() {
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(nullptr);
}
#else
bool HandleHostFault(int signal, siginfo_t* info, void* native) {
    if ((signal != SIGSEGV && signal != SIGBUS) || info == nullptr || native == nullptr) return false;
    auto& context = *static_cast<ucontext_t*>(native);
    return ResolveFault(DecodeAccess(context), reinterpret_cast<std::uint64_t>(info->si_addr));
}

void InstallFaultChain() {
    GuestArena::GuestArenaSetHostWriteHold_nid_no_patch(&Libs::LibKernel::Memory::Shim::HoldHostWrite, &Libs::LibKernel::Memory::Shim::ReleaseHostWrite);
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(&PrepareHostWrite);
    GuestWriteWatch::GuestWriteWatchSetGpuFaultHandler_nid_no_patch(&HandleHostFault);
}

void RemoveFaultChain() {
    GuestWriteWatch::GuestWriteWatchSetGpuFaultHandler_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetHostWriteHold_nid_no_patch(nullptr, nullptr);
}
#endif

}
