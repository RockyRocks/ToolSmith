#include <core/Config.h>
#include <core/Env.h>
#include <core/Logger.h>
#include <core/Version.h>
#include <core/ProtocolHandler.h>
#include <core/ThreadPool.h>
#include <plugins/PackManifest.h>
#include <core/ToolProfile.h>
#include <core/WorkspaceJail.h>
#include <commands/CommandRegistry.h>
#include <commands/CoreTools.h>
#include <commands/EchoCommand.h>
#include <llm/LiteLLMProvider.h>
#include <llm/LLMCommand.h>
#include <discovery/McpServerRegistry.h>
#include <discovery/CompositeCommand.h>
#include <skills/SkillEngine.h>
#include <skills/SkillToolAdapter.h>
#include <skills/PluginLoader.h>
#include <skills/AgentSkillLoader.h>
#include <plugins/NativePluginLoader.h>
#include <plugins/ScriptPluginLoader.h>
#include <http/HttplibClient.h>
#include <security/RateLimiter.h>
#include <security/ApiKeyValidator.h>
#include <server/IServer.h>
#include <server/HttplibServer.h>
#include <server/StdioTransport.h>

#ifdef USE_UWS
#include <server/UwsServer.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string Trim(std::string s) {
    auto a = s.find_first_not_of(" \t");
    auto b = s.find_last_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            auto t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    auto t = Trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

void PrintUsage() {
    std::cerr
        << "Usage: toolsmith [--stdio] [--config PATH] [--profile PROFILE]\n"
        << "                 [--tools a,b,c] [--workspace PATH]\n"
        << "  PROFILE = auto|core|cpp|csharp|fullstack|all\n"
        << "  Env: TOOLSMITH_PROFILE TOOLSMITH_TOOLS TOOLSMITH_WORKSPACE\n";
}

