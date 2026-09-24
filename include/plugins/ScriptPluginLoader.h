#pragma once
#include <commands/CommandRegistry.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

class ScriptPluginLoader {
public:
    static constexpr int kWatchIntervalMs = 3000;

    ScriptPluginLoader() = default;
    ~ScriptPluginLoader();

    ScriptPluginLoader(const ScriptPluginLoader&) = delete;
    ScriptPluginLoader& operator=(const ScriptPluginLoader&) = delete;

    void LoadAll(const std::string& pluginsDir, CommandRegistry& registry);

    /// When set, only plugin.json packs in this set are discovered.
    void SetActivePacks(std::unordered_set<std::string> packs);

    void SetNotifyCallback(std::function<void(const nlohmann::json&)> cb);

    void StartWatcher(const std::string& pluginsDir,
                      std::shared_ptr<CommandRegistry> registry);

    void StopWatcher();

private:
    void FireNotification(const nlohmann::json& payload);

    std::unordered_set<std::string>            m_ActivePacks;
    bool                                       m_GatePacks = false;
    std::function<void(const nlohmann::json&)> m_NotifyCallback;
    std::mutex                                 m_NotifyMutex;

    std::atomic<bool>  m_WatcherStop{false};
    std::thread        m_WatcherThread;
    std::mutex         m_WatcherMutex;
};
