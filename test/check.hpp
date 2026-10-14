// Minimal test helpers. No framework: each test is a function, CHECK records
// failures, run_tests() prints PASS/FAIL per test and sets the exit code.
#pragma once

#include <cstdio>

namespace check {

inline int failures = 0;  // failed CHECKs in the current test

// #expr turns the expression into text, so a failure prints what was checked.
#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); \
            ++check::failures;                                               \
        }                                                                    \
    } while (0)

struct Test {
    const char* name;
    const char* what;  // one plain-English line: what this test proves
    void (*fn)();      // pointer to a function taking nothing, returning nothing
};

// Returns 0 if every test passed, 1 otherwise (the process exit code).
template <int N>
int run_tests(const Test (&tests)[N]) {
    int failed_tests = 0;
    for (const Test& t : tests) {
        failures = 0;
        t.fn();
        std::printf("  %s  %-34s %s\n", failures ? "FAIL" : "pass", t.name, t.what);
        if (failures) ++failed_tests;
    }
    std::printf("%d/%d passed\n", N - failed_tests, N);
    return failed_tests ? 1 : 0;
}

}  // namespace check
