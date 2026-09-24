#include <server/StdioTransport.h>
#include <commands/CommandRegistry.h>
#include <commands/CoreTools.h>
#include <skills/SkillEngine.h>
#include <skills/AgentSkillLoader.h>
#include <discovery/McpServerRegistry.h>
#include <core/Env.h>
#include <core/ThreadPool.h>
#include <core/Logger.h>
#include <core/ResultBudget.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// JSON-RPC 2.0 error codes
static constexpr int JSONRPC_PARSE_ERROR      = -32700;
static constexpr int JSONRPC_INVALID_REQUEST  = -32600;
static constexpr int JSONRPC_METHOD_NOT_FOUND = -32601;
static constexpr int JSONRPC_INVALID_PARAMS   = -32602;
static constexpr int JSONRPC_INTERNAL_ERROR   = -32603;

static constexpr const char* MCP_PROTOCOL_VERSION = "2024-11-05";

StdioTransport::StdioTransport(
    std::shared_ptr<CommandRegistry> registry,
    std::shared_ptr<SkillEngine> skillEngine,
    std::shared_ptr<McpServerRegistry> mcpRegistry,
    std::istream& input,
    std::ostream& output,
    const std::string& serverName,
    const std::string& serverVersion)
    : m_Registry(std::move(registry))
    , m_SkillEngine(std::move(skillEngine))
    , m_McpRegistry(std::move(mcpRegistry))
    , m_Input(input)
    , m_Output(output)
    , m_ServerName(serverName)
    , m_ServerVersion(serverVersion)
{
    std::error_code ec;
    m_ResourceRoot = std::filesystem::current_path(ec).string();
}

void StdioTransport::SetCoreRuntime(std::shared_ptr<CoreRuntime> runtime) {
    m_CoreRuntime = std::move(runtime);
    if (m_CoreRuntime) {
        m_ResourceRoot = m_CoreRuntime->jail.Root().string();
    }
}

void StdioTransport::Run() {
#ifdef _WIN32
    // Set binary mode on stdin/stdout to avoid \r\n corruption
    if (&m_Input == &std::cin) {
        _setmode(_fileno(stdin), _O_BINARY);
    }
    if (&m_Output == &std::cout) {
        _setmode(_fileno(stdout), _O_BINARY);
    }
#endif

    // Redirect logger to stderr when using real stdin/stdout
    if (&m_Output == &std::cout) {
        Logger::GetInstance().SetSuppressStdout(true);
        Logger::GetInstance().SetObserver([](const std::string& msg) {
            std::cerr << "[LOG] " << msg << std::endl;
        });
    }

    m_Running = true;
    std::string line;
    std::vector<std::future<void>> inFlightCalls;

    while (m_Running && std::getline(m_Input, line)) {
        if (line.empty()) {
            continue;
        }

        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        nlohmann::json message;
        try {
            message = nlohmann::json::parse(line);
        } catch (const nlohmann::json::parse_error&) {
            SendMessage(MakeError(nullptr, JSONRPC_PARSE_ERROR, "Parse error"));
            continue;
        }

        // Validate basic JSON-RPC structure
        if (!message.is_object() || !message.contains("jsonrpc") ||
            message["jsonrpc"] != "2.0" || !message.contains("method")) {
            auto id = message.contains("id") ? message["id"] : nullptr;
            SendMessage(MakeError(id, JSONRPC_INVALID_REQUEST, "Invalid JSON-RPC 2.0 request"));
            continue;
        }

        // Notifications have no "id" field — process but don't respond
        bool isNotification = !message.contains("id");

        const std::string method = message.value("method", "");
        if (method == "tools/call" && message.contains("id")) {
            inFlightCalls.push_back(ThreadPool::Shared().Submit([this, message]() {
                nlohmann::json response = Dispatch(message);
                if (!response.is_null()) SendMessage(response);
            }));
            continue;
        }

        nlohmann::json response = Dispatch(message);

        if (!isNotification && !response.is_null()) {
            SendMessage(response);
        }
    }

    for (auto& call : inFlightCalls) call.wait();
    StopResourceWatcher();
    m_Running = false;
}

void StdioTransport::Stop() {
    m_Running = false;
}

void StdioTransport::PushNotification(const nlohmann::json& notification) {
    SendMessage(notification);
}