std::string HomeDir() {
#ifdef _WIN32
    std::string p = GetEnvVar("USERPROFILE");
    if (p.empty()) p = GetEnvVar("HOME");
    return p;
#else
    return GetEnvVar("HOME");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "config/mcp_config.json";
    bool stdioMode = false;
    std::string cliProfile;
    std::string cliWorkspace;
    std::vector<std::string> cliPinTools;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--config") {
            configPath = need("--config");
        } else if (arg == "--stdio") {
            stdioMode = true;
        } else if (arg == "--profile") {
            cliProfile = need("--profile");
        } else if (arg == "--tools") {
            cliPinTools = SplitCsv(need("--tools"));
        } else if (arg == "--workspace") {
            cliWorkspace = need("--workspace");
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage();
            return 1;
        }
    }

    if (stdioMode) {
        Logger::GetInstance().SetSuppressStdout(true);
        Logger::GetInstance().SetObserver([](const std::string& msg) {
            std::cerr << "[LOG] " << msg << std::endl;
        });
    }

    Config config;
    try {
        config = Config::LoadFromFile(configPath);
        Logger::GetInstance().Log("Config loaded from " + configPath);
    } catch (...) {
        Logger::GetInstance().Log("Config file not found, using environment variables");
        config = Config::LoadFromEnv();
    }

    std::string workspace = cliWorkspace;
    if (workspace.empty()) workspace = GetEnvVar("TOOLSMITH_WORKSPACE");
    if (workspace.empty() || workspace == "auto") workspace = config.GetWorkspaceRoot();
    if (workspace.empty() || workspace == "auto") {
        workspace = WorkspaceJail::DetectRoot(fs::current_path()).string();
    }

    std::vector<fs::path> allow;
    for (const auto& a : config.GetWorkspaceAllow()) allow.emplace_back(a);

    std::shared_ptr<CoreRuntime> runtime;
    try {
        runtime = std::make_shared<CoreRuntime>();
        runtime->jail = WorkspaceJail::Open(workspace, allow);
    } catch (const std::exception& e) {
        std::cerr << "Workspace jail: " << e.what() << "\n";
        return 1;
    }

    std::string profile = cliProfile;
    if (profile.empty()) profile = GetEnvVar("TOOLSMITH_PROFILE");
    if (profile.empty()) profile = config.GetToolsProfile();

    std::vector<std::string> pinTools = cliPinTools;
    if (pinTools.empty()) {
        std::string envTools = GetEnvVar("TOOLSMITH_TOOLS");
        if (!envTools.empty()) pinTools = SplitCsv(envTools);
        else pinTools = config.GetToolsPin();
    }

    ToolProfile::ResolveInput rin;
    rin.profile = profile;
    rin.enable = config.GetToolsEnable();
    rin.disable = config.GetToolsDisable();
    rin.pinTools = pinTools;
    rin.workspaceRoot = runtime->jail.Root().string();

    auto resolved = ToolProfile::Resolve(rin);
    if (!resolved.error.empty()) {
        std::cerr << resolved.error << "\n";
        return 1;
    }
    runtime->profile = resolved.profile;
    runtime->advertised = resolved.advertised;
    runtime->skills = DiscoverAgentSkills(runtime->jail.Root().string(), HomeDir());

    auto httpClient = std::make_shared<HttplibClient>();
    auto llmProvider = std::make_shared<LiteLLMProvider>(httpClient, config);

    auto skillEngine = std::make_shared<SkillEngine>();
    auto mcpRegistry = std::make_shared<McpServerRegistry>();
    try {
        *mcpRegistry = McpServerRegistry::LoadFromFile(config.GetMcpServersConfigPath());
    } catch (const std::exception& e) {
        Logger::GetInstance().Log(std::string("MCP servers config: ") + e.what());
    }

    auto commandRegistry = std::make_shared<CommandRegistry>();
    commandRegistry->SetChainingEnabled(config.IsChainingEnabled());

    try {
        RegisterCoreTools(*commandRegistry, runtime);
        commandRegistry->RegisterCommand("echo", CreateEchoCommand(true));
        commandRegistry->RegisterCommand("llm", std::make_shared<LLMCommand>(llmProvider));
        commandRegistry->RegisterCommand(
            "remote", std::make_shared<CompositeCommand>(mcpRegistry, httpClient));

        if (resolved.loadAllPlugins) {
            skillEngine->LoadFromDirectory(config.GetSkillsDirectory());
            auto pluginSkillNames = PluginLoader::LoadIntoEngine(
                config.GetPluginsDirectory(), *skillEngine);
            std::unordered_set<std::string> pluginSkillSet(
                pluginSkillNames.begin(), pluginSkillNames.end());
            for (const auto& name : skillEngine->ListSkills()) {
                auto def = skillEngine->Resolve(name);
                if (!def.has_value()) continue;
                if (commandRegistry->HasCommand(name)) {
                    throw std::runtime_error(
                        "Tool name collision: '" + name + "' is already registered");
                }
                ToolSource source = pluginSkillSet.count(name) ? ToolSource::Plugin
                                                               : ToolSource::JsonSkill;
                commandRegistry->RegisterCommand(
                    name, std::make_shared<SkillToolAdapter>(*def, llmProvider, source));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    auto nativeLoader = std::make_shared<NativePluginLoader>();
    auto scriptLoader = std::make_shared<ScriptPluginLoader>();

    if (!resolved.loadAllPlugins) {
        std::unordered_set<std::string> packs{resolved.profile, "core"};
        for (const auto& p : config.GetToolsEnable()) packs.insert(p);
        nativeLoader->SetActivePacks(packs);
        scriptLoader->SetActivePacks(packs);
    }

    try {
        nativeLoader->LoadAll(config.GetPluginsDirectory(), *commandRegistry);
        if (resolved.loadAllPlugins || !config.GetToolsEnable().empty())
            scriptLoader->LoadAll(config.GetPluginsDirectory(), *commandRegistry);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    if (!pinTools.empty()) {
        for (const auto& t : pinTools) {
            if (!commandRegistry->HasCommand(t)) {
                std::cerr << "Unknown tool in --tools: " << t << "\n";
                return 1;
            }
        }
        std::unordered_set<std::string> pin(pinTools.begin(), pinTools.end());
        runtime->advertised = pin;
        commandRegistry->SetAdvertised(pin);
    } else if (!resolved.loadAllPlugins) {
        commandRegistry->SetAdvertised(runtime->advertised);
    }

    Logger::GetInstance().Log(
        "Profile " + runtime->profile + " advertised "
        + std::to_string(commandRegistry->ListToolMetadata().size()) + " tool(s)");

    if (stdioMode) {
        auto transportPtr = std::make_shared<StdioTransport>(
            commandRegistry, skillEngine, mcpRegistry,
            std::cin, std::cout, "toolsmith", TOOLSMITH_VERSION_STRING);
        transportPtr->SetCoreRuntime(runtime);

        if (resolved.loadAllPlugins) {
            nativeLoader->SetNotifyCallback(
                [transportPtr](const nlohmann::json& payload) {
                    nlohmann::json notification = {
                        {"jsonrpc", "2.0"},
                        {"method",  "notifications/tools/list_changed"},
                        {"params",  payload}
                    };
                    transportPtr->PushNotification(notification);
                });
            scriptLoader->SetNotifyCallback(
                [transportPtr](const nlohmann::json& payload) {
                    nlohmann::json notification = {
                        {"jsonrpc", "2.0"},
                        {"method",  "notifications/tools/list_changed"},
                        {"params",  payload}
                    };
                    transportPtr->PushNotification(notification);
                });
            nativeLoader->StartWatcher(config.GetPluginsDirectory(), commandRegistry);
            scriptLoader->StartWatcher(config.GetPluginsDirectory(), commandRegistry);
        }

        transportPtr->Run();

        if (resolved.loadAllPlugins) {
            scriptLoader->StopWatcher();
            nativeLoader->StopWatcher();
        }
        return 0;
    }

    auto rateLimiter = std::make_shared<RateLimiter>(config.GetRateLimitRequestsPerMinute());
    auto apiKeyValidator = std::make_shared<ApiKeyValidator>(
        config.GetAuthApiKey(), config.IsAuthEnabled());

    auto protocolHandler = std::make_shared<ProtocolHandler>(
        commandRegistry, rateLimiter, apiKeyValidator, config.GetMaxRequestBodySize());

    std::unique_ptr<IServer> server;
#ifdef USE_UWS
    server = std::make_unique<UwsServer>();
    Logger::GetInstance().Log("Using uWebSockets server");
#else
    server = std::make_unique<HttplibServer>();
    Logger::GetInstance().Log("Using httplib server");
#endif

    server->AddRoute("POST", "/mcp",
        [protocolHandler](const std::string& body, const std::string& clientIp,
                           const std::string& authHeader,
                           std::function<void(int, const std::string&)> respond) {
            auto result = protocolHandler->HandleRequest(body, clientIp, authHeader);

            int status = 200;
            try {
                auto parsed = nlohmann::json::parse(result);
                if (parsed.value("error", "") == "Payload too large") status = 413;
                else if (parsed.value("error", "") == "Rate limit exceeded") status = 429;
                else if (parsed.value("error", "") == "Unauthorized") status = 401;
                else if (parsed.value("error", "") == "Invalid JSON") status = 400;
                else if (parsed.value("error", "") == "JSON nesting too deep") status = 400;
                else if (parsed.contains("error")) status = 400;
            } catch (...) {}

            respond(status, result);
        });

    server->AddRoute("GET", "/health",
        [](const std::string&, const std::string&, const std::string&,
           std::function<void(int, const std::string&)> respond) {
            respond(200, R"({"status":"ok"})");
        });

    server->AddRoute("GET", "/skills",
        [commandRegistry](const std::string&, const std::string&, const std::string&,
                           std::function<void(int, const std::string&)> respond) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& meta : commandRegistry->ListToolMetadata()) {
                if (meta.m_Source == ToolSource::BuiltIn) continue;
                arr.push_back({
                    {"name", meta.m_Name},
                    {"description", meta.m_Description},
                    {"inputSchema", meta.m_InputSchema}
                });
            }
            respond(200, arr.dump());
        });

    server->AddRoute("GET", "/servers",
        [mcpRegistry](const std::string&, const std::string&, const std::string&,
                       std::function<void(int, const std::string&)> respond) {
            respond(200, mcpRegistry->ToJson().dump());
        });

    server->AddRoute("GET", "/commands",
        [commandRegistry](const std::string&, const std::string&, const std::string&,
                           std::function<void(int, const std::string&)> respond) {
            nlohmann::json cmds = nlohmann::json::array();
            for (const auto& meta : commandRegistry->ListToolMetadata()) {
                cmds.push_back(meta.m_Name);
            }
            respond(200, cmds.dump());
        });

    if (resolved.loadAllPlugins) {
        nativeLoader->SetNotifyCallback([](const nlohmann::json& payload) {
            Logger::GetInstance().Log("[plugin_loaded] " + payload.dump());
        });
        scriptLoader->SetNotifyCallback([](const nlohmann::json& payload) {
            Logger::GetInstance().Log("[script_plugin_reloaded] " + payload.dump());
        });
        nativeLoader->StartWatcher(config.GetPluginsDirectory(), commandRegistry);
        scriptLoader->StartWatcher(config.GetPluginsDirectory(), commandRegistry);
    }

    int port = config.GetServerPort();
    Logger::GetInstance().Log("Starting server on port " + std::to_string(port));
    server->Listen("0.0.0.0", port);

    if (resolved.loadAllPlugins) {
        scriptLoader->StopWatcher();
        nativeLoader->StopWatcher();
    }
    return 0;
}
