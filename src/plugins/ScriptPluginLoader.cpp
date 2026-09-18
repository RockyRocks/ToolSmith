#include <plugins/ScriptPluginLoader.h>
#include <plugins/ScriptPluginAdapter.h>
#include <plugins/StdioMCPAdapter.h>
#include <core/Logger.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

ScriptPluginLoader::~ScriptPluginLoader() {
    StopWatcher();
}

void ScriptPluginLoader::SetNotifyCallback(
    std::function<void(const nlohmann::json&)> cb) {
    std::lock_guard<std::mutex> lock(m_NotifyMutex);
    m_NotifyCallback = std::move(cb);
}

void ScriptPluginLoader::FireNotification(const nlohmann::json& payload) {
    Logger::GetInstance().Log("[ScriptPlugin] plugin reloaded: " + payload.dump());
    std::lock_guard<std::mutex> lock(m_NotifyMutex);
    if (m_NotifyCallback) {
        try { m_NotifyCallback(payload); } catch (...) {}
    }
}

void ScriptPluginLoader::StartWatcher(const std::string& pluginsDir,
                                       std::shared_ptr<CommandRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_WatcherMutex);
    if (m_WatcherThread.joinable()) return;

    m_WatcherStop = false;

    m_WatcherThread = std::thread([this, pluginsDir, registry]() {
        std::unordered_map<std::string, fs::file_time_type> mtimeMap;

        fs::path root(pluginsDir);
        std::error_code ec;
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_directory()) continue;
                fs::path jsonPath = entry.path() / "plugin.json";
                if (!fs::exists(jsonPath, ec)) continue;
                auto lwt = fs::last_write_time(jsonPath, ec);
                if (!ec) mtimeMap[jsonPath.string()] = lwt;
            }
        }

        while (!m_WatcherStop.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kWatchIntervalMs));

            if (m_WatcherStop.load()) break;
            if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_directory()) continue;
                fs::path jsonPath = entry.path() / "plugin.json";
                if (!fs::exists(jsonPath, ec)) continue;

                auto lwt = fs::last_write_time(jsonPath, ec);
                if (ec) continue;

                auto it = mtimeMap.find(jsonPath.string());
                if (it != mtimeMap.end() && it->second == lwt) continue;

                mtimeMap[jsonPath.string()] = lwt;

                try {
                    std::ifstream f(jsonPath);
                    if (!f.is_open()) continue;
                    std::ostringstream ss;
                    ss << f.rdbuf();
                    auto pluginJson = nlohmann::json::parse(ss.str());
                    if (!pluginJson.contains("runtime")) continue;

                    std::string runtime    = pluginJson["runtime"].get<std::string>();
                    std::string entrypoint = pluginJson.value("entrypoint", "");
                    std::string name       = pluginJson.value("name",
                                               entry.path().filename().string());
                    if (runtime.empty() || entrypoint.empty()) continue;

                    fs::path absEntrypoint = (entry.path() / entrypoint).lexically_normal();
                    if (!fs::exists(absEntrypoint)) continue;

                    Logger::GetInstance().Log(
                        "[ScriptPlugin] watcher: reloading " + name);

                    nlohmann::json toolNames = nlohmann::json::array();
                    int reloaded = 0;

                    if (runtime == "mcp-stdio") {
                        std::string cmd = ScriptPluginAdapter::GetRuntimeExecutable(
                            pluginJson.value("command_runtime", "python"));
                        std::vector<std::string> spawnArgs;
                        spawnArgs.push_back(absEntrypoint.string());
                        if (pluginJson.contains("args") && pluginJson["args"].is_array()) {
                            for (const auto& a : pluginJson["args"])
                                if (a.is_string()) spawnArgs.push_back(a.get<std::string>());
                        }
                        auto tools = StdioMCPAdapter::DiscoverTools(name, cmd, spawnArgs);
                        for (const auto& tool : tools) {
                            registry->ReplaceCommand(
                                tool.m_Name,
                                std::make_shared<StdioMCPAdapter>(
                                    name, cmd, spawnArgs,
                                    tool.m_Name, tool.m_Description, tool.m_InputSchema));
                            toolNames.push_back(tool.m_Name);
                            ++reloaded;
                        }
                    } else {
                        auto tools = ScriptPluginAdapter::DiscoverTools(
                            name, runtime, absEntrypoint.string());
                        for (const auto& tool : tools) {
                            registry->ReplaceCommand(
                                tool.m_Name,
                                std::make_shared<ScriptPluginAdapter>(
                                    name, runtime, absEntrypoint.string(), tool));
                            toolNames.push_back(tool.m_Name);
                            ++reloaded;
                        }
                    }

                    if (reloaded > 0) {
                        FireNotification({
                            {"event",  "script_plugin_reloaded"},
                            {"plugin", name},
                            {"tools",  toolNames}
                        });
                    }
                } catch (const std::exception& e) {
                    Logger::GetInstance().Log(
                        "[ScriptPlugin] watcher error: " + std::string(e.what()));
                }
            }
        }

        Logger::GetInstance().Log("[ScriptPlugin] watcher stopped");
    });
}