nlohmann::json StdioTransport::Dispatch(const nlohmann::json& message) {
    std::string method = message["method"].get<std::string>();
    auto id = message.contains("id") ? message["id"] : nullptr;
    auto params = message.value("params", nlohmann::json::object());

    if (method == "initialize") {
        return HandleInitialize(params, id);
    }
    if (method == "notifications/initialized") {
        return nullptr;
    }
    if (method == "notifications/cancelled") {
        HandleCancelNotification(params);
        return nullptr;
    }

    // All methods below require initialization
    if (!m_Initialized) {
        return MakeError(id, JSONRPC_INVALID_REQUEST, "Server not initialized");
    }

    if (method == "tools/list") {
        return HandleToolsList(id);
    }
    if (method == "tools/call") {
        return HandleToolsCall(params, id);
    }
    if (method == "tools/call_batch") {
        return HandleToolsCallBatch(params, id);
    }
    if (method == "prompts/list") {
        return HandlePromptsList(id);
    }
    if (method == "prompts/get") {
        return HandlePromptsGet(params, id);
    }
    if (method == "resources/list") {
        return HandleResourcesList(params, id);
    }
    if (method == "resources/read") {
        return HandleResourcesRead(params, id);
    }
    if (method == "resources/subscribe") {
        return HandleResourcesSubscribe(params, id);
    }
    if (method == "resources/unsubscribe") {
        return HandleResourcesUnsubscribe(params, id);
    }

    return MakeError(id, JSONRPC_METHOD_NOT_FOUND,
                     "Method not found: " + method);
}

// ---------------------------------------------------------------------------
// MCP method handlers
// ---------------------------------------------------------------------------

nlohmann::json StdioTransport::HandleInitialize(
    const nlohmann::json& /*params*/, const nlohmann::json& id) {
    m_Initialized = true;

    nlohmann::json result = {
        {"protocolVersion", MCP_PROTOCOL_VERSION},
        {"capabilities", {
            {"tools", {{"listChanged", true}}},
            {"prompts", nlohmann::json::object()},
            {"resources", {{"subscribe", true}}}
        }},
        {"serverInfo", {
            {"name", m_ServerName},
            {"version", m_ServerVersion}
        }}
    };

    return MakeResponse(id, result);
}

nlohmann::json StdioTransport::HandleToolsList(const nlohmann::json& id) {
    auto tools = BuildToolList();
    nlohmann::json toolsArray = nlohmann::json::array();

    for (const auto& tool : tools) {
        toolsArray.push_back({
            {"name", tool.m_Name},
            {"description", tool.m_Description},
            {"inputSchema", tool.m_InputSchema}
        });
    }

    nlohmann::json resp = MakeResponse(id, {{"tools", toolsArray}});
    const std::string metrics = GetEnvVar("TOOLSMITH_METRICS");
    if (!metrics.empty() && metrics[0] == '1') {
        const std::string dumped = resp.dump();
        Logger::GetInstance().Log("tools_list_bytes=" + std::to_string(dumped.size()));
    }
    return resp;
}

nlohmann::json StdioTransport::HandleToolsCall(
    const nlohmann::json& params, const nlohmann::json& id) {

    if (!params.contains("name") || !params["name"].is_string()) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Missing required parameter: name");
    }

    std::string toolName = params["name"].get<std::string>();
    auto arguments = params.value("arguments", nlohmann::json::object());

    // Check if the tool/command exists
    auto cmd = m_Registry->Resolve(toolName);
    if (!cmd) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Unknown tool: " + toolName);
    }

    // Inject per-tool defaults if the caller didn't provide overrides
    auto meta = cmd->GetMetadata();
    if (!meta.m_DefaultModel.empty() && !arguments.contains("model")) {
        arguments["model"] = meta.m_DefaultModel;
    }
    if (!meta.m_DefaultParameters.is_null() && !meta.m_DefaultParameters.empty()
        && !arguments.contains("parameters")) {
        arguments["parameters"] = meta.m_DefaultParameters;
    }

    std::string requestId = id.is_string() ? id.get<std::string>() : id.dump();
    arguments["_requestId"] = requestId;

    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        m_InFlightRequests[requestId] = toolName;
    }

    nlohmann::json internalRequest = {
        {"command", toolName},
        {"payload", arguments}
    };

    try {
        nlohmann::json result = m_Registry->ExecuteWithChaining(toolName, internalRequest);

        {
            std::lock_guard<std::mutex> lock(m_InFlightMutex);
            m_InFlightRequests.erase(requestId);
        }

        bool isError = result.value("isError", false)
                    || result.value("status", "ok") == "error";
        nlohmann::json textContent;
        if (result.contains("content") && result["content"].is_string())
            textContent = result["content"].get<std::string>();
        else if (result.contains("content") && result["content"].is_array())
            textContent = result["content"];
        else
            textContent = result.dump();

        nlohmann::json mcpResult = {
            {"content", textContent.is_array() ? textContent : nlohmann::json::array({
                {{"type", "text"}, {"text", textContent}}
            })},
            {"isError", isError}
        };

        return MakeResponse(id, mcpResult);

    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(m_InFlightMutex);
            m_InFlightRequests.erase(requestId);
        }
        return MakeError(id, JSONRPC_INTERNAL_ERROR, e.what());
    }
}

