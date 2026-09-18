#include <core/Config.h>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {

std::string GetEnvVar(const char* name) {
#ifdef _MSC_VER
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, name) == 0 && val != nullptr) {
        std::string result(val);
        free(val);
        return result;
    }
    return "";
#else
    const char* val = std::getenv(name);
    return val ? std::string(val) : "";
#endif
}

std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        auto start = cur.find_first_not_of(" \t");
        auto end = cur.find_last_not_of(" \t");
        if (start != std::string::npos)
            out.push_back(cur.substr(start, end - start + 1));
        cur.clear();
    };
    for (char c : s) {
        if (c == ',') flush();
        else cur.push_back(c);
    }
    if (!cur.empty()) flush();
    return out;
}

std::vector<std::string> JsonStringList(const nlohmann::json& data,
                                        const char* a, const char* b) {
    std::vector<std::string> out;
    if (!data.contains(a) || !data[a].is_object()) return out;
    if (!data[a].contains(b) || !data[a][b].is_array()) return out;
    for (const auto& v : data[a][b]) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

}  // namespace

Config Config::LoadFromFile(const std::string& path) {
    Config cfg;
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }
    cfg.m_Data = nlohmann::json::parse(file);
    return cfg;
}

Config Config::LoadFromEnv() {
    Config cfg;
    cfg.m_Data = nlohmann::json::object();

    std::string port = GetEnvVar("MCP_SERVER_PORT");
    if (!port.empty()) cfg.m_Data["server"]["port"] = std::stoi(port);

    std::string litellmUrl = GetEnvVar("MCP_LITELLM_URL");
    if (!litellmUrl.empty()) cfg.m_Data["litellm"]["base_url"] = litellmUrl;

    std::string model = GetEnvVar("MCP_DEFAULT_MODEL");
    if (!model.empty()) cfg.m_Data["litellm"]["default_model"] = model;

    std::string authKey = GetEnvVar("MCP_AUTH_API_KEY");
    if (!authKey.empty()) {
        cfg.m_Data["auth"]["enabled"] = true;
        cfg.m_Data["auth"]["api_key"] = authKey;
    }

    std::string profile = GetEnvVar("TOOLSMITH_PROFILE");
    if (!profile.empty()) cfg.m_Data["tools"]["profile"] = profile;

    std::string tools = GetEnvVar("TOOLSMITH_TOOLS");
    if (!tools.empty()) cfg.m_Data["tools"]["pin"] = SplitCsv(tools);

    std::string workspace = GetEnvVar("TOOLSMITH_WORKSPACE");
    if (!workspace.empty()) cfg.m_Data["workspace"]["root"] = workspace;

    return cfg;
}

int Config::GetServerPort() const {
    if (m_Data.contains("server") && m_Data["server"].contains("port")) {
        return m_Data["server"]["port"].get<int>();
    }
    return 9001;
}

std::string Config::GetLiteLlmBaseUrl() const {
    if (m_Data.contains("litellm") && m_Data["litellm"].contains("base_url")) {
        return m_Data["litellm"]["base_url"].get<std::string>();
    }
    return "http://localhost:4000";
}

std::string Config::GetDefaultModel() const {
    if (m_Data.contains("litellm") && m_Data["litellm"].contains("default_model")) {
        return m_Data["litellm"]["default_model"].get<std::string>();
    }
    return "gpt-3.5-turbo";
}

size_t Config::GetThreadPoolSize() const {
    size_t n = std::thread::hardware_concurrency();
    if (m_Data.contains("thread_pool") && m_Data["thread_pool"].contains("size")) {
        n = m_Data["thread_pool"]["size"].get<size_t>();
    }
    if (n == 0) n = 1;
    if (n > 8) n = 8;
    return n;
}

size_t Config::GetMaxRequestBodySize() const {
    if (m_Data.contains("server") && m_Data["server"].contains("max_request_body_bytes")) {
        return m_Data["server"]["max_request_body_bytes"].get<size_t>();
    }
    return 1048576; // 1MB
}

size_t Config::GetRateLimitRequestsPerMinute() const {
    if (m_Data.contains("rate_limit") && m_Data["rate_limit"].contains("requests_per_minute")) {
        return m_Data["rate_limit"]["requests_per_minute"].get<size_t>();
    }
    return 60;
}

bool Config::IsAuthEnabled() const {
    if (m_Data.contains("auth") && m_Data["auth"].contains("enabled")) {
        return m_Data["auth"]["enabled"].get<bool>();
    }
    return false;
}

std::string Config::GetAuthApiKey() const {
    if (m_Data.contains("auth") && m_Data["auth"].contains("api_key")) {
        return m_Data["auth"]["api_key"].get<std::string>();
    }
    return "";
}

std::string Config::GetSkillsDirectory() const {
    if (m_Data.contains("skills") && m_Data["skills"].contains("directory")) {
        return m_Data["skills"]["directory"].get<std::string>();
    }
    return "skills";
}

std::string Config::GetPluginsDirectory() const {
    if (m_Data.contains("plugins") && m_Data["plugins"].contains("directory")) {
        return m_Data["plugins"]["directory"].get<std::string>();
    }
    return "plugins";
}

std::string Config::GetMcpServersConfigPath() const {
    if (m_Data.contains("discovery") && m_Data["discovery"].contains("servers_config")) {
        return m_Data["discovery"]["servers_config"].get<std::string>();
    }
    return "config/mcp_servers.json";
}

std::string Config::GetToolsProfile() const {
    if (m_Data.contains("tools") && m_Data["tools"].contains("profile")
        && m_Data["tools"]["profile"].is_string()) {
        return m_Data["tools"]["profile"].get<std::string>();
    }
    return "auto";
}

std::vector<std::string> Config::GetToolsEnable() const {
    return JsonStringList(m_Data, "tools", "enable");
}

std::vector<std::string> Config::GetToolsDisable() const {
    return JsonStringList(m_Data, "tools", "disable");
}

std::vector<std::string> Config::GetToolsPin() const {
    return JsonStringList(m_Data, "tools", "pin");
}

bool Config::IsChainingEnabled() const {
    if (m_Data.contains("tools") && m_Data["tools"].contains("chaining")) {
        return m_Data["tools"]["chaining"].get<bool>();
    }
    return false;
}

bool Config::AllowCommandSkills() const {
    if (m_Data.contains("tools") && m_Data["tools"].contains("allow_command_skills")) {
        return m_Data["tools"]["allow_command_skills"].get<bool>();
    }
    return false;
}

std::string Config::GetWorkspaceRoot() const {
    if (m_Data.contains("workspace") && m_Data["workspace"].contains("root")
        && m_Data["workspace"]["root"].is_string()) {
        return m_Data["workspace"]["root"].get<std::string>();
    }
    return "auto";
}

std::vector<std::string> Config::GetWorkspaceAllow() const {
    return JsonStringList(m_Data, "workspace", "allow");
}

const nlohmann::json& Config::GetRaw() const {
    return m_Data;
}