void ScriptPluginLoader::StopWatcher() {
    m_WatcherStop = true;
    std::lock_guard<std::mutex> lock(m_WatcherMutex);
    if (m_WatcherThread.joinable()) {
        m_WatcherThread.join();
    }
}

void ScriptPluginLoader::LoadAll(const std::string& pluginsDir,
                                  CommandRegistry& registry)
{
    if (!fs::exists(pluginsDir) || !fs::is_directory(pluginsDir)) {
        Logger::GetInstance().Log(
            "[ScriptPlugin] Plugins directory not found: " + pluginsDir + " (skipping)");
        return;
    }

    std::error_code rootEc;
    fs::path canonRoot = fs::canonical(fs::path(pluginsDir), rootEc);
    if (rootEc) {
        Logger::GetInstance().Log(
            "[ScriptPlugin] Cannot resolve plugins root: " + rootEc.message());
        return;
    }

    // Phase 1: Collect valid plugin descriptors
    struct PluginDesc {
        std::string name;
        std::string runtime;
        std::string entrypoint;     // absolute path
        std::string command;        // for mcp-stdio: resolved executable
        std::vector<std::string> spawnArgs; // for mcp-stdio
        bool isMcpStdio = false;
    };

    std::vector<PluginDesc> descs;

    for (const auto& entry : fs::directory_iterator(pluginsDir)) {
        if (!entry.is_directory()) continue;

        fs::path pluginDir = entry.path();
        fs::path jsonPath  = pluginDir / "plugin.json";
        if (!fs::exists(jsonPath)) continue;

        try {
            std::ifstream f(jsonPath);
            if (!f.is_open()) {
                Logger::GetInstance().Log(
                    "[ScriptPlugin] Cannot open " + jsonPath.string() + " (skipping)");
                continue;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            auto pluginJson = nlohmann::json::parse(ss.str());

            if (!pluginJson.contains("runtime")) continue;

            std::string runtime    = pluginJson["runtime"].get<std::string>();
            std::string entrypoint = pluginJson.value("entrypoint", "");
            std::string name       = pluginJson.value("name",
                                       pluginDir.filename().string());

            if (runtime.empty() || entrypoint.empty()) {
                Logger::GetInstance().Log(
                    "[ScriptPlugin] Missing runtime or entrypoint in "
                    + jsonPath.string() + " (skipping)");
                continue;
            }

            fs::path absEntrypoint = (pluginDir / entrypoint).lexically_normal();
            if (!fs::exists(absEntrypoint)) {
                Logger::GetInstance().Log(
                    "[ScriptPlugin] Entrypoint not found: " + absEntrypoint.string()
                    + " (skipping)");
                continue;
            }

            std::error_code epEc;
            fs::path canonEntrypoint = fs::canonical(absEntrypoint, epEc);
            if (epEc || canonEntrypoint.string().rfind(canonRoot.string(), 0) != 0) {
                Logger::GetInstance().Log(
                    "[ScriptPlugin] Path traversal blocked: " + absEntrypoint.string()
                    + " (skipping)");
                continue;
            }

            PluginDesc desc;
            desc.name       = name;
            desc.runtime    = runtime;
            desc.entrypoint = canonEntrypoint.string();

            if (runtime == "mcp-stdio") {
                desc.isMcpStdio = true;
                desc.command = ScriptPluginAdapter::GetRuntimeExecutable(
                    pluginJson.value("command_runtime", "python"));
                desc.spawnArgs.push_back(desc.entrypoint);
                if (pluginJson.contains("args") && pluginJson["args"].is_array()) {
                    for (const auto& a : pluginJson["args"])
                        if (a.is_string()) desc.spawnArgs.push_back(a.get<std::string>());
                }
            }

            descs.push_back(std::move(desc));

        } catch (const std::exception& e) {
            Logger::GetInstance().Log(
                "[ScriptPlugin] Error reading " + pluginDir.string()
                + ": " + e.what());
        }
    }

    if (descs.empty()) {
        Logger::GetInstance().Log("[ScriptPlugin] Loaded 0 tool(s)");
        return;
    }

    // Phase 2: Discover tools in parallel
    struct DiscoveryResult {
        size_t descIndex;
        std::vector<ScriptPluginToolInfo> tools;
    };

    std::vector<std::future<DiscoveryResult>> futures;
    futures.reserve(descs.size());

    for (size_t i = 0; i < descs.size(); ++i) {
        const auto& d = descs[i];
        if (d.isMcpStdio) {
            futures.push_back(std::async(std::launch::async,
                [i, name = d.name, cmd = d.command, args = d.spawnArgs]() -> DiscoveryResult {
                    return {i, StdioMCPAdapter::DiscoverTools(name, cmd, args)};
                }));
        } else {
            futures.push_back(std::async(std::launch::async,
                [i, name = d.name, runtime = d.runtime, ep = d.entrypoint]() -> DiscoveryResult {
                    return {i, ScriptPluginAdapter::DiscoverTools(name, runtime, ep)};
                }));
        }
    }

    // Phase 3: Collect results and register tools
    int loaded = 0;
    for (auto& fut : futures) {
        try {
            auto result = fut.get();
            const auto& desc = descs[result.descIndex];

            if (result.tools.empty()) {
                Logger::GetInstance().Log(
                    "[ScriptPlugin] No tools discovered from " + desc.name + " (skipping)");
                continue;
            }

            for (const auto& tool : result.tools) {
                if (desc.isMcpStdio) {
                    registry.RegisterCommand(
                        tool.m_Name,
                        std::make_shared<StdioMCPAdapter>(
                            desc.name, desc.command, desc.spawnArgs,
                            tool.m_Name, tool.m_Description, tool.m_InputSchema));
                    Logger::GetInstance().Log(
                        "[StdioMCP] Registered tool '" + tool.m_Name
                        + "' from plugin '" + desc.name + "'");
                } else {
                    registry.RegisterCommand(
                        tool.m_Name,
                        std::make_shared<ScriptPluginAdapter>(
                            desc.name, desc.runtime, desc.entrypoint, tool));
                    Logger::GetInstance().Log(
                        "[ScriptPlugin] Registered tool '" + tool.m_Name
                        + "' from plugin '" + desc.name + "'");
                }
                ++loaded;
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Log(
                "[ScriptPlugin] Discovery failed: " + std::string(e.what()));
        }
    }

    Logger::GetInstance().Log(
        "[ScriptPlugin] Loaded " + std::to_string(loaded) + " tool(s)");
}