nlohmann::json StdioTransport::HandleToolsCallBatch(
    const nlohmann::json& params, const nlohmann::json& id) {

    if (!params.contains("calls") || !params["calls"].is_array()) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Missing required parameter: calls (array)");
    }

    const auto& calls = params["calls"];
    if (calls.empty()) {
        return MakeResponse(id, {{"results", nlohmann::json::array()}});
    }

    // Phase 1: resolve all commands and launch futures in parallel
    struct PendingCall {
        std::string name;
        std::future<nlohmann::json> future;
        bool isError = false;
        std::string errorMsg;
    };

    std::vector<PendingCall> pending;
    pending.reserve(calls.size());

    for (const auto& call : calls) {
        if (!call.contains("name") || !call["name"].is_string()) {
            PendingCall pc;
            pc.name = "<unknown>";
            pc.isError = true;
            pc.errorMsg = "Each call must have a string 'name' field";
            pending.push_back(std::move(pc));
            continue;
        }

        std::string toolName = call["name"].get<std::string>();
        auto arguments = call.value("arguments", nlohmann::json::object());

        auto cmd = m_Registry->Resolve(toolName);
        if (!cmd) {
            PendingCall pc;
            pc.name = toolName;
            pc.isError = true;
            pc.errorMsg = "Unknown tool: " + toolName;
            pending.push_back(std::move(pc));
            continue;
        }

        auto meta = cmd->GetMetadata();
        if (!meta.m_DefaultModel.empty() && !arguments.contains("model")) {
            arguments["model"] = meta.m_DefaultModel;
        }
        if (!meta.m_DefaultParameters.is_null() && !meta.m_DefaultParameters.empty()
            && !arguments.contains("parameters")) {
            arguments["parameters"] = meta.m_DefaultParameters;
        }

        nlohmann::json internalRequest = {
            {"command", toolName},
            {"payload", arguments}
        };

        PendingCall pc;
        pc.name = toolName;
        try {
            pc.future = cmd->ExecuteAsync(internalRequest);
        } catch (const std::exception& e) {
            pc.isError = true;
            pc.errorMsg = e.what();
        }
        pending.push_back(std::move(pc));
    }

    // Phase 2: collect all results (futures run in parallel during phase 1)
    nlohmann::json results = nlohmann::json::array();
    for (auto& pc : pending) {
        if (pc.isError) {
            results.push_back({
                {"name", pc.name},
                {"isError", true},
                {"content", nlohmann::json::array({
                    {{"type", "text"}, {"text", pc.errorMsg}}
                })}
            });
            continue;
        }

        try {
            nlohmann::json result = pc.future.get();
            bool isError = result.value("status", "ok") == "error";
            results.push_back({
                {"name", pc.name},
                {"isError", isError},
                {"content", nlohmann::json::array({
                    {{"type", "text"}, {"text", result.dump()}}
                })}
            });
        } catch (const std::exception& e) {
            results.push_back({
                {"name", pc.name},
                {"isError", true},
                {"content", nlohmann::json::array({
                    {{"type", "text"}, {"text", std::string(e.what())}}
                })}
            });
        }
    }

    return MakeResponse(id, {{"results", results}});
}

