#pragma once
#include <commands/CommandRegistry.h>
#include <core/WorkspaceJail.h>
#include <skills/AgentSkillLoader.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

/// Shared state for in-process core tools.
struct CoreRuntime {
    WorkspaceJail jail;
    std::string profile;
    std::unordered_set<std::string> advertised;
    std::unordered_set<std::string> locked; // cannot deactivate
    AgentSkillDiscovery skills;
    CommandRegistry* registry = nullptr;
    std::mutex mutex;

    void SyncAdvertised() {
        if (!registry) return;
        registry->SetAdvertised(advertised);
    }
};

/// Register read/edit/search/shell/catalog/activate/deactivate/project/git/build/test/diagnose.
void RegisterCoreTools(CommandRegistry& registry,
                       const std::shared_ptr<CoreRuntime>& runtime);
