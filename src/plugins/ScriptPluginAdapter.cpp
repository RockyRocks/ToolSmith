#include <plugins/ScriptPluginAdapter.h>
#include <commands/ToolMetadata.h>
#include <core/Logger.h>

#include <atomic>
#include <chrono>
#include <cstdio>       // popen/_popen, fgets, pclose/_pclose
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <process.h>    // _getpid
#else
#include <unistd.h>     // getpid
#include <sys/stat.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// RAII guard that removes a temp file on destruction (never throws).
///
/// Rule of Five: user-declared destructor → must explicitly handle all four
/// remaining specials.  This guard is always a stack-local; copying would
/// cause a double-remove and moving is unnecessary, so both are deleted.
/// The explicit single-argument constructor makes initialization unambiguous
/// and satisfies MSVC's stricter aggregate rules (which don't recognise
/// deleted constructors as "non-user-provided" in all configurations).
struct TempFileGuard {
    fs::path path;

    explicit TempFileGuard(fs::path p) noexcept : path(std::move(p)) {}
    ~TempFileGuard() {
        std::error_code ec;
        fs::remove(path, ec);  // error_code overload: never throws
    }
    TempFileGuard(const TempFileGuard&)            = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    TempFileGuard(TempFileGuard&&)                 = delete;
    TempFileGuard& operator=(TempFileGuard&&)      = delete;
};

/// Build a canonical JSON error response matching NativePluginAdapter's format.
nlohmann::json ErrorResponse(const std::string& msg) {
    return {
        {"isError", true},
        {"content", {{{"type", "text"}, {"text", msg}}}}
    };
}

/// Run a command via popen, read stdout up to maxBytes, return output string.
/// Returns empty string on pipe open failure; logs the error.
std::string RunCommand(const std::string& cmd, size_t maxBytes) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        Logger::GetInstance().Log("[ScriptPlugin] Failed to spawn: " + cmd);
        return "";
    }

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
        if (output.size() >= maxBytes) {
            output.resize(maxBytes);
            Logger::GetInstance().Log(
                "[ScriptPlugin] Output truncated at " + std::to_string(maxBytes) + " bytes");
            break;
        }
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

/// Strip trailing whitespace and newline characters from str in-place.
void TrimRight(std::string& str) {
    while (!str.empty() && (str.back() == '\n' || str.back() == '\r'
                             || str.back() == ' ' || str.back() == '\t'))
        str.pop_back();
}

