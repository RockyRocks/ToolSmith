#pragma once

#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

class CommandRegistry;
class SkillEngine;
class McpServerRegistry;
struct CoreRuntime;

/// Implements MCP protocol (JSON-RPC 2.0) over stdio transport.
/// Reads newline-delimited JSON-RPC messages from an input stream,
/// dispatches to existing CommandRegistry/SkillEngine, and writes
/// responses to an output stream.
class StdioTransport {
public:
    StdioTransport(
        std::shared_ptr<CommandRegistry> registry,
        std::shared_ptr<SkillEngine> skillEngine,
        std::shared_ptr<McpServerRegistry> mcpRegistry,
        std::istream& input = std::cin,
        std::ostream& output = std::cout,
        const std::string& serverName = "toolsmith",
        const std::string& serverVersion = "1.0.0"
    );

    /// Optional core runtime for skill:// resources and advertised-pack checks.
    void SetCoreRuntime(std::shared_ptr<CoreRuntime> runtime);

    /// Blocking read loop. Returns when input stream reaches EOF or Stop() is called.
    void Run();

    /// Signal the transport to stop after the current message.
    void Stop();

    /// Push an unsolicited JSON-RPC notification to the output stream.
    /// Thread-safe. Called by NativePluginLoader when a plugin loads at runtime
    /// so that connected LLM clients receive notifications/tools/list_changed.
    void PushNotification(const nlohmann::json& notification);

private:
    // JSON-RPC dispatch
    nlohmann::json Dispatch(const nlohmann::json& message);

    // MCP method handlers
    nlohmann::json HandleInitialize(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleToolsList(const nlohmann::json& id);
    nlohmann::json HandleToolsCall(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleToolsCallBatch(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandlePromptsList(const nlohmann::json& id);
    nlohmann::json HandlePromptsGet(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleResourcesList(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleResourcesRead(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleResourcesSubscribe(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleResourcesUnsubscribe(const nlohmann::json& params, const nlohmann::json& id);
    void HandleCancelNotification(const nlohmann::json& params);

    void StartResourceWatcher();
    void StopResourceWatcher();

    // JSON-RPC helpers
    nlohmann::json MakeResponse(const nlohmann::json& id, const nlohmann::json& result);
    nlohmann::json MakeError(const nlohmann::json& id, int code,
                             const std::string& message,
                             const nlohmann::json& data = nullptr);
    void SendMessage(const nlohmann::json& msg);

    // Tool metadata
    struct ToolMeta {
        std::string m_Name;
        std::string m_Description;
        nlohmann::json m_InputSchema;
    };
    std::vector<ToolMeta> BuildToolList() const;

    std::shared_ptr<CommandRegistry> m_Registry;
    std::shared_ptr<SkillEngine> m_SkillEngine;
    std::shared_ptr<McpServerRegistry> m_McpRegistry;
    std::shared_ptr<CoreRuntime> m_CoreRuntime;
    std::istream& m_Input;
    std::ostream& m_Output;
    std::string m_ServerName;
    std::string m_ServerVersion;
    std::string m_ResourceRoot;
    bool m_Initialized = false;
    std::atomic<bool> m_Running{false};
    std::mutex m_WriteMutex;

    std::mutex m_InFlightMutex;
    std::map<std::string, std::string> m_InFlightRequests;

    std::mutex m_SubscriptionMutex;
    std::set<std::string> m_SubscribedUris;
    std::map<std::string, std::filesystem::file_time_type> m_SubscribedMtimes;
    std::atomic<bool> m_ResourceWatcherStop{false};
    std::thread m_ResourceWatcherThread;
    static constexpr int kResourceWatchIntervalMs = 2000;
};
