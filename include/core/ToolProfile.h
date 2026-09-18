#pragma once
#include <string>
#include <unordered_set>
#include <vector>

/// Profile + pin resolution for the advertised MCP tool set.
namespace ToolProfile {

inline constexpr const char* kCoreTools[] = {
    "read", "edit", "search", "shell", "catalog", "activate", "deactivate", "project"
};

inline constexpr const char* kLanguageExtra[] = {
    "git", "build", "test", "diagnose"
};

/// Cheap scan of workspace root filenames only.
std::string Detect(const std::string& workspaceRoot);

/// Canonical profile name: auto|core|cpp|csharp|fullstack|all
std::string Normalize(std::string raw);

/// Tools advertised for a resolved profile (not "auto").
std::unordered_set<std::string> ToolsForProfile(const std::string& profile);

/// Packs that must never auto-enable.
bool IsOptionalPack(const std::string& packId);

struct ResolveInput {
    std::string profile;                 // auto/core/cpp/csharp/fullstack/all
    std::vector<std::string> enable;     // pack ids
    std::vector<std::string> disable;    // pack ids
    std::vector<std::string> pinTools;   // --tools, empty = unused
    std::string workspaceRoot;
};

struct ResolveOutput {
    std::string profile;
    std::unordered_set<std::string> advertised;
    std::string error;                   // non-empty = fail closed
    bool loadAllPlugins = false;         // profile == all
};

ResolveOutput Resolve(const ResolveInput& in);

}  // namespace ToolProfile
