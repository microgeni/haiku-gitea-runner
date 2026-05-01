// tests/test_runner.h — minimal unit test harness (no external dependencies)
#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <sstream>

namespace test {

struct TestResult {
    std::string name;
    bool        passed;
    std::string message;
};

inline std::vector<TestResult>& results() {
    static std::vector<TestResult> r;
    return r;
}

inline void run(const std::string& name, std::function<void()> fn) {
    try {
        fn();
        results().push_back({name, true, ""});
        std::cout << "  \033[32m✓\033[0m " << name << "\n";
    } catch (const std::exception& e) {
        results().push_back({name, false, e.what()});
        std::cout << "  \033[31m✗\033[0m " << name << "\n"
                  << "    " << e.what() << "\n";
    }
}

inline int summary() {
    int passed = 0, failed = 0;
    for (auto& r : results()) {
        if (r.passed) ++passed; else ++failed;
    }
    std::cout << "\n" << passed << "/" << (passed+failed) << " tests passed";
    if (failed) std::cout << "  (" << failed << " FAILED)";
    std::cout << "\n";
    return failed;
}

#define ASSERT(expr) \
    do { if (!(expr)) { \
        std::ostringstream _os; \
        _os << "ASSERT failed: " #expr " at " __FILE__ ":" << __LINE__; \
        throw std::runtime_error(_os.str()); \
    }} while(0)

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (!(_a == _b)) { \
            std::ostringstream _os; \
            _os << "ASSERT_EQ failed: [" << _a << "] != [" << _b << "] at " \
                << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(_os.str()); \
        } \
    } while(0)

#define ASSERT_NE(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a == _b) { \
            std::ostringstream _os; \
            _os << "ASSERT_NE failed: [" << _a << "] == [" << _b << "] at " \
                << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(_os.str()); \
        } \
    } while(0)

#define ASSERT_THROWS(expr) \
    do { \
        bool _threw = false; \
        try { expr; } catch (...) { _threw = true; } \
        if (!_threw) { \
            throw std::runtime_error("ASSERT_THROWS: did not throw: " #expr); \
        } \
    } while(0)

} // namespace test
