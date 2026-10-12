#include "core/perf_log.h"

#include <atomic>
#include <format>
#include <thread>
#include <utility>

#include "core/census.h"
#include "core/log.h"

namespace tox2 {

std::string formatPerfLine(double t_s, std::string_view body) {
    return std::format("tox2: perf: t {:.1f} s: {}", t_s, body);
}

void startPerfLog(std::vector<PerfSource> sources, std::chrono::milliseconds interval) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;

    // Detached because the process ends without joining its threads on every stop path.
    std::thread([sources = std::move(sources), interval] {
        // Ticks are scheduled from the start time so a slow source does not stretch the period.
        auto next = std::chrono::steady_clock::now() + interval;
        for (;;) {
            std::this_thread::sleep_until(next);
            next += interval;
            const bool log = logEnabled(LogLevel::Debug);
            const double t_s = static_cast<double>(systemTimeUs()) / 1e6;
            for (const PerfSource& source : sources) {
                if (!source) continue;
                const std::string body = source();
                if (log && !body.empty()) logWrite(LogLevel::Debug, formatPerfLine(t_s, body));
            }
        }
    }).detach();
}

}  // namespace tox2
