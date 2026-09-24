#include <plugins/NativePluginAdapter.h>
#include <commands/ToolMetadata.h>
#include <core/Logger.h>
#include <future>
#include <chrono>

NativePluginAdapter::NativePluginAdapter(std::shared_ptr<IPlugin> plugin,
                                         std::string toolName,
                                         std::string description,
                                         nlohmann::json inputSchema,
                                         int timeoutSeconds)
    : m_Plugin(std::move(plugin))
    , m_ToolName(std::move(toolName))
    , m_Description(std::move(description))
    , m_InputSchema(std::move(inputSchema))
    , m_TimeoutSeconds(timeoutSeconds)
{}

void NativePluginAdapter::Cancel(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(m_CancelMutex);
    m_CancelledRequests.insert(requestId);
}

void NativePluginAdapter::Shutdown() {
    m_ShutdownRequested.store(true);
}

std::future<nlohmann::json> NativePluginAdapter::ExecuteAsync(
    const nlohmann::json& request)
{
    auto self = shared_from_this();
    std::string requestId = request.value("_requestId", "");

    return std::async(std::launch::deferred, [self, request, requestId]() -> nlohmann::json {
        if (self->m_ShutdownRequested.load()) {
            return {
                {"isError", true},
                {"content", {{{"type","text"},
                              {"text","Plugin tool '" + self->m_ToolName
                                      + "' is shutting down"}}}}
            };
        }

        if (self->IsDisabled()) {
            return {
                {"isError", true},
                {"content", {{{"type","text"},
                              {"text","Plugin tool '" + self->m_ToolName
                                      + "' is disabled after "
                                      + std::to_string(kMaxFaults)
                                      + " consecutive faults"}}}}
            };
        }

        int active = self->m_ActiveThreads.load();
        if (active >= kMaxConcurrentCalls) {
            Logger::GetInstance().Log(
                "[NativePlugin] '" + self->m_ToolName + "' rejected: "
                + std::to_string(active) + " zombie threads outstanding");
            return {
                {"isError", true},
                {"content", {{{"type","text"},
                              {"text","Plugin tool '" + self->m_ToolName
                                      + "' has too many outstanding calls ("
                                      + std::to_string(active) + ")"}}}}
            };
        }

        ++self->m_ActiveThreads;
        auto started = std::chrono::steady_clock::now();
        nlohmann::json executed;
        try {
            executed = self->m_Plugin->Execute(self->m_ToolName, request);
        } catch (const std::exception& ex) {
            --self->m_ActiveThreads;
            int faults = ++self->m_FaultCount;
            Logger::GetInstance().Log(
                "[NativePlugin] exception in '" + self->m_ToolName
                + "': " + ex.what()
                + " (fault " + std::to_string(faults) + "/"
                + std::to_string(kMaxFaults) + ")");
            return {
                {"isError", true},
                {"content", {{{"type","text"},
                              {"text","Plugin tool '" + self->m_ToolName
                                      + "' threw: " + ex.what()}}}}
            };
        } catch (...) {
            --self->m_ActiveThreads;
            int faults = ++self->m_FaultCount;
            Logger::GetInstance().Log(
                "[NativePlugin] unknown exception in '" + self->m_ToolName
                + "' (fault " + std::to_string(faults) + "/"
                + std::to_string(kMaxFaults) + ")");
            return {
                {"isError", true},
                {"content", {{{"type","text"},
                              {"text","Plugin tool '" + self->m_ToolName
                                      + "' threw an unknown exception"}}}}
            };
        }
        --self->m_ActiveThreads;

        if (!requestId.empty()) {
            std::lock_guard<std::mutex> lock(self->m_CancelMutex);
            if (self->m_CancelledRequests.erase(requestId) > 0) {
                return {
                    {"isError", true},
                    {"content", {{{"type","text"},
                                  {"text","Plugin tool '" + self->m_ToolName
                                          + "' was cancelled"}}}}
                };
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed > std::chrono::seconds(self->m_TimeoutSeconds)) {
            int faults = ++self->m_FaultCount;
            Logger::GetInstance().Log(
                "[NativePlugin] timeout executing '" + self->m_ToolName
                + "' (fault " + std::to_string(faults) + "/"
                + std::to_string(kMaxFaults) + ")");
            return {
                {"isError", true},
                {"content", {{{"type","text"},
                              {"text","Plugin tool '" + self->m_ToolName
                                      + "' timed out after "
                                      + std::to_string(self->m_TimeoutSeconds)
                                      + "s"}}}}
            };
        }

        self->m_FaultCount.store(0);
        return executed;
    });
}

ToolMetadata NativePluginAdapter::GetMetadata() const {
    ToolMetadata meta;
    meta.m_Name        = m_ToolName;
    meta.m_Description = m_Description;
    meta.m_InputSchema = m_InputSchema;
    meta.m_Source      = ToolSource::NativePlugin;
    meta.m_Hidden      = false;
    return meta;
}
