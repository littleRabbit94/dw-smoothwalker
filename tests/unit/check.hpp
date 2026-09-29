// The unit tests' framework: TEST registers a function under a suite, CHECK* record a failure and carry on, run()
// runs every test or one suite and answers non-zero on any failure or on a suite with no tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace check
{
    struct Test
    {
        const char* suite;
        const char* name;
        void (*body)();
    };

    inline auto tests() -> std::vector<Test>& { static std::vector<Test> all; return all; }
    inline int failures = 0;

    struct Register
    {
        Register(const char* suite, const char* name, void (*body)()) { tests().push_back({suite, name, body}); }
    };

    inline auto fail(const char* file, int line, const char* what) -> void
    {
        ++failures;
        std::printf("%s(%d): FAILED %s\n", file, line, what);
    }

    inline auto near_value(double a, double b, double tolerance, const char* file, int line, const char* what) -> void
    {
        if (std::abs(a - b) <= tolerance) return;
        fail(file, line, what);
        std::printf("    %.17g vs %.17g\n", a, b);
    }

    // argv[1]: a suite name; none runs every suite.
    inline auto run(int argc, char** argv) -> int
    {
        const char* only = argc > 1 ? argv[1] : nullptr;
        int ran = 0;
        for (const Test& t : tests())
        {
            if (only && std::strcmp(only, t.suite) != 0) continue;
            int before = failures;
            t.body();
            ++ran;
            std::printf("%s %s.%s\n", failures == before ? "ok  " : "FAIL", t.suite, t.name);
        }
        std::printf("%d tests, %d failed checks\n", ran, failures);
        return ran == 0 || failures != 0 ? 1 : 0;
    }
} // namespace check

#define TEST(suite, name)                                                                   \
    static void suite##_##name();                                                           \
    static const check::Register suite##_##name##_registered(#suite, #name, &suite##_##name); \
    static void suite##_##name()

#define CHECK(x) ((x) ? void() : check::fail(__FILE__, __LINE__, #x))
#define CHECK_EQ(a, b) (((a) == (b)) ? void() : check::fail(__FILE__, __LINE__, #a " == " #b))
#define CHECK_NEAR(a, b, tolerance) check::near_value((a), (b), (tolerance), __FILE__, __LINE__, #a " ~ " #b)
