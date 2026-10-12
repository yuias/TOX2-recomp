#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tox2 {

// A body per source; an empty string is skipped. Called on the perf thread only, so a source may keep its previous
// snapshot in its own state.
using PerfSource = std::function<std::string()>;

// Starts one detached host thread that every `interval` logs "tox2: perf: t <s> s: <body>" at debug level for each
// source (t = systemTimeUs() / 1e6 with one decimal). Idempotent; later calls do nothing.
void startPerfLog(std::vector<PerfSource> sources, std::chrono::milliseconds interval = std::chrono::seconds(1));

std::string formatPerfLine(double t_s, std::string_view body);

}  // namespace tox2
