// Checks the behaviour of the OS write watch (MEM_WRITE_WATCH) that the RSX texture hash skip relies on, in the layout
// of the guest reservation. It links no runtime library: the guarantees come from Windows, not from this project.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

// The guest reservation: 4 GiB plus the guard (runtime/include/tox2/ppu_context.h), RSX local memory at 0xC0000000.
constexpr std::size_t kReserveSize = (std::size_t{1} << 32) + 0x10000;
constexpr std::size_t kLocalOffset = 0xC0000000u;
constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kWindowPages = 256;  // 1 MiB committed

using PageSet = std::vector<std::size_t>;

std::uint8_t* g_window;

std::uint8_t* pageAddr(std::size_t page) { return g_window + page * kPageSize; }

void touch(std::size_t page) {
    // A byte that changes every call, so the store is a real write.
    static std::uint8_t counter;
    *reinterpret_cast<volatile std::uint8_t*>(pageAddr(page)) = ++counter;
}

struct Query {
    UINT result = 0;  // return value of GetWriteWatch
    DWORD error = 0;
    ULONG_PTR count = 0;  // as returned by the call
    ULONG granularity = 0;
    PageSet pages;  // indices relative to the window
};

// One GetWriteWatch call over [first, first + count) pages of the window.
Query query(std::size_t first, std::size_t count, bool reset, std::size_t capacity = 0) {
    Query q;
    if (capacity == 0) {
        capacity = count;
    }
    std::vector<PVOID> addresses(capacity);
    ULONG_PTR n = capacity;
    DWORD gran = 0;
    SetLastError(0);
    q.result = GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, pageAddr(first), count * kPageSize, addresses.data(),
                             &n, &gran);
    q.error = GetLastError();
    q.count = n;
    q.granularity = gran;
    if (q.result == 0) {
        for (ULONG_PTR i = 0; i < n; ++i) {
            q.pages.push_back((static_cast<std::uint8_t*>(addresses[i]) - g_window) / kPageSize);
        }
    }
    return q;
}

// Makes the next query report only later writes.
void drain() { query(0, kWindowPages, true); }

std::string describe(const PageSet& pages) {
    std::string s = "{";
    for (std::size_t i = 0; i < pages.size(); ++i) {
        s += (i != 0 ? ", " : "") + std::to_string(pages[i]);
    }
    return s + "}";
}

// Sets are compared in order: the API reports ascending addresses.
bool expectPages(const Query& q, const PageSet& expected, const char* what) {
    const bool ok = q.result == 0 && q.granularity == kPageSize && q.pages == expected;
    if (!ok) {
        std::fprintf(stderr, "write watch: %s: result %u error %lu granularity %lu, reported %s, expected %s\n", what,
                     q.result, q.error, static_cast<unsigned long>(q.granularity), describe(q.pages).c_str(),
                     describe(expected).c_str());
    }
    return ok;
}

// (A) Sub-range isolation and reset: what the per-validation design (D55.1) relies on.
void testSubRangeIsolation() {
    drain();
    touch(3);
    touch(10);
    touch(200);

    CHECK(expectPages(query(0, 8, true), {3}, "reset over pages [0, 8)"));
    CHECK(expectPages(query(0, 256, false), {10, 200}, "no reset over [0, 256)"));
    // The no-reset call left them in place.
    CHECK(expectPages(query(0, kWindowPages, true), {10, 200}, "reset over the whole window"));
    CHECK(expectPages(query(0, kWindowPages, true), {}, "second reset over the whole window"));

    touch(10);
    CHECK(expectPages(query(0, kWindowPages, true), {10}, "write after a reset"));

    // A range that starts and ends inside the window, and writes just outside it, stay separate.
    touch(99);
    touch(100);
    touch(101);
    CHECK(expectPages(query(100, 1, true), {100}, "reset over one page"));
    CHECK(expectPages(query(0, kWindowPages, true), {99, 101}, "neighbours of a reset page"));
}

