#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "test_util.h"

int main() {
    using namespace tox2::decrypt::test;

    std::vector<TestCase> tests = registry();
    std::sort(tests.begin(), tests.end(),
              [](const TestCase& a, const TestCase& b) { return std::strcmp(a.name, b.name) < 0; });

    TestCounters& total = counters();
    for (const TestCase& test : tests) {
        const int failed_before = total.failed;
        test.run();
        std::printf("%s %s\n", total.failed == failed_before ? "ok" : "FAIL", test.name);
    }
    std::printf("decrypt core tests: %d passed, %d failed\n", total.passed, total.failed);
    return total.failed == 0 ? 0 : 1;
}
