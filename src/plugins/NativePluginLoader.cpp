#include <plugins/NativePluginLoader.h>
#include <plugins/DlPlugin.h>
#include <plugins/NativePluginAdapter.h>
#include <core/Logger.h>

#include <exception>
#include <filesystem>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

bool IsPluginBinary(const fs::path& p) {
    auto ext = p.extension().string();
#ifdef _WIN32
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib" || ext == ".so";
#else
    return ext == ".so";
#endif
}

} // anonymous namespace

NativePluginLoader::~NativePluginLoader() {
    StopWatcher();
}

void NativePluginLoader::SetNotifyCallback(
    std::function<void(const nlohmann::json&)> cb)
{
    std::lock_guard<std::mutex> lock(m_NotifyMutex);
    m_NotifyCallback = std::move(cb);
}

void NativePluginLoader::FireNotification(const nlohmann::json& payload) {
    Logger::GetInstance().Log("[NativePlugin] plugin loaded: " + payload.dump());
    std::lock_guard<std::mutex> lock(m_NotifyMutex);
    if (m_NotifyCallback) {
        try { m_NotifyCallback(payload); } catch (...) {}
    }
}

bool NativePluginLoader::LoadOne(const std::string& dlPath,
                                 CommandRegistry& registry,
                                 const std::string& source)
{
    auto plugin = DlPlugin::Load(dlPath);
    if (!plugin) {
        return false;
    }

    auto tools = plugin->ListTools();
    if (tools.empty()) {
        Logger::GetInstance().Log("[NativePlugin] '" + dlPath
                                  + "' loaded but exposes no tools — skipping");
        return false;
    }

    auto sharedPlugin = std::shared_ptr<IPlugin>(std::move(plugin));

    nlohmann::json toolNames = nlohmann::json::array();
    int registered = 0;
    for (const auto& toolInfo : tools) {
        if (toolInfo.m_Name.empty()) continue;

        auto adapter = std::make_shared<NativePluginAdapter>(
            sharedPlugin,
            toolInfo.m_Name,
            toolInfo.m_Description,
            toolInfo.m_InputSchema);

        registry.RegisterCommand(toolInfo.m_Name, adapter);
        toolNames.push_back(toolInfo.m_Name);
        ++registered;
    }

    if (registered == 0) {
        return false;
    }

    nlohmann::json notification = {
        {"event",  "plugin_loaded"},
        {"plugin", {
            {"name",        sharedPlugin->GetName()},
            {"description", sharedPlugin->GetDescription()},
            {"version",     sharedPlugin->GetVersion()}
        }},
        {"tools",  toolNames},
        {"source", source}
    };
    FireNotification(notification);
    return true;
}

void NativePluginLoader::LoadAll(const std::string& pluginsDir,
                                 CommandRegistry& registry)
{
    fs::path root(pluginsDir);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        Logger::GetInstance().Log("[NativePlugin] plugins directory '"
                                  + pluginsDir + "' not found — skipping");
        return;
    }

    std::error_code ec;
    fs::path canonRoot = fs::canonical(root, ec);
    if (ec) {
        Logger::GetInstance().Log("[NativePlugin] cannot resolve plugins root: " + ec.message());
        return;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;

        fs::path binDir = entry.path() / "bin";
        if (!fs::exists(binDir) || !fs::is_directory(binDir)) continue;

        for (const auto& binEntry : fs::directory_iterator(binDir, ec)) {
            if (!binEntry.is_regular_file()) continue;
            if (!IsPluginBinary(binEntry.path())) continue;

            std::error_code ec2;
            fs::path canonBin = fs::canonical(binEntry.path(), ec2);
            if (ec2 || canonBin.string().rfind(canonRoot.string(), 0) != 0) {
                Logger::GetInstance().Log(
                    "[NativePlugin] path traversal blocked: " + binEntry.path().string());
                continue;
            }

            LoadOne(canonBin.string(), registry, "startup");
        }
    }
}

void NativePluginLoader::StartWatcher(const std::string& pluginsDir,
                                      std::shared_ptr<CommandRegistry> registry)
{
    std::lock_guard<std::mutex> lock(m_WatcherMutex);
    if (m_WatcherThread.joinable()) {
        return;
    }

    m_WatcherStop = false;

    m_WatcherThread = std::thread([this, pluginsDir, registry]() {
        std::unordered_set<std::string> loaded;

        fs::path root(pluginsDir);
        std::error_code ec;
        fs::path canonRoot = fs::canonical(root, ec);
        if (ec) {
            Logger::GetInstance().Log("[NativePlugin] watcher: cannot resolve root: " + ec.message());
            return;
        }
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_directory()) continue;
                fs::path binDir = entry.path() / "bin";
                if (!fs::exists(binDir, ec)) continue;
                for (const auto& binEntry : fs::directory_iterator(binDir, ec)) {
                    if (binEntry.is_regular_file()
                        && IsPluginBinary(binEntry.path()))
                    {
                        loaded.insert(binEntry.path().string());
                    }
                }
            }
        }

        while (!m_WatcherStop.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kWatchIntervalMs));

            if (m_WatcherStop.load()) break;
            if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_directory()) continue;
                fs::path binDir = entry.path() / "bin";
                if (!fs::exists(binDir, ec)) continue;

                for (const auto& binEntry : fs::directory_iterator(binDir, ec)) {
                    if (!binEntry.is_regular_file()) continue;
                    if (!IsPluginBinary(binEntry.path())) continue;

                    std::error_code ec3;
                    fs::path canonBin = fs::canonical(binEntry.path(), ec3);
                    if (ec3 || canonBin.string().rfind(canonRoot.string(), 0) != 0) {
                        Logger::GetInstance().Log(
                            "[NativePlugin] watcher: path traversal blocked: "
                            + binEntry.path().string());
                        continue;
                    }

                    std::string path = canonBin.string();
                    if (loaded.count(path)) continue;

                    Logger::GetInstance().Log(
                        "[NativePlugin] watcher detected new plugin: " + path);
                    try {
                        if (LoadOne(path, *registry, "runtime")) {
                            loaded.insert(path);
                        }
                    } catch (const std::exception& e) {
                        Logger::GetInstance().Log(
                            std::string("[NativePlugin] watcher load failed: ") + e.what());
                    }
                }
            }
        }

        Logger::GetInstance().Log("[NativePlugin] watcher stopped");
    });
}

void NativePluginLoader::StopWatcher() {
    m_WatcherStop = true;
    std::lock_guard<std::mutex> lock(m_WatcherMutex);
    if (m_WatcherThread.joinable()) {
        m_WatcherThread.join();
    }
}
