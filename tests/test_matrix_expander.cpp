// tests/test_matrix_expander.cpp — Unit tests for matrix expansion
#include "test_runner.h"
#include "../src/workflow/WorkflowParser.h"
#include "../src/workflow/MatrixExpander.h"

#include <set>

using namespace runner;
using namespace test;

static Matrix makeMatrix(
    std::map<std::string, std::vector<std::string>> axes,
    std::vector<std::map<std::string,std::string>> include = {},
    std::vector<std::map<std::string,std::string>> exclude = {})
{
    Matrix m;
    m.axes    = std::move(axes);
    m.include = std::move(include);
    m.exclude = std::move(exclude);
    return m;
}

int main() {
    std::cout << "=== MatrixExpander tests ===\n\n";

    run("empty matrix returns empty", []() {
        Matrix m;
        auto result = expandMatrix(m);
        ASSERT(result.empty());
    });

    run("single axis: 3 values → 3 combinations", []() {
        auto m = makeMatrix({{"os", {"ubuntu", "haiku", "windows"}}});
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 3u);
    });

    run("single axis values preserved", []() {
        auto m = makeMatrix({{"os", {"ubuntu", "haiku"}}});
        auto result = expandMatrix(m);
        bool has_ubuntu = false, has_haiku = false;
        for (auto& r : result) {
            if (r.at("os") == "ubuntu") has_ubuntu = true;
            if (r.at("os") == "haiku")  has_haiku  = true;
        }
        ASSERT(has_ubuntu && has_haiku);
    });

    run("two axes: cross-product", []() {
        auto m = makeMatrix({
            {"os",   {"ubuntu", "haiku"}},
            {"arch", {"x64", "arm64"}}
        });
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 4u);
    });

    run("two axes: all combinations present", []() {
        auto m = makeMatrix({
            {"os",   {"ubuntu", "haiku"}},
            {"arch", {"x64", "arm64"}}
        });
        auto result = expandMatrix(m);
        std::set<std::string> keys;
        for (auto& r : result) {
            keys.insert(r.at("os") + "/" + r.at("arch"));
        }
        ASSERT_EQ(keys.size(), 4u);
        ASSERT(keys.count("ubuntu/x64")  == 1);
        ASSERT(keys.count("ubuntu/arm64") == 1);
        ASSERT(keys.count("haiku/x64")   == 1);
        ASSERT(keys.count("haiku/arm64") == 1);
    });

    run("three axes: correct count", []() {
        auto m = makeMatrix({
            {"os",     {"ubuntu", "haiku"}},
            {"arch",   {"x64", "arm64"}},
            {"config", {"debug", "release"}}
        });
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 8u);
    });

    run("include: augments matching combination", []() {
        auto m = makeMatrix(
            {{"os", {"ubuntu", "haiku"}}, {"arch", {"x64"}}},
            {/* include: */ {{"os", "haiku"}, {"extra", "yes"}}});
        auto result = expandMatrix(m);
        // Find the haiku/x64 combination
        bool found = false;
        for (auto& r : result) {
            if (r.at("os") == "haiku") {
                ASSERT_EQ(r.at("extra"), "yes");
                found = true;
            }
        }
        ASSERT(found);
    });

    run("include: adds new row when no match", []() {
        auto m = makeMatrix(
            {{"os", {"ubuntu"}}},
            {/* include: */ {{"os", "haiku"}, {"arch", "x64"}}});
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 2u);
        bool has_haiku = false;
        for (auto& r : result) {
            if (r.at("os") == "haiku") {
                ASSERT_EQ(r.at("arch"), "x64");
                has_haiku = true;
            }
        }
        ASSERT(has_haiku);
    });

    run("exclude: removes matching combinations", []() {
        auto m = makeMatrix(
            {{"os", {"ubuntu", "haiku"}}, {"arch", {"x64", "arm64"}}},
            {},
            {/* exclude: */ {{"os", "haiku"}, {"arch", "arm64"}}});
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 3u);
        for (auto& r : result) {
            ASSERT(!(r.at("os") == "haiku" && r.at("arch") == "arm64"));
        }
    });

    run("exclude: removes multiple matches", []() {
        auto m = makeMatrix(
            {{"os", {"ubuntu", "haiku"}}, {"arch", {"x64"}}},
            {},
            {/* exclude: */ {{"os", "ubuntu"}}, {{"os", "haiku"}}});
        auto result = expandMatrix(m);
        ASSERT(result.empty());
    });

    run("include then exclude interaction", []() {
        // Include adds haiku/arm64; then exclude removes it
        auto m = makeMatrix(
            {{"os", {"ubuntu"}}, {"arch", {"x64"}}},
            {/* include: */ {{"os", "haiku"}, {"arch", "arm64"}}},
            {/* exclude: */ {{"os", "haiku"}}});
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 1u);
        ASSERT_EQ(result[0].at("os"), "ubuntu");
    });

    run("single-axis matrix with one value", []() {
        auto m = makeMatrix({{"os", {"haiku"}}});
        auto result = expandMatrix(m);
        ASSERT_EQ(result.size(), 1u);
        ASSERT_EQ(result[0].at("os"), "haiku");
    });

    return summary();
}