nlohmann::json StdioTransport::HandlePromptsList(const nlohmann::json& id) {
    nlohmann::json promptsArray = nlohmann::json::array();

    if (m_SkillEngine) {
        auto skillsJson = m_SkillEngine->ListSkillsJson();
        for (const auto& skill : skillsJson) {
            nlohmann::json prompt = {
                {"name", skill.value("name", "")},
                {"description", skill.value("description", "")}
            };

            // Map required_variables to MCP prompt arguments
            nlohmann::json args = nlohmann::json::array();
            if (skill.contains("required_variables")) {
                for (const auto& var : skill["required_variables"]) {
                    args.push_back({
                        {"name", var.get<std::string>()},
                        {"description", "Required variable"},
                        {"required", true}
                    });
                }
            }
            prompt["arguments"] = args;
            promptsArray.push_back(prompt);
        }
    }

    return MakeResponse(id, {{"prompts", promptsArray}});
}

nlohmann::json StdioTransport::HandlePromptsGet(
    const nlohmann::json& params, const nlohmann::json& id) {

    if (!params.contains("name") || !params["name"].is_string()) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Missing required parameter: name");
    }

    std::string promptName = params["name"].get<std::string>();
    auto arguments = params.value("arguments", nlohmann::json::object());

    if (!m_SkillEngine) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "No skill engine available");
    }

    auto skill = m_SkillEngine->Resolve(promptName);
    if (!skill) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Unknown prompt: " + promptName);
    }

    try {
        std::string rendered = m_SkillEngine->RenderPrompt(*skill, arguments);

        nlohmann::json result = {
            {"messages", nlohmann::json::array({
                {
                    {"role", "user"},
                    {"content", {
                        {"type", "text"},
                        {"text", rendered}
                    }}
                }
            })}
        };

        return MakeResponse(id, result);

    } catch (const std::exception& e) {
        return MakeError(id, JSONRPC_INVALID_PARAMS, e.what());
    }
}

nlohmann::json StdioTransport::HandleResourcesList(
    const nlohmann::json& /*params*/, const nlohmann::json& id) {

    namespace fs = std::filesystem;
    nlohmann::json resources = nlohmann::json::array();

    if (m_CoreRuntime && m_Registry && m_Registry->IsAdvertised("skills")) {
        for (const auto& skill : m_CoreRuntime->skills.skills) {
            resources.push_back({
                {"uri", "skill://" + skill.m_Name},
                {"name", skill.m_Name},
                {"description", skill.m_Description},
                {"mimeType", "text/markdown"}
            });
        }
    }

    if (m_ResourceRoot.empty()) {
        return MakeResponse(id, {{"resources", resources}});
    }

    std::error_code ec;
    fs::path root(m_ResourceRoot);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return MakeResponse(id, {{"resources", resources}});
    }

    static constexpr int kMaxEntries = 200;
    int count = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator() && count < kMaxEntries; ++it)
    {
        if (it->is_directory(ec)) continue;

        auto relPath = fs::relative(it->path(), root, ec);
        if (ec) continue;

        std::string uri = "file://" + relPath.generic_string();
        std::string name = relPath.generic_string();

        resources.push_back({
            {"uri", uri},
            {"name", name},
            {"mimeType", "text/plain"}
        });
        ++count;
    }

    return MakeResponse(id, {{"resources", resources}});
}

