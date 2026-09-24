#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// Workspace root + allowlist. Writes/search/shell stay jailed.
/// Reads of absolute paths outside the jail are allowed but flagged.
class WorkspaceJail {
public:
    struct ResolveResult {
        std::filesystem::path path;
        bool outsideWorkspace = false;
        std::string error;
        bool ok() const { return error.empty(); }
    };

    static WorkspaceJail Open(const std::filesystem::path& root,
                              const std::vector<std::filesystem::path>& allow = {});

    static std::filesystem::path DetectRoot(const std::filesystem::path& start);

    const std::filesystem::path& Root() const { return m_Root; }
    const std::vector<std::filesystem::path>& Allow() const { return m_Allow; }

    /// Resolve a user path. Relative paths are rooted at the workspace.
    ResolveResult Resolve(const std::string& userPath, bool forWrite) const;

    bool Contains(const std::filesystem::path& canonical) const;

private:
    std::filesystem::path m_Root;
    std::vector<std::filesystem::path> m_Allow;
};
