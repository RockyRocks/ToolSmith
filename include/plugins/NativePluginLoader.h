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

class NativePluginLoader {
public:
    static constexpr int kWatchIntervalMs = 2000;

    NativePluginLoader() = default;
    ~NativePluginLoader();

    NativePluginLoader(const NativePluginLoader&) = delete;
    NativePluginLoader& operator=(const NativePluginLoader&) = delete;

    void SetNotifyCallback(std::function<void(const nlohmann::json&)> cb);

    /// When non-empty, only plugin.json packs that intersect this set are loaded.
    void SetActivePacks(std::unordered_set<std::string> packs);

    void LoadAll(const std::string& pluginsDir,
                 CommandRegistry& registry);

    bool LoadOne(const std::string& dlPath,
                 CommandRegistry& registry,
                 const std::string& source = "startup");

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