nlohmann::json StdioTransport::HandleResourcesRead(
    const nlohmann::json& params, const nlohmann::json& id) {

    namespace fs = std::filesystem;

    if (!params.contains("uri") || !params["uri"].is_string()) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Missing required parameter: uri");
    }

    std::string uri = params["uri"].get<std::string>();

    if (uri.rfind("skill://", 0) == 0) {
        if (!m_CoreRuntime || !m_Registry || !m_Registry->IsAdvertised("skills")) {
            return MakeError(id, JSONRPC_INVALID_PARAMS,
                             "skills pack is not advertised; call activate pack=skills");
        }
        std::string rest = uri.substr(8);
        auto slash = rest.find('/');
        std::string name = slash == std::string::npos ? rest : rest.substr(0, slash);
        std::string rel = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (rel.empty()) {
            auto body = LoadAgentSkillBody(m_CoreRuntime->skills, name);
            if (!body.error.empty()) {
                return MakeError(id, JSONRPC_INVALID_PARAMS, body.error);
            }
            bool trunc = false;
            std::string text = CapHead(body.body, trunc);
            (void)trunc;
            return MakeResponse(id, {{"contents", nlohmann::json::array({
                {{"uri", uri}, {"mimeType", "text/markdown"}, {"text", text}}
            })}});
        }
        namespace fs = std::filesystem;
        fs::path skillRoot;
        for (const auto& s : m_CoreRuntime->skills.skills) {
            if (s.m_Name == name) { skillRoot = s.m_Root; break; }
        }
        if (skillRoot.empty()) {
            return MakeError(id, JSONRPC_INVALID_PARAMS, "Unknown skill: " + name);
        }
        std::error_code ec;
        fs::path full = fs::weakly_canonical(skillRoot / rel, ec);
        fs::path root = fs::weakly_canonical(skillRoot, ec);
        auto relative = full.lexically_relative(root);
        bool outside = relative.empty();
        for (const auto& part : relative) if (part == "..") outside = true;
        if (outside) {
            return MakeError(id, JSONRPC_INVALID_PARAMS, "Access denied: path outside skill root");
        }
        if (!fs::is_regular_file(full, ec)) {
            return MakeError(id, JSONRPC_INVALID_PARAMS, "Resource not found: " + uri);
        }
        std::ifstream f(full, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        bool trunc = false;
        content = CapHead(std::move(content), trunc);
        (void)trunc;
        return MakeResponse(id, {{"contents", nlohmann::json::array({
            {{"uri", uri}, {"mimeType", "text/plain"}, {"text", content}}
        })}});
    }

    // Strip file:// prefix
    std::string relPath = uri;
    if (relPath.rfind("file://", 0) == 0) {
        relPath = relPath.substr(7);
    }

    fs::path fullPath = (fs::path(m_ResourceRoot) / relPath).lexically_normal();

    // Path traversal check
    std::error_code ec;
    fs::path canonRoot = fs::canonical(fs::path(m_ResourceRoot), ec);
    if (ec) {
        return MakeError(id, JSONRPC_INTERNAL_ERROR, "Cannot resolve resource root");
    }

    if (!fs::exists(fullPath, ec)) {
        return MakeError(id, JSONRPC_INVALID_PARAMS, "Resource not found: " + uri);
    }

    fs::path canonPath = fs::canonical(fullPath, ec);
    if (ec || canonPath.string().rfind(canonRoot.string(), 0) != 0) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Access denied: path outside resource root");
    }

    // Read file contents (cap at 1 MiB)
    static constexpr size_t kMaxReadSize = 1024 * 1024;
    std::ifstream f(canonPath, std::ios::binary);
    if (!f.is_open()) {
        return MakeError(id, JSONRPC_INTERNAL_ERROR,
                         "Cannot open resource: " + uri);
    }

    std::string content;
    content.resize(kMaxReadSize);
    f.read(content.data(), static_cast<std::streamsize>(kMaxReadSize));
    content.resize(static_cast<size_t>(f.gcount()));

    nlohmann::json result = {
        {"contents", nlohmann::json::array({
            {
                {"uri", uri},
                {"mimeType", "text/plain"},
                {"text", content}
            }
        })}
    };

    return MakeResponse(id, result);
}

nlohmann::json StdioTransport::HandleResourcesSubscribe(
    const nlohmann::json& params, const nlohmann::json& id) {

    namespace fs = std::filesystem;

    if (!params.contains("uri") || !params["uri"].is_string()) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Missing required parameter: uri");
    }

    std::string uri = params["uri"].get<std::string>();

    std::string relPath = uri;
    if (relPath.rfind("file://", 0) == 0) {
        relPath = relPath.substr(7);
    }

    fs::path fullPath = (fs::path(m_ResourceRoot) / relPath).lexically_normal();

    std::error_code ec;
    fs::path canonRoot = fs::canonical(fs::path(m_ResourceRoot), ec);
    if (ec) {
        return MakeError(id, JSONRPC_INTERNAL_ERROR, "Cannot resolve resource root");
    }

    if (!fs::exists(fullPath, ec)) {
        return MakeError(id, JSONRPC_INVALID_PARAMS, "Resource not found: " + uri);
    }

    fs::path canonPath = fs::canonical(fullPath, ec);
    if (ec || canonPath.string().rfind(canonRoot.string(), 0) != 0) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Access denied: path outside resource root");
    }

    {
        std::lock_guard<std::mutex> lock(m_SubscriptionMutex);
        m_SubscribedUris.insert(uri);
        auto mtime = fs::last_write_time(canonPath, ec);
        if (!ec) {
            m_SubscribedMtimes[uri] = mtime;
        }
    }

    StartResourceWatcher();

    return MakeResponse(id, nlohmann::json::object());
}

