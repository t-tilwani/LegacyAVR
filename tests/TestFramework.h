// LegacyAVR - minimal self-registering test framework (no external dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace avrtest {

struct TestCase {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

struct Failure {
    std::string message;
};

inline int& checks() { static int n = 0; return n; }

// Printable form of a CHECK_EQ operand (numbers, enums, strings; "?" otherwise).
template <class T>
std::string toStr(const T& v) {
    if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<long long>(v));
    else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);
    else if constexpr (std::is_convertible_v<const T&, std::string>) return std::string(v);
    else return "?";
}

[[noreturn]] inline void fail(const char* file, int line, const std::string& what) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s(%d): %s", file, line, what.c_str());
    throw Failure{buf};
}

} // namespace avrtest

#define AVR_CAT2(a, b) a##b
#define AVR_CAT(a, b) AVR_CAT2(a, b)

#define TEST(suite, name)                                                                           \
    static void AVR_CAT(test_, AVR_CAT(suite, AVR_CAT(_, name)))();                                 \
    static ::avrtest::Registrar AVR_CAT(reg_, AVR_CAT(suite, AVR_CAT(_, name))){                    \
        #suite, #name, &AVR_CAT(test_, AVR_CAT(suite, AVR_CAT(_, name)))};                           \
    static void AVR_CAT(test_, AVR_CAT(suite, AVR_CAT(_, name)))()

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++::avrtest::checks();                                                                       \
        if (!(cond)) ::avrtest::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed");               \
    } while (0)

#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        ++::avrtest::checks();                                                                       \
        const auto va_ = (a);                                                                        \
        const auto vb_ = (b);                                                                        \
        if (!(va_ == vb_))                                                                           \
            ::avrtest::fail(__FILE__, __LINE__,                                                      \
                            "CHECK_EQ(" #a ", " #b ") failed: " + ::avrtest::toStr(va_) + " != " +   \
                                ::avrtest::toStr(vb_));                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                        \
    do {                                                                                             \
        ++::avrtest::checks();                                                                       \
        const double va_ = double(a), vb_ = double(b);                                               \
        if (!(std::fabs(va_ - vb_) <= double(tol)))                                                  \
            ::avrtest::fail(__FILE__, __LINE__,                                                      \
                            "CHECK_NEAR(" #a ", " #b ") failed: " + std::to_string(va_) + " vs " +   \
                                std::to_string(vb_) + " tol " + std::to_string(double(tol)));        \
    } while (0)
