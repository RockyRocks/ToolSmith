#pragma once
#include <commands/ICommandStrategy.h>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CommandRegistry {
public:
    static constexpr int kMaxChainDepth = 5;

    /// Install a tool. Duplicate names are a hard error (fail-closed).
    void RegisterCommand(const std::string& name, std::shared_ptr<ICommandStrategy> command);

    /// Overwrite an existing tool. Used only for plugin hot-reload of the same name.
    void ReplaceCommand(const std::string& name, std::shared_ptr<ICommandStrategy> command);

    std::shared_ptr<ICommandStrategy> Resolve(const std::string& name) const;
    bool HasCommand(const std::string& name) const;
    std::vector<std::string> ListCommands() const;
    std::vector<ToolMetadata> ListToolMetadata() const;

    /// When a filter is active, tools/list only includes these names (plus hidden skip).
    /// An empty filter means "advertise everything that is not hidden".
    void SetAdvertised(std::unordered_set<std::string> names);
    void ClearAdvertisedFilter();
    void Advertise(const std::string& name);
    void Unadvertise(const std::string& name);
    bool HasAdvertisedFilter() const;
    bool IsAdvertised(const std::string& name) const;
    std::unordered_set<std::string> AdvertisedNames() const;

    void SetChainingEnabled(bool enabled);
    bool IsChainingEnabled() const;

    nlohmann::json ExecuteWithChaining(const std::string& toolName,
                                       const nlohmann::json& request,
                                       int depth = 0);

private:
    mutable std::shared_mutex m_Mutex;
    std::unordered_map<std::string, std::shared_ptr<ICommandStrategy>> m_Commands;
    std::unordered_set<std::string> m_Advertised;
    bool m_FilterAdvertised = false;
    bool m_ChainingEnabled = true;  // unit tests cover chaining; server_entry turns this off
};
