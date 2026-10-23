#include "core/host_memory.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <iterator>
#include <string>
#include <system_error>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace tox2::host {
namespace {

// Larger than the Windows guarantee: the whole fatal path (logging, formatting) runs on this stack.
constexpr std::size_t kFaultStackBytes = std::size_t{1} << 20;
// A fault this far below the stack's low bound counts as a stack overflow.
constexpr std::uintptr_t kStackOverflowWindow = 0x10000;

std::atomic<FaultHandler> g_fault_handler{nullptr};

constexpr int kFaultSignals[] = {SIGSEGV, SIGBUS};
struct sigaction g_previous_action[std::size(kFaultSignals)];

struct sigaction* previousAction(int signo) { return &g_previous_action[signo == SIGSEGV ? 0 : 1]; }

#if defined(__APPLE__) && defined(__aarch64__)
FaultAccess accessFromContext(void* context) {
    const std::uint64_t esr = static_cast<ucontext_t*>(context)->uc_mcontext->__es.__esr;
    const std::uint64_t exception_class = esr >> 26;
    if (exception_class == 0x20 || exception_class == 0x21) {
        return FaultAccess::Execute;
    }
    // WnR: set when the aborted data access was a write.
    return (esr & (1u << 6)) != 0 ? FaultAccess::Write : FaultAccess::Read;
}
#elif defined(__APPLE__) && defined(__x86_64__)
FaultAccess accessFromContext(void* context) {
    // Page-fault error code: bit 1 is set for a write, bit 4 for an instruction fetch.
    const std::uint32_t err = static_cast<ucontext_t*>(context)->uc_mcontext->__es.__err;
    return (err & (1u << 4)) != 0   ? FaultAccess::Execute
           : (err & (1u << 1)) != 0 ? FaultAccess::Write
                                    : FaultAccess::Read;
}
#else
FaultAccess accessFromContext(void*) { return FaultAccess::Read; }
#endif

bool isStackOverflow(std::uintptr_t address) {
#if defined(__APPLE__)
    const pthread_t self = pthread_self();
    const auto high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
    const std::uintptr_t low = high - pthread_get_stacksize_np(self);
    return address < low && address >= low - kStackOverflowWindow;
#else
#error "host_memory_posix.cpp: isStackOverflow has no implementation for this platform"
#endif
}

void signalHandler(int signo, siginfo_t* info, void* context) {
    if (FaultHandler handler = g_fault_handler.load()) {
        const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
        handler(FaultInfo{isStackOverflow(address), address, accessFromContext(context)});
    }
    // Declined: put the previous action back and return, so the access faults again into it.
    sigaction(signo, previousAction(signo), nullptr);
}

#if defined(__APPLE__)
// Protection of the region holding `address`, or false when the address is unmapped.
bool queryRegion(std::uintptr_t address, vm_prot_t& protection, std::uintptr_t& region_end) {
    mach_vm_address_t start = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count, &object_name) != KERN_SUCCESS ||
        start > address) {
        return false;
    }
    protection = info.protection;
    region_end = static_cast<std::uintptr_t>(start + size);
    return true;
}
#endif

// The alternate signal stack of one thread, released when the thread exits. `base` starts with one
// inaccessible guard page, so a fatal path that outgrows the stack faults instead of overwriting the
// mapping below it.
struct FaultStack {
    void* base = nullptr;
    std::size_t bytes = 0;

    ~FaultStack() {
        if (base != nullptr) {
            stack_t disabled{};
            disabled.ss_flags = SS_DISABLE;
            sigaltstack(&disabled, nullptr);
            munmap(base, bytes);
        }
    }
};

}  // namespace

std::size_t pageSize() {
    static const std::size_t size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

void* reserve(std::size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool commit(void* address, std::size_t bytes) { return mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0; }

void decommit(void* address, std::size_t bytes) {
    // Mapping fresh anonymous memory over the range drops the old pages, which a plain
    // mprotect(PROT_NONE) would keep.
    mmap(address, bytes, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
}

void release(void* address, std::size_t bytes) { munmap(address, bytes); }

PageState queryPage(const void* address) {
#if defined(__APPLE__)
    vm_prot_t protection;
    std::uintptr_t region_end;
    if (!queryRegion(reinterpret_cast<std::uintptr_t>(address), protection, region_end)) {
        return PageState::Free;
    }
    return protection == VM_PROT_NONE ? PageState::Reserved : PageState::Committed;
#else
#error "host_memory_posix.cpp: queryPage has no implementation for this platform"
#endif
}

bool readable(const void* address, std::size_t bytes) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = begin + std::max<std::size_t>(bytes, 1);
    if (end < begin) {
        return false;
    }
    for (std::uintptr_t at = begin; at < end;) {
        vm_prot_t protection;
        std::uintptr_t region_end;
        if (!queryRegion(at, protection, region_end) || (protection & VM_PROT_READ) == 0) {
            return false;
        }
        at = region_end;
    }
    return true;
#else
#error "host_memory_posix.cpp: readable has no implementation for this platform"
#endif
}

std::string lastErrorText() {
    const int code = errno;
    return std::error_code(code, std::generic_category()).message() + " (errno " + std::to_string(code) + ")";
}

void installFaultHandler(FaultHandler handler) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
    g_fault_handler.store(handler);
    struct sigaction action{};
    action.sa_sigaction = &signalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signo : kFaultSignals) {
        // macOS reports protection faults as SIGBUS, other systems as SIGSEGV.
        sigaction(signo, &action, previousAction(signo));
    }
}

void reserveFaultStack() {
    thread_local FaultStack stack;
    if (stack.base != nullptr) {
        return;
    }
    const std::size_t guard = pageSize();
    const std::size_t bytes = guard + kFaultStackBytes;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return;
    }
    stack_t alt{};
    alt.ss_sp = static_cast<char*>(p) + guard;
    alt.ss_size = kFaultStackBytes;
    if (mprotect(p, guard, PROT_NONE) != 0 || sigaltstack(&alt, nullptr) != 0) {
        munmap(p, bytes);
        return;
    }
    stack.base = p;
    stack.bytes = bytes;
}

}  // namespace tox2::host
