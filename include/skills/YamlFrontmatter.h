#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

/// Indent-aware YAML frontmatter subset used by Agent Skills SKILL.md files.
/// Handles nested string maps, lists, quoted values, and unquoted colons in
/// scalar values (the agentskills.io "when: the user asks" compatibility case).
struct YamlFrontmatter {
    nlohmann::json fields = nlohmann::json::object();
    std::string body;
    std::string error;

    bool ok() const { return error.empty(); }

    std::string GetString(const std::string& key) const;
    std::vector<std::string> GetStringList(const std::string& key) const;
    std::map<std::string, std::string> GetStringMap(const std::string& key) const;
};

YamlFrontmatter ParseYamlFrontmatter(const std::string& content);
