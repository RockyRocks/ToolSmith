#include <commands/CommandRegistry.h>
#include <core/Logger.h>
#include <core/ResultBudget.h>
#include <stdexcept>

void CommandRegistry::RegisterCommand(const std::string& name,
                                       std::shared_ptr<ICommandStrategy> command) {
    if (name.empty()) {
        throw std::invalid_argument("Command name cannot be empty");
    }
    std::unique_lock lock(m_Mutex);
    if (m_Commands.count(name)) {
        throw std::runtime_error(
            "Tool name collision: '" + name + "' is already registered");
    }
    m_Commands[name] = std::move(command);
}

void CommandRegistry::ReplaceCommand(const std::string& name,
                                     std::shared_ptr<ICommandStrategy> command) {
    if (name.empty()) {
        throw std::invalid_argument("Command name cannot be empty");
    }
    std::shared_ptr<ICommandStrategy> old;
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Commands.find(name);
        if (it != m_Commands.end()) {
            old = std::move(it->second);
        }
        m_Commands[name] = std::move(command);
    }
    if (old) old->Shutdown();
}

std::shared_ptr<ICommandStrategy> CommandRegistry::Resolve(const std::string& name) const {
    std::shared_lock lock(m_Mutex);
    auto it = m_Commands.find(name);
    if (it == m_Commands.end()) {
        return nullptr;
    }
    return it->second;
}

bool CommandRegistry::HasCommand(const std::string& name) const {
    std::shared_lock lock(m_Mutex);
    return m_Commands.count(name) > 0;
}

std::vector<std::string> CommandRegistry::ListCommands() const {
    std::shared_lock lock(m_Mutex);
    std::vector<std::string> names;
    names.reserve(m_Commands.size());
    for (const auto& [name, _] : m_Commands) {
        names.push_back(name);
    }
    return names;
}

void CommandRegistry::SetAdvertised(std::unordered_set<std::string> names) {
    std::unique_lock lock(m_Mutex);
    m_Advertised = std::move(names);
    m_FilterAdvertised = true;
}

void CommandRegistry::ClearAdvertisedFilter() {
    std::unique_lock lock(m_Mutex);
    m_Advertised.clear();
    m_FilterAdvertised = false;
}

void CommandRegistry::Advertise(const std::string& name) {
    if (name.empty()) return;
    std::unique_lock lock(m_Mutex);
    m_FilterAdvertised = true;
    m_Advertised.insert(name);
}

void CommandRegistry::Unadvertise(const std::string& name) {
    std::unique_lock lock(m_Mutex);
    m_Advertised.erase(name);
}

bool CommandRegistry::HasAdvertisedFilter() const {
    std::shared_lock lock(m_Mutex);
    return m_FilterAdvertised;
}

bool CommandRegistry::IsAdvertised(const std::string& name) const {
    std::shared_lock lock(m_Mutex);
    if (!m_FilterAdvertised) return true;
    return m_Advertised.count(name) > 0;
}

std::unordered_set<std::string> CommandRegistry::AdvertisedNames() const {
    std::shared_lock lock(m_Mutex);
    return m_Advertised;
}

void CommandRegistry::SetChainingEnabled(bool enabled) {
    std::unique_lock lock(m_Mutex);
    m_ChainingEnabled = enabled;
}

bool CommandRegistry::IsChainingEnabled() const {
    std::shared_lock lock(m_Mutex);
    return m_ChainingEnabled;
}

nlohmann::json CommandRegistry::ExecuteWithChaining(
    const std::string& toolName,
    const nlohmann::json& request,
    int depth)
{
    auto cmd = Resolve(toolName);
    if (!cmd)
        return {{"status", "error"}, {"error", "Unknown command: " + toolName}};

    nlohmann::json result = cmd->ExecuteAsync(request).get();

    bool chaining;
    {
        std::shared_lock lock(m_Mutex);
        chaining = m_ChainingEnabled;
    }
    if (!chaining) return result;

    if (depth < kMaxChainDepth
        && result.contains("chain") && result["chain"].is_object())
    {
        const auto& chain    = result["chain"];
        std::string nextTool = chain.value("tool", "");
        nlohmann::json nextArgs = chain.value("args", nlohmann::json::object());

        if (!nextTool.empty()) {
            nlohmann::json nextReq = {{"command", nextTool}, {"payload", nextArgs}};
            return ExecuteWithChaining(nextTool, nextReq, depth + 1);
        }
    } else if (depth >= kMaxChainDepth && result.contains("chain")) {
        Logger::GetInstance().Log(
            "[Chain] Max depth " + std::to_string(kMaxChainDepth)
            + " reached — stopping chain from " + toolName);
    }

    return result;
}

std::vector<ToolMetadata> CommandRegistry::ListToolMetadata() const {
    std::shared_lock lock(m_Mutex);
    std::vector<ToolMetadata> result;
    result.reserve(m_Commands.size());
    for (const auto& [name, cmd] : m_Commands) {
        if (m_FilterAdvertised && m_Advertised.count(name) == 0) continue;
        ToolMetadata meta = cmd->GetMetadata();
        if (meta.m_Hidden) continue;
        if (meta.m_Name.empty()) {
            meta.m_Name = name;
        }
        if (meta.m_Description.empty()) {
            meta.m_Description = "Execute the " + name + " command";
        }
        if (meta.m_Description.size() > kDescriptionMaxChars) {
            meta.m_Description.resize(kDescriptionMaxChars);
        }
        if (meta.m_InputSchema.is_null() || meta.m_InputSchema.empty()) {
            meta.m_InputSchema = {
                {"type", "object"},
                {"properties", nlohmann::json::object()},
                {"additionalProperties", true}
            };
        }
        result.push_back(std::move(meta));
    }
    return result;
}