// Reads `pages` pages from `file` into window page `first_page` and returns how many the write watch reported.
// Negative when the read fails or the bytes do not match `expected`.
int readFileReported(HANDLE file, std::size_t first_page, std::size_t pages, const std::uint8_t* expected) {
    drain();
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(pages * kPageSize);
    if (!ReadFile(file, pageAddr(first_page), want, &got, nullptr) || got != want) {
        return -1;
    }
    const Query q = query(first_page, pages, false);
    if (q.result != 0) {
        return -1;
    }
    const bool match = std::memcmp(pageAddr(first_page), expected, want) == 0;
    CHECK(match);
    CHECK(q.pages.size() <= pages);
    return static_cast<int>(q.pages.size());
}

// (B) Whether ReadFile into watched pages is reported is printed, not asserted: the explicit marks cover file reads
// whatever the answer.
void testReadFile() {
    std::vector<std::uint8_t> pattern(3 * kPageSize);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::uint8_t>((i * 7 + 1) & 0xFF);
    }

    wchar_t temp_dir[MAX_PATH + 1] = {};
    CHECK(GetTempPathW(MAX_PATH, temp_dir) != 0);
    const std::wstring path =
            std::wstring(temp_dir) + L"tox2_write_watch_" + std::to_wstring(GetCurrentProcessId()) + L".bin";

    const HANDLE out =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(out != INVALID_HANDLE_VALUE);
    if (out == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    CHECK(WriteFile(out, pattern.data(), static_cast<DWORD>(pattern.size()), &written, nullptr) &&
          written == pattern.size());
    CloseHandle(out);

    // Access, share mode and attributes of sys_fs.cpp's open.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        int r = readFileReported(file, 20, 3, pattern.data());
        std::printf("write watch: ReadFile (file, first read) reported %d of 3 pages\n", r);
        CHECK(r >= 0);

        CHECK(SetFilePointer(file, 0, nullptr, FILE_BEGIN) == 0);
        r = readFileReported(file, 30, 3, pattern.data());
        std::printf("write watch: ReadFile (file, cached) reported %d of 3 pages\n", r);
        CHECK(r >= 0);
        CloseHandle(file);
    }
    CHECK(DeleteFileW(path.c_str()) != 0);
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CHECK(CreatePipe(&read_end, &write_end, nullptr, 65536) != 0);
    if (read_end != nullptr) {
        DWORD put = 0;
        CHECK(WriteFile(write_end, pattern.data(), static_cast<DWORD>(kPageSize), &put, nullptr) && put == kPageSize);
        const int r = readFileReported(read_end, 40, 1, pattern.data());
        std::printf("write watch: ReadFile (pipe) reported %d of 1 pages\n", r);
        CHECK(r >= 0);
        CloseHandle(read_end);
        CloseHandle(write_end);
    }
}

void printQuery(const char* what, const Query& q) {
    std::printf("write watch: %s: result %u error %lu count %lu granularity %lu pages %s\n", what, q.result, q.error,
                static_cast<unsigned long>(q.count), static_cast<unsigned long>(q.granularity),
                describe(q.pages).c_str());
}

bool pageIsZero(std::size_t page) {
    const std::uint8_t* p = pageAddr(page);
    return std::all_of(p, p + kPageSize, [](std::uint8_t b) { return b == 0; });
}

