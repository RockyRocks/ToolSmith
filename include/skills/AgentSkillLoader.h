#pragma once
#include <map>
#include <string>
#include <vector>

/// Progressive-disclosure Agent Skill (agentskills.io) — catalog row only.
struct AgentSkill {
    std::string m_Name;
    std::string m_Description;
    std::string m_Location;     // absolute SKILL.md
    std::string m_Root;         // parent of SKILL.md
    std::string m_License;
    std::string m_Compatibility;
    std::string m_AllowedTools;
    std::map<std::string, std::string> m_Metadata;
    std::vector<std::string> m_Warnings;
};

struct AgentSkillDiscovery {
    std::vector<AgentSkill> skills;
    std::vector<std::string> skipped;  // fail-closed messages
};

/// Spec name: 1-64 chars, [a-z0-9-], no leading/trailing hyphen, no `--`.
bool IsSpecSkillName(const std::string& name);

/// Scan `.agents/skills` and `.toolsmith/skills` (project then user), then bundled.
/// Project overrides user. Client-native (`.toolsmith`) overrides `.agents` at the same scope.
AgentSkillDiscovery DiscoverAgentSkills(const std::string& workspaceRoot,
                                        const std::string& homeDir,
                                        const std::string& bundledDir = "");

/// Load SKILL.md body (tier 2). Returns error in `error` when it fails.
struct AgentSkillBody {
    std::string body;
    std::string root;
    std::string allowedTools;
    std::string error;
};
AgentSkillBody LoadAgentSkillBody(const AgentSkillDiscovery& catalog,
                                  const std::string& name);