nlohmann::json StdioTransport::HandleResourcesUnsubscribe(
    const nlohmann::json& params, const nlohmann::json& id) {

    if (!params.contains("uri") || !params["uri"].is_string()) {
        return MakeError(id, JSONRPC_INVALID_PARAMS,
                         "Missing required parameter: uri");
    }

    std::string uri = params["uri"].get<std::string>();

    {
        std::lock_guard<std::mutex> lock(m_SubscriptionMutex);
        m_SubscribedUris.erase(uri);
        m_SubscribedMtimes.erase(uri);

        if (m_SubscribedUris.empty()) {
            StopResourceWatcher();
        }
    }

    return MakeResponse(id, nlohmann::json::object());
}

void StdioTransport::StartResourceWatcher() {
    if (m_ResourceWatcherThread.joinable()) return;

    m_ResourceWatcherStop = false;

    m_ResourceWatcherThread = std::thread([this]() {
        namespace fs = std::filesystem;

        while (!m_ResourceWatcherStop.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kResourceWatchIntervalMs));
            if (m_ResourceWatcherStop.load()) break;

            std::lock_guard<std::mutex> lock(m_SubscriptionMutex);

            for (const auto& uri : m_SubscribedUris) {
                std::string relPath = uri;
                if (relPath.rfind("file://", 0) == 0)
                    relPath = relPath.substr(7);

                fs::path fullPath = (fs::path(m_ResourceRoot) / relPath).lexically_normal();
                std::error_code ec;
                if (!fs::exists(fullPath, ec)) continue;

                auto mtime = fs::last_write_time(fullPath, ec);
                if (ec) continue;

                auto it = m_SubscribedMtimes.find(uri);
                if (it != m_SubscribedMtimes.end() && it->second == mtime)
                    continue;

                m_SubscribedMtimes[uri] = mtime;

                nlohmann::json notification = {
                    {"jsonrpc", "2.0"},
                    {"method", "notifications/resources/updated"},
                    {"params", {{"uri", uri}}}
                };
                SendMessage(notification);
            }
        }
    });
}

void StdioTransport::StopResourceWatcher() {
    m_ResourceWatcherStop = true;
    if (m_ResourceWatcherThread.joinable()) {
        m_ResourceWatcherThread.join();
    }
}

void StdioTransport::HandleCancelNotification(const nlohmann::json& params) {
    if (!params.contains("requestId")) return;

    const auto& rawId = params["requestId"];
    std::string cancelledId = rawId.is_string() ? rawId.get<std::string>() : rawId.dump();
    std::string toolName;
    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        auto it = m_InFlightRequests.find(cancelledId);
        if (it == m_InFlightRequests.end()) return;
        toolName = it->second;
    }

    auto cmd = m_Registry->Resolve(toolName);
    if (cmd) {
        cmd->Cancel(cancelledId);
    }
}

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

nlohmann::json StdioTransport::MakeResponse(
    const nlohmann::json& id, const nlohmann::json& result) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

nlohmann::json StdioTransport::MakeError(
    const nlohmann::json& id, int code,
    const std::string& message, const nlohmann::json& data) {
    nlohmann::json error = {
        {"code", code},
        {"message", message}
    };
    if (!data.is_null()) {
        error["data"] = data;
    }
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", error}
    };
}

void StdioTransport::SendMessage(const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lock(m_WriteMutex);
    m_Output << msg.dump() << "\n";
    m_Output.flush();
}

// ---------------------------------------------------------------------------
// Tool metadata
// ---------------------------------------------------------------------------

std::vector<StdioTransport::ToolMeta> StdioTransport::BuildToolList() const {
    std::vector<ToolMeta> tools;
    for (auto& meta : m_Registry->ListToolMetadata()) {
        tools.push_back({
            std::move(meta.m_Name),
            std::move(meta.m_Description),
            std::move(meta.m_InputSchema)
        });
    }
    return tools;
}