// (C1) Decommit and recommit are printed, not asserted: the runtime marks both explicitly.
void testDecommitRecommit() {
    constexpr std::size_t kFirst = 50;
    constexpr std::size_t kCount = 4;
    constexpr std::size_t kVictim = 51;

    // Start from freshly committed pages so the commit below is a real one.
    CHECK(VirtualFree(pageAddr(kFirst), kCount * kPageSize, MEM_DECOMMIT) != 0);
    CHECK(VirtualAlloc(pageAddr(kFirst), kCount * kPageSize, MEM_COMMIT, PAGE_READWRITE) == pageAddr(kFirst));
    for (std::size_t page = kFirst; page < kFirst + kCount; ++page) {
        touch(page);
    }
    query(kFirst, kCount, true);

    CHECK(VirtualFree(pageAddr(kVictim), kPageSize, MEM_DECOMMIT) != 0);
    printQuery("C1 after decommit of page 51, query 50-53 (no reset)", query(kFirst, kCount, false));

    SetLastError(0);
    const void* recommitted = VirtualAlloc(pageAddr(kVictim), kPageSize, MEM_COMMIT, PAGE_READWRITE);
    CHECK(recommitted == pageAddr(kVictim));
    std::printf("write watch: C1 recommit of page 51: %s (error %lu)\n", recommitted != nullptr ? "ok" : "failed",
                GetLastError());
    const Query after = query(kFirst, kCount, false);
    printQuery("C1 after recommit of page 51, query 50-53 (no reset)", after);
    std::printf("write watch: C1 recommitted page 51 is %s\n",
                std::find(after.pages.begin(), after.pages.end(), kVictim) != after.pages.end() ? "reported"
                                                                                                : "not reported");
    CHECK(pageIsZero(kVictim));
}

// (C2) A range with an uncommitted page, printed. The page lies past the committed window of the reservation.
void testUncommittedInRange() {
    constexpr std::size_t kFirst = kWindowPages + 4;
    constexpr std::size_t kHole = kFirst + 2;

    for (std::size_t page = kFirst; page < kFirst + 4; ++page) {
        if (page != kHole) {
            CHECK(VirtualAlloc(pageAddr(page), kPageSize, MEM_COMMIT, PAGE_READWRITE) == pageAddr(page));
        }
    }
    query(kFirst, 1, true);
    touch(kFirst + 1);
    std::printf("write watch: C2 pages %zu-%zu, page %zu never committed, page %zu written\n", kFirst, kFirst + 3,
                kHole, kFirst + 1);
    // Reported page numbers are relative to the window; the range is not.
    printQuery("C2 query over the range with an uncommitted page (no reset)", query(kFirst, 4, false));
    printQuery("C2 query over the range with an uncommitted page (reset)", query(kFirst, 4, true));
}

// (C3) A result buffer smaller than the dirty count: whether a reset also clears pages that were not returned.
void testShortBuffer() {
    constexpr std::size_t kFirst = 70;
    constexpr std::size_t kCount = 4;

    query(kFirst, kCount, true);
    for (std::size_t page = kFirst; page < kFirst + kCount; ++page) {
        touch(page);
    }
    printQuery("C3 four pages written, reset with room for 2", query(kFirst, kCount, true, 2));
    const Query rest = query(kFirst, kCount, false, 8);
    printQuery("C3 then query with room for 8 (no reset)", rest);
    std::printf("write watch: C3 a reset with a short buffer left %zu of 4 pages reported\n", rest.pages.size());
}

}  // namespace

int main() {
    g_window = nullptr;
    void* base = VirtualAlloc(nullptr, kReserveSize, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    CHECK(base != nullptr);
    if (base == nullptr) {
        std::fprintf(stderr, "cannot reserve the test address space (error %lu)\n", GetLastError());
        std::printf("write watch tests: %d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }
    g_window = static_cast<std::uint8_t*>(base) + kLocalOffset;
    void* committed = VirtualAlloc(g_window, kWindowPages * kPageSize, MEM_COMMIT, PAGE_READWRITE);
    CHECK(committed == g_window);
    if (committed != nullptr) {
        // Fault every page in first, so a later report is never a demand-zero side effect.
        for (std::size_t page = 0; page < kWindowPages; ++page) {
            touch(page);
        }
        testSubRangeIsolation();
        testReadFile();
        testDecommitRecommit();
        testUncommittedInRange();
        testShortBuffer();
    }
    VirtualFree(base, 0, MEM_RELEASE);
    std::printf("write watch tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
