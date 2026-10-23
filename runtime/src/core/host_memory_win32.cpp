#include "core/host_memory.h"

#include <algorithm>
#include <atomic>
#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tox2::host {
namespace {

constexpr ULONG kFaultStackBytes = 0x20000;

std::atomic<FaultHandler> g_fault_handler{nullptr};

LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& rec = *info->ExceptionRecord;
    FaultInfo fault{};
    if (rec.ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fault.stack_overflow = true;
    } else if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
        const ULONG_PTR access = rec.ExceptionInformation[0];
        fault.address = rec.ExceptionInformation[1];
        fault.access = access == 1 ? FaultAccess::Write : access == 8 ? FaultAccess::Execute : FaultAccess::Read;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (FaultHandler handler = g_fault_handler.load()) {
        handler(fault);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

std::size_t pageSize() {
    static const std::size_t size = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
    return size;
}

void* reserve(std::size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS); }

bool commit(void* address, std::size_t bytes) {
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

#if TOX2_RSX_WRITE_WATCH
void* reserveWriteWatched(std::size_t bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
}

bool collectWrittenPages(void* address, std::size_t bytes, std::span<void*> pages, std::size_t& count,
                         std::size_t& granularity) {
    ULONG_PTR found = pages.size();
    ULONG os_granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, address, bytes, pages.data(), &found, &os_granularity) != 0) {
        return false;
    }
    count = found;
    granularity = os_granularity;
    return true;
}
#endif

void decommit(void* address, std::size_t bytes) { VirtualFree(address, bytes, MEM_DECOMMIT); }

void release(void* address, std::size_t) { VirtualFree(address, 0, MEM_RELEASE); }

PageState queryPage(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(address, &info, sizeof info) != sizeof info) {
        return PageState::Free;
    }
    return info.State == MEM_COMMIT    ? PageState::Committed
           : info.State == MEM_RESERVE ? PageState::Reserved
                                       : PageState::Free;
}

bool readable(const void* address, std::size_t bytes) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = begin + std::max<std::size_t>(bytes, 1);
    if (end < begin) {
        return false;
    }
    // One query covers a whole run of pages with the same state and protection.
    for (std::uintptr_t at = begin; at < end;) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info) != sizeof info ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

std::string lastErrorText() { return std::format("error {}", GetLastError()); }

void installFaultHandler(FaultHandler handler) {
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true)) {
        g_fault_handler.store(handler);
        AddVectoredExceptionHandler(1, &vectoredHandler);
    }
}

void reserveFaultStack() {
    ULONG bytes = kFaultStackBytes;
    SetThreadStackGuarantee(&bytes);
}

}  // namespace tox2::host
