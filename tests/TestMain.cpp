// LegacyAVR.Tests.exe
//   (no args)        run all unit tests (exit code = number of failures)
//   --filter TEXT    run tests whose "Suite.Name" contains TEXT
//   --bench          run the real-time performance benchmark (bench.cpp)
//   --latency        measure pipeline latency per output-margin setting (bench.cpp)
//   --report FILE    also write the names of failed tests to FILE (used by build.ps1)
//   --list           list tests

#include "TestFramework.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

int runBenchmarks();        // bench.cpp
int runLatencyReport();     // bench.cpp

int main(int argc, char** argv) {
    std::string filter, report;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bench")) return runBenchmarks();
        if (!std::strcmp(argv[i], "--latency")) return runLatencyReport();
        if (!std::strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        if (!std::strcmp(argv[i], "--list")) {
            for (auto& t : avrtest::registry()) std::printf("%s.%s\n", t.suite, t.name);
            return 0;
        }
    }

    int failed = 0, run = 0;
    std::vector<std::string> failedNames;
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& t : avrtest::registry()) {
        const std::string full = std::string(t.suite) + "." + t.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        ++run;
        const auto s = std::chrono::steady_clock::now();
        try {
            t.fn();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
            std::printf("[ PASS ] %-48s %8.1f ms\n", full.c_str(), ms);
        } catch (const avrtest::Failure& f) {
            ++failed;
            failedNames.push_back(full + "  ->  " + f.message);
            std::printf("[ FAIL ] %s\n         %s\n", full.c_str(), f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            failedNames.push_back(full + "  ->  exception: " + e.what());
            std::printf("[ FAIL ] %s\n         exception: %s\n", full.c_str(), e.what());
        }
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d/%d tests passed, %d checks, %.2f s\n", run - failed, run, avrtest::checks(), total);
    if (!failedNames.empty()) {
        std::printf("\nFailed tests:\n");
        for (const auto& n : failedNames) std::printf("  %s\n", n.c_str());
    }
    if (!report.empty()) {
        std::ofstream f(report, std::ios::trunc);
        for (const auto& n : failedNames) f << n << '\n';
    }
    return failed;
}
