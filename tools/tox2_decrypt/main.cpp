#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    std::string out;
    std::string err;
    const int code = tox2::decrypt::runCli(args, out, err);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fwrite(err.data(), 1, err.size(), stderr);
    return code;
}
