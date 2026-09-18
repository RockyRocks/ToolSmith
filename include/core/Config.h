#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>
#include <fstream>
#include <vector>

class Config {
public:
    static Config LoadFromFile(const std::string& path);
    static Config LoadFromEnv();

    int GetServerPort() const;
    std::string GetLiteLlmBaseUrl() const;
    std::string GetDefaultModel() const;
    size_t GetThreadPoolSize() const;
    size_t GetMaxRequestBodySize() const;
    size_t GetRateLimitRequestsPerMinute() const;
    bool IsAuthEnabled() const;
    std::string GetAuthApiKey() const;
    std::string GetSkillsDirectory() const;
    std::string GetPluginsDirectory() const;
    std::string GetMcpServersConfigPath() const;

    /// tools.profile: auto|core|cpp|csharp|fullstack|all (default auto)
    std::string GetToolsProfile() const;
    std::vector<std::string> GetToolsEnable() const;
    std::vector<std::string> GetToolsDisable() const;
    std::vector<std::string> GetToolsPin() const;
    /// Default false — ExecuteWithChaining is a no-op wrapper on the server path.
    bool IsChainingEnabled() const;
    bool AllowCommandSkills() const;
    std::string GetWorkspaceRoot() const;
    std::vector<std::string> GetWorkspaceAllow() const;

    const nlohmann::json& GetRaw() const;

private:
    nlohmann::json m_Data = nlohmann::json::object();
};
