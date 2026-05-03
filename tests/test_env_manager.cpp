// tests/test_env_manager.cpp — Unit tests for the EnvManager
#include "test_runner.h"
#include "../src/process/EnvManager.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>

using namespace runner;
using namespace test;

namespace fs = std::filesystem;

// Create a temporary directory for test workspace
static std::string makeTempDir() {
    // Use a fixed-size array sized to the template + NUL.
    // mkdtemp replaces the trailing XXXXXX in-place.
    char buf[] = "/tmp/act_runner_test_XXXXXX";
    char* dir = mkdtemp(buf);
    if (!dir) throw std::runtime_error("mkdtemp failed");
    return dir;
}

static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

int main() {
    std::cout << "=== EnvManager tests ===\n\n";

    run("setup creates workspace and protocol files", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        ASSERT(fs::exists(mgr.githubOutputPath()));
        ASSERT(fs::exists(mgr.githubEnvPath()));
        ASSERT(fs::exists(mgr.githubPathPath()));
        ASSERT(fs::exists(mgr.githubStatePath()));
        ASSERT(fs::exists(mgr.githubSummaryPath()));

        mgr.cleanup();
        fs::remove_all(ws + "/..");
    });

    run("currentEnv includes GITHUB_OUTPUT path", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        auto env = mgr.currentEnv();
        bool found = false;
        for (auto& [k, v] : env) {
            if (k == "GITHUB_OUTPUT") { found = true; ASSERT(!v.empty()); }
        }
        ASSERT(found);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("parseOutputs: simple KEY=VALUE", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubOutputPath(), "my_key=my_value\n");

        auto outputs = mgr.parseOutputs();
        ASSERT_EQ(outputs.at("my_key"), "my_value");

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("parseOutputs: multiple KEY=VALUE lines", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubOutputPath(), "key1=val1\nkey2=val2\nkey3=val3\n");

        auto outputs = mgr.parseOutputs();
        ASSERT_EQ(outputs.size(), 3u);
        ASSERT_EQ(outputs.at("key1"), "val1");
        ASSERT_EQ(outputs.at("key2"), "val2");
        ASSERT_EQ(outputs.at("key3"), "val3");

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("parseOutputs: multiline delimiter format", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        // GitHub's heredoc-style multiline format
        writeFile(mgr.githubOutputPath(),
                  "multiline_key<<EOF\nline one\nline two\nEOF\n");

        auto outputs = mgr.parseOutputs();
        ASSERT_EQ(outputs.at("multiline_key"), "line one\nline two");

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("applyEnvChanges: updates internal env map", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubEnvPath(), "NEW_VAR=new_value\nANOTHER=42\n");
        mgr.applyEnvChanges();

        auto env = mgr.currentEnv();
        bool found_new = false, found_another = false;
        for (auto& [k, v] : env) {
            if (k == "NEW_VAR"  && v == "new_value") found_new     = true;
            if (k == "ANOTHER"  && v == "42")        found_another = true;
        }
        ASSERT(found_new && found_another);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("applyPathChanges: prepends to PATH", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubPathPath(), "/my/new/path\n");
        mgr.applyPathChanges();

        auto env = mgr.currentEnv();
        bool found = false;
        for (auto& [k, v] : env) {
            if (k == "PATH" && v.substr(0, 12) == "/my/new/path") {
                found = true;
            }
        }
        ASSERT(found);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("resetBetweenSteps: truncates output file", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubOutputPath(), "key=value\n");
        ASSERT(fs::file_size(mgr.githubOutputPath()) > 0);

        mgr.resetBetweenSteps();
        ASSERT_EQ(fs::file_size(mgr.githubOutputPath()), 0u);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("resetBetweenSteps: keeps env file", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubEnvPath(), "KEY=val\n");
        mgr.resetBetweenSteps();
        // env file should NOT be cleared (cumulative across steps)
        ASSERT(fs::file_size(mgr.githubEnvPath()) > 0);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("parseState: reads GITHUB_STATE", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        writeFile(mgr.githubStatePath(), "pre_state=saved_value\n");
        auto state = mgr.parseState();
        ASSERT_EQ(state.at("pre_state"), "saved_value");

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("parseOutputs: empty file returns empty map", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        // File exists but empty (from setup)
        auto outputs = mgr.parseOutputs();
        ASSERT(outputs.empty());

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("cleanup removes protocol files", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup();

        std::string out_path = mgr.githubOutputPath();
        ASSERT(fs::exists(out_path));

        mgr.cleanup();
        ASSERT(!fs::exists(out_path));

        fs::remove_all(ws);
    });

    // ── New tests for GITHUB_EVENT_PATH ───────────────────────────────────

    run("setup with event payload creates GITHUB_EVENT_PATH file", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup(R"({"action":"opened","number":42})", "pull_request");

        std::string ep = mgr.githubEventPath();
        ASSERT(!ep.empty());
        ASSERT(fs::exists(ep));

        // Read back content
        std::ifstream f(ep);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        ASSERT(content.find("opened") != std::string::npos);
        ASSERT(content.find("42") != std::string::npos);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("currentEnv includes GITHUB_EVENT_PATH when payload provided", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup(R"({"event":"push"})", "push");

        auto env = mgr.currentEnv();
        bool found_path = false, found_name = false;
        for (auto& [k, v] : env) {
            if (k == "GITHUB_EVENT_PATH") { found_path = true; ASSERT(!v.empty()); }
            if (k == "GITHUB_EVENT_NAME" && v == "push") { found_name = true; }
        }
        ASSERT(found_path);
        ASSERT(found_name);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("currentEnv does NOT include GITHUB_EVENT_PATH when no payload", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup("", "");  // empty payload

        auto env = mgr.currentEnv();
        bool found_path = false;
        for (auto& [k, v] : env) {
            if (k == "GITHUB_EVENT_PATH") found_path = true;
        }
        ASSERT(!found_path);

        mgr.cleanup();
        fs::remove_all(ws);
    });

    run("cleanup removes GITHUB_EVENT_PATH file", []() {
        std::string ws = makeTempDir() + "/runner";
        EnvManager mgr(ws);
        mgr.setup(R"({"x":1})", "push");

        std::string ep = mgr.githubEventPath();
        ASSERT(fs::exists(ep));

        mgr.cleanup();
        ASSERT(!fs::exists(ep));

        fs::remove_all(ws);
    });

    return summary();
}
