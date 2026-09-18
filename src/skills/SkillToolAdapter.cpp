#include <skills/SkillToolAdapter.h>
#include <skills/SkillEngine.h>
#include <core/Logger.h>
#include <core/ResultBudget.h>

#include <cstdio>
#include <sstream>
#include <string>

namespace {

// Max output captured from command subprocess (4 MiB)
constexpr size_t kMaxOutput = 4 * 1024 * 1024;

/// Shell-escape a user-provided value before interpolation into a command template.
/// This prevents injection: the value is always treated as a single literal token.
std::string ShellEscape(const std::string& value) {
#ifdef _WIN32
    // cmd.exe: wrap in double quotes, escape internal quotes and % expansion
    std::string escaped = "\"";
    for (char c : value) {
        if (c == '"')       escaped += "\\\"";
        else if (c == '%')  escaped += "%%";
        else                escaped += c;
    }
    escaped += "\"";
    return escaped;
#else
    // POSIX sh: wrap in single quotes, escape embedded single quotes
    std::string escaped = "'";
    for (char c : value) {
        if (c == '\'') escaped += "'\\''";
        else           escaped += c;
    }
    escaped += "'";
    return escaped;
#endif
}

/// Run a command via the shell (popen). Variable values MUST be shell-escaped
/// before interpolation into the command string.
std::pair<std::string, int> RunShellCommand(const std::string& cmd) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe)
        return {"", -1};

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
        if (output.size() >= kMaxOutput) {
            output.resize(kMaxOutput);
            break;
        }
    }

#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif
    return {output, rc};
}

}  // anonymous namespace

SkillToolAdapter::SkillToolAdapter(SkillDefinition def,
                                   std::shared_ptr<ILLMProvider> provider,
                                   ToolSource source)
    : m_Def(std::move(def)), m_Provider(std::move(provider)), m_Source(source) {}

std::future<nlohmann::json> SkillToolAdapter::ExecuteAsync(const nlohmann::json& request) {
    auto def = m_Def;
    auto provider = m_Provider;

    return std::async(std::launch::async, [def, provider, request]() -> nlohmann::json {
        nlohmann::json variables = request.value("payload", nlohmann::json::object());

        if (def.m_Type == SkillType::Command) {
            // Shell-escape all user-provided variable values before interpolation
            nlohmann::json escapedVars = nlohmann::json::object();
            for (auto& [key, val] : variables.items()) {
                if (val.is_string())
                    escapedVars[key] = ShellEscape(val.get<std::string>());
                else
                    escapedVars[key] = val;
            }

            SkillDefinition cmdDef = def;
            cmdDef.m_PromptTemplate = def.m_CommandTemplate;
            std::string cmd;
            try {
                cmd = SkillEngine::StaticRenderPrompt(cmdDef, escapedVars);
            } catch (const std::invalid_argument& e) {
                return {{"status", "error"}, {"error", e.what()}};
            }

            if (cmd.empty())
                return {{"status", "error"}, {"error", "Empty command after interpolation"}};

            auto [output, rc] = RunShellCommand(cmd);

            return nlohmann::json{
                {"status",    "ok"},
                {"skill",     def.m_Name},
                {"content",   output.empty() ? "(no results)" : output},
                {"exit_code", rc}
            };
        }

        std::string prompt;
        try {
            prompt = SkillEngine::StaticRenderPrompt(def, variables);
        } catch (const std::invalid_argument& e) {
            return {{"status", "error"}, {"error", e.what()}};
        }

        std::string model = variables.value("model", def.m_DefaultModel);

        LLMRequest llmReq;
        llmReq.m_Model = model;

        nlohmann::json messages = nlohmann::json::array();
        std::string systemContent;
        if (!def.m_SystemPrompt.empty()) {
            systemContent = def.m_SystemPrompt;
        }
        if (!def.m_Rules.empty()) {
            if (!systemContent.empty()) systemContent += "\n\n";
            systemContent += "Rules:\n";
            for (size_t i = 0; i < def.m_Rules.size(); ++i) {
                systemContent += std::to_string(i + 1) + ". " + def.m_Rules[i] + "\n";
            }
        }
        if (!systemContent.empty()) {
            messages.push_back({{"role", "system"}, {"content", systemContent}});
        }
        messages.push_back({{"role", "user"}, {"content", prompt}});

        llmReq.m_Messages = messages;
        llmReq.m_Parameters = def.m_DefaultParameters;

        if (request.contains("payload") && request["payload"].contains("parameters")) {
            for (const auto& [k, v] : request["payload"]["parameters"].items()) {
                llmReq.m_Parameters[k] = v;
            }
        }

        try {
            auto future = provider->Complete(llmReq);
            auto resp = future.get();
            return nlohmann::json{
                {"status", "ok"},
                {"skill", def.m_Name},
                {"content", resp.m_Content},
                {"model", model},
                {"input_tokens", resp.m_InputTokens},
                {"output_tokens", resp.m_OutputTokens}
            };
        } catch (const std::exception& e) {
            Logger::GetInstance().Log(std::string("SkillToolAdapter LLM error: ") + e.what());
            return {{"status", "error"}, {"error", e.what()}};
        }
    });
}

ToolMetadata SkillToolAdapter::GetMetadata() const {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();

    for (const auto& var : m_Def.m_RequiredVariables) {
        properties[var] = {{"type", "string"}, {"description", "Required: " + var}};
        required.push_back(var);
    }
    properties["model"] = {{"type", "string"}, {"description", "Override the default LLM model"}};
    properties["parameters"] = {{"type", "object"}, {"description", "Override default LLM parameters (temperature, max_tokens, ...)"}};

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", properties},
        {"required", required}
    };

    ToolMetadata meta;
    meta.m_Name = m_Def.m_Name;
    std::string desc = m_Def.m_Description;
    if (desc.size() > kDescriptionMaxChars) desc.resize(kDescriptionMaxChars);
    meta.m_Description = std::move(desc);
    meta.m_InputSchema = std::move(schema);
    meta.m_DefaultModel = m_Def.m_DefaultModel;
    meta.m_DefaultParameters = m_Def.m_DefaultParameters;
    meta.m_Source = m_Source;
    meta.m_Hidden = false;
    return meta;
}
