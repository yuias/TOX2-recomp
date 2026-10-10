#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tox2 {

// PS3 timebase frequency; tox2Mftb and sys_time_get_timebase_frequency report it.
inline constexpr std::uint64_t kTimebaseFrequency = 79800000;

// Microseconds since the runtime started (a monotonic host clock).
std::uint64_t systemTimeUs();

// Run statistics printed on every stop path. Counters are updated under the guest lock; the
// census keeps its own mutex as well so that the supervisor can print without holding it.

// Counts one syscall. `name` may be null (printed as "?"). `implemented` is false for calls that
// went through the default handler. The first call of each number logs "tox2: first sc N name".
void censusSyscall(std::uint32_t number, const char* name, bool implemented);

// Counts one HLE import call; the first call logs "tox2: first hle module::name".
void censusHle(const char* module, const char* name, bool implemented);

// Attaches a note to an import that was already counted; the census line becomes
// "  module::name: N [default] <note>". Ignored when the import has not been called yet.
void censusHleNote(const char* module, const char* name, std::string note);

// Counts a dynamic transfer to an address with no generated function. Returns true the first
// time this (target, site) pair is seen.
bool censusUnknownTarget(std::uint32_t target, std::uint32_t site);

struct ThreadCensusEntry {
    enum class State { Running, Blocked, Exited };
    std::uint32_t id = 0;
    std::string name;
    State state = State::Running;
    std::string blocked_in;  // meaningful for State::Blocked
};

// Returns every guest thread ever created, in creation order. Registered by the thread layer so
// the census does not depend on it.
using ThreadCensusFn = std::vector<ThreadCensusEntry> (*)();
void setThreadCensusProvider(ThreadCensusFn fn);

// What the RSX consumer counted: one entry per distinct register written through the FIFO.
struct RsxMethodCount {
    std::uint32_t method = 0;  // byte offset of the register
    std::uint64_t count = 0;
    std::string name;  // empty when the register has no known name
};

struct RsxCensus {
    std::uint64_t headers = 0;
    std::uint64_t flips = 0;
    std::vector<RsxMethodCount> methods;
    // Guest interrupt handler calls made by the interrupt thread; the line is printed once any ran.
    std::uint64_t vblank_handler_runs = 0, flip_handler_runs = 0, user_handler_runs = 0;
    // Set while a renderer executes the commands; the line then ends with its draw counts.
    bool rendering = false;
    std::uint64_t draws_rendered = 0, draws_skipped = 0;
};

// Registered by the RSX consumer; the section is printed only while a provider is set. The provider
// runs without the census lock and may be called from any thread.
using RsxCensusFn = RsxCensus (*)();
void setRsxCensusProvider(RsxCensusFn fn);

// What the RSX layer could not handle: one entry per "<category> <key>" with its count, most frequent
// first, and the occurrences of kinds beyond the layer's cap.
struct RsxUnimplementedCensus {
    std::vector<std::pair<std::string, std::uint64_t>> kinds;
    std::uint64_t dropped = 0;
};

// The section is printed only while a provider is set and it reports something. `reset` runs from
// resetCensus() before the providers are forgotten. Both run without the census lock.
using RsxUnimplementedFn = RsxUnimplementedCensus (*)();
using CensusResetFn = void (*)();
void setRsxUnimplementedProvider(RsxUnimplementedFn fn, CensusResetFn reset);

// The RSX provider's current numbers, empty without a provider. Any thread; for callers that want
// the flip count at a point in time (the movie player's per-movie windows).
RsxCensus rsxCensusSnapshot();

// One line per movie the cellSail player has finished, in the order they ended. The section
// "census: sail <n> movies" is printed only while a provider is set and it returns lines. The
// provider runs without the census lock and may be called from any thread.
using SailCensusFn = std::vector<std::string> (*)();
void setSailCensusProvider(SailCensusFn fn);

// Lines of the audio engine and cellAudio statistics. The section "census: audio" is printed only
// while a provider is set and it returns lines. The provider runs without the census lock and may be
// called from any thread.
using AudioCensusFn = std::vector<std::string> (*)();
void setAudioCensusProvider(AudioCensusFn fn);

// Called once by printCensus() after the text census, on the stop path; the RSX consumer uses it to
// log the frame-builder totals and write its argument census (--rsx-census). Runs without the census
// lock.
using CensusWriterFn = void (*)();
void setRsxStopWriter(CensusWriterFn fn);

// The census as text, one line per element, no trailing newline. Entries are sorted by count
// (descending) and then by name. In the syscall and HLE headers "default-handled" is the number of
// distinct entries that used the default handler.
std::string formatCensus();

// Logs formatCensus() line by line.
void printCensus();

// Registers printCensus as the census printer of the stop path.
void installCensus();

// Forgets all counters. Intended for tests.
void resetCensus();

}  // namespace tox2