/// Write content to a file with owner-only permissions.
/// Returns true on success.
bool WriteSecureTempFile(const fs::path& path, const std::string& content) {
#ifdef _WIN32
    // CreateFileW with no sharing + write DACL after creation
    HANDLE h = CreateFileW(path.wstring().c_str(),
                           GENERIC_WRITE, 0 /*no sharing*/,
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(),
                        static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(h);
    return ok && (written == static_cast<DWORD>(content.size()));
#else
    // Open with 0600 permissions (owner read/write only)
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    auto remaining = content.size();
    auto ptr = content.data();
    while (remaining > 0) {
        auto n = write(fd, ptr, remaining);
        if (n <= 0) { close(fd); return false; }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    close(fd);
    return true;
#endif
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// ScriptPluginAdapter — static helpers
// ---------------------------------------------------------------------------

std::string ScriptPluginAdapter::GetRuntimeExecutable(const std::string& runtime) {
    if (runtime == "python") {
#ifdef _WIN32
        return "python";
#else
        return "python3";
#endif
    }
    if (runtime == "node")   return "node";
    if (runtime == "dotnet") return "dotnet";
    // "executable" → caller handles it (no exe prefix)
    // anything else → use as-is (e.g. "python3", "node20")
    return runtime;
}

bool ScriptPluginAdapter::IsValidToolName(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

std::string ScriptPluginAdapter::BuildListCommand(const std::string& runtime,
                                                   const std::string& entrypoint) {
    if (runtime == "executable")
        return "\"" + entrypoint + "\" --mcp-list";
    return GetRuntimeExecutable(runtime) + " \"" + entrypoint + "\" --mcp-list";
}

std::string ScriptPluginAdapter::BuildCallCommand(const std::string& runtime,
                                                   const std::string& entrypoint,
                                                   const std::string& toolName,
                                                   const std::string& argsFilePath) {
    // toolName has already been validated — safe to embed unquoted
    std::string prefix = (runtime == "executable")
        ? "\"" + entrypoint + "\""
        : GetRuntimeExecutable(runtime) + " \"" + entrypoint + "\"";
    return prefix + " --mcp-call " + toolName
           + " --mcp-args-file \"" + argsFilePath + "\"";
}

// ---------------------------------------------------------------------------
// ScriptPluginAdapter — DiscoverTools
// ---------------------------------------------------------------------------

std::vector<ScriptPluginToolInfo> ScriptPluginAdapter::DiscoverTools(
    const std::string& pluginName,
    const std::string& runtime,
    const std::string& entrypoint)
{
    std::vector<ScriptPluginToolInfo> result;
    try {
        std::string cmd = BuildListCommand(runtime, entrypoint);
        std::string output = RunCommand(cmd, kMaxOutputBytes);
        if (output.empty()) {
            Logger::GetInstance().Log(
                "[ScriptPlugin] " + pluginName + ": --mcp-list produced no output");
            return {};
        }

        TrimRight(output);
        auto arr = nlohmann::json::parse(output);
        if (!arr.is_array()) {
            Logger::GetInstance().Log(
                "[ScriptPlugin] " + pluginName + ": --mcp-list output is not a JSON array");
            return {};
        }

        for (const auto& item : arr) {
            if (!item.contains("name") || !item["name"].is_string()) continue;
            std::string name = item["name"].get<std::string>();
            if (!IsValidToolName(name)) {
                Logger::GetInstance().Log(
                    "[ScriptPlugin] " + pluginName
                    + ": skipping tool with invalid name \"" + name + "\"");
                continue;
            }
            ScriptPluginToolInfo info;
            info.m_Name        = name;
            info.m_Description = item.value("description", "");
            if (item.contains("inputSchema") && item["inputSchema"].is_object())
                info.m_InputSchema = item["inputSchema"];
            result.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Log(
            "[ScriptPlugin] " + pluginName + ": DiscoverTools failed: " + e.what());
        return {};
    }
    return result;
}

// ---------------------------------------------------------------------------
// ScriptPluginAdapter — constructor, GetMetadata, ExecuteAsync
// ---------------------------------------------------------------------------

ScriptPluginAdapter::ScriptPluginAdapter(std::string pluginName,
                                          std::string runtime,
                                          std::string entrypoint,
                                          ScriptPluginToolInfo toolInfo)
    : m_PluginName(std::move(pluginName))
    , m_Runtime(std::move(runtime))
    , m_Entrypoint(std::move(entrypoint))
    , m_ToolInfo(std::move(toolInfo))
{}

ToolMetadata ScriptPluginAdapter::GetMetadata() const {
    ToolMetadata meta;
    meta.m_Name        = m_ToolInfo.m_Name;
    meta.m_Description = m_ToolInfo.m_Description;
    meta.m_InputSchema = (m_ToolInfo.m_InputSchema.is_null() || m_ToolInfo.m_InputSchema.empty())
        ? nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}}
        : m_ToolInfo.m_InputSchema;
    meta.m_Source      = ToolSource::ScriptPlugin;
    meta.m_Hidden      = false;
    return meta;
}

void ScriptPluginAdapter::Cancel(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(m_CancelState->mutex);
    m_CancelState->cancelledRequests.insert(requestId);
}

std::future<nlohmann::json> ScriptPluginAdapter::ExecuteAsync(const nlohmann::json& request) {
    auto runtime    = m_Runtime;
    auto entrypoint = m_Entrypoint;
    auto toolName   = m_ToolInfo.m_Name;
    std::string requestId = request.value("_requestId", "");
    auto cancelState = m_CancelState;

    return std::async(std::launch::deferred, [runtime, entrypoint, toolName, request,
                                           requestId, cancelState]()
        -> nlohmann::json
    {
        nlohmann::json args = request.value("payload",
                              request.value("arguments", nlohmann::json::object()));

        static std::atomic<uint64_t> s_Counter{0};
        uint64_t n = s_Counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        int pid = _getpid();
#else
        int pid = getpid();
#endif
        fs::path tmpPath = fs::temp_directory_path()
            / ("mcp_script_" + std::to_string(n) + "_" + std::to_string(pid) + ".json");

        TempFileGuard guard{tmpPath};

        if (!WriteSecureTempFile(tmpPath, args.dump()))
            return ErrorResponse("Failed to create temp args file: " + tmpPath.string());

        std::string cmd = BuildCallCommand(runtime, entrypoint, toolName, tmpPath.string());

        // Run subprocess in a detached thread with timeout to prevent thread starvation
        auto promise = std::make_shared<std::promise<std::string>>();
        std::future<std::string> outputFuture = promise->get_future();

        std::thread([cmd, promise]() {
            try {
                promise->set_value(RunCommand(cmd, kMaxOutputBytes));
            } catch (...) {
                try { promise->set_exception(std::current_exception()); } catch (...) {}
            }
        }).detach();

        auto status = outputFuture.wait_for(std::chrono::seconds(kTimeoutSeconds));

        if (!requestId.empty()) {
            std::lock_guard<std::mutex> lock(cancelState->mutex);
            if (cancelState->cancelledRequests.erase(requestId) > 0) {
                return ErrorResponse("Script tool '" + toolName + "' was cancelled");
            }
        }

        if (status == std::future_status::timeout) {
            Logger::GetInstance().Log(
                "[ScriptPlugin] timeout executing '" + toolName
                + "' after " + std::to_string(kTimeoutSeconds) + "s");
            return ErrorResponse(
                "Script tool '" + toolName + "' timed out after "
                + std::to_string(kTimeoutSeconds) + "s");
        }

        std::string output;
        try {
            output = outputFuture.get();
        } catch (const std::exception& e) {
            return ErrorResponse(
                std::string("Script subprocess failed: ") + e.what());
        }

        if (output.empty())
            return ErrorResponse("Script returned no output for tool: " + toolName);

        TrimRight(output);

        try {
            auto result = nlohmann::json::parse(output);

            if (result.value("status", "") == "error") {
                return {
                    {"isError", true},
                    {"content", {{{"type", "text"},
                                  {"text", result.value("error", "Script reported error")}}}}
                };
            }

            if (result.contains("content") && result["content"].is_string()) {
                nlohmann::json wrapped = result;
                wrapped["content"] = {{{"type", "text"},
                                        {"text", result["content"].get<std::string>()}}};
                return wrapped;
            }

            return result;

        } catch (const nlohmann::json::parse_error& e) {
            return ErrorResponse(
                std::string("Script output is not valid JSON: ") + e.what()
                + " | output: " + output.substr(0, 200));
        }
    });
}
