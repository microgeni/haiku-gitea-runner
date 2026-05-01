// Config.cpp — YAML configuration implementation
#include "Config.h"

#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <cstdlib>

#ifdef __HAIKU__
#  include <FindDirectory.h>
#  include <Path.h>
#endif

namespace runner {

// ─── Helpers ──────────────────────────────────────────────────────────────

static LabelDef parseLabelDef(const std::string& raw) {
    // Format: "name:executor"  e.g. "haiku-latest:host"
    // If no colon present, default executor = "host"
    auto pos = raw.rfind(':');
    if (pos == std::string::npos) {
        return LabelDef{raw, "host"};
    }
    return LabelDef{raw.substr(0, pos), raw.substr(pos + 1)};
}

// ─── Config::labelStrings ─────────────────────────────────────────────────

std::vector<std::string> Config::labelStrings() const {
    std::vector<std::string> out;
    out.reserve(labels.size());
    for (auto& l : labels) {
        out.push_back(l.name + ":" + l.executor);
    }
    return out;
}

// ─── loadConfig ───────────────────────────────────────────────────────────

Config loadConfig(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse config file '" + path + "': " + e.what());
    }

    Config cfg;

    // Required
    if (!root["gitea_url"]) {
        throw std::runtime_error("Config missing required field: gitea_url");
    }
    cfg.gitea_url = root["gitea_url"].as<std::string>();

    // Strip trailing slash
    while (!cfg.gitea_url.empty() && cfg.gitea_url.back() == '/') {
        cfg.gitea_url.pop_back();
    }

    // Optional fields
    if (root["runner_token"]) cfg.runner_token = root["runner_token"].as<std::string>();
    if (root["name"])         cfg.name         = root["name"].as<std::string>();
    if (root["capacity"])     cfg.capacity     = root["capacity"].as<int>();
    if (root["fetch_timeout"])  cfg.fetch_timeout  = root["fetch_timeout"].as<int>();
    if (root["fetch_interval"]) cfg.fetch_interval = root["fetch_interval"].as<int>();
    if (root["insecure"])     cfg.insecure     = root["insecure"].as<bool>();
    if (root["log_level"])    cfg.log_level    = root["log_level"].as<std::string>();

    // Cache server
    if (root["cache"] && root["cache"].IsMap()) {
        const auto& c = root["cache"];
        if (c["enabled"])      cfg.cache_enabled      = c["enabled"].as<bool>();
        if (c["dir"])          cfg.cache_dir          = c["dir"].as<std::string>();
        if (c["host"])         cfg.cache_host         = c["host"].as<std::string>();
        if (c["port"])         cfg.cache_port         = c["port"].as<int>();
        if (c["max_age_days"]) cfg.cache_max_age_days = c["max_age_days"].as<int>();
    }

    // Labels
    if (root["labels"] && root["labels"].IsSequence()) {
        for (const auto& lbl : root["labels"]) {
            cfg.labels.push_back(parseLabelDef(lbl.as<std::string>()));
        }
    }

    // Default labels if none specified
    if (cfg.labels.empty()) {
        cfg.labels.push_back({"haiku", "host"});
        cfg.labels.push_back({"haiku-latest", "host"});
    }

    // Default capacity sanity
    if (cfg.capacity < 1)  cfg.capacity = 1;
    if (cfg.capacity > 16) cfg.capacity = 16;

    return cfg;
}

// ─── saveConfig ───────────────────────────────────────────────────────────

void saveConfig(const Config& cfg, const std::string& path) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "gitea_url"    << YAML::Value << cfg.gitea_url;
    out << YAML::Key << "runner_token" << YAML::Value << cfg.runner_token;
    out << YAML::Key << "name"         << YAML::Value << cfg.name;
    out << YAML::Key << "capacity"     << YAML::Value << cfg.capacity;
    out << YAML::Key << "fetch_timeout"  << YAML::Value << cfg.fetch_timeout;
    out << YAML::Key << "fetch_interval" << YAML::Value << cfg.fetch_interval;
    out << YAML::Key << "insecure"     << YAML::Value << cfg.insecure;
    out << YAML::Key << "log_level"    << YAML::Value << cfg.log_level;

    out << YAML::Key << "labels" << YAML::Value << YAML::BeginSeq;
    for (auto& l : cfg.labels) {
        out << (l.name + ":" + l.executor);
    }
    out << YAML::EndSeq;

    out << YAML::Key << "cache" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << cfg.cache_enabled;
    if (!cfg.cache_dir.empty())
        out << YAML::Key << "dir"     << YAML::Value << cfg.cache_dir;
    out << YAML::Key << "host"        << YAML::Value << cfg.cache_host;
    out << YAML::Key << "port"        << YAML::Value << cfg.cache_port;
    out << YAML::Key << "max_age_days" << YAML::Value << cfg.cache_max_age_days;
    out << YAML::EndMap;

    out << YAML::EndMap;

    // Create parent directories
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("Cannot write config to '" + path + "'");
    }
    f << out.c_str() << "\n";
}

// ─── defaultConfigPath ────────────────────────────────────────────────────

std::string defaultConfigPath() {
#ifdef __HAIKU__
    BPath settingsPath;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath) == B_OK) {
        settingsPath.Append("act_runner");
        settingsPath.Append("config.yaml");
        return settingsPath.Path();
    }
    // Fallback
    return std::string(getenv("HOME") ? getenv("HOME") : "/boot/home")
           + "/config/settings/act_runner/config.yaml";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/act_runner/config.yaml";
    }
    const char* home = getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/act_runner/config.yaml";
    }
    return "./config.yaml";
#endif
}

} // namespace runner
