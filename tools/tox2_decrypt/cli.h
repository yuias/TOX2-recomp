#pragma once

#include <span>
#include <string>
#include <string_view>

namespace tox2::decrypt {

// Runs one command (`self`, `pup` or `keys`); `args` excludes the program name. `out` and `err` receive
// the text that main prints to stdout and stderr. Returns the exit code: 0 success, 1 failure, 2 usage
// error. Every error line starts with "error: " and never contains key bytes.
int runCli(std::span<const std::string_view> args, std::string& out, std::string& err);

}  // namespace tox2::decrypt
