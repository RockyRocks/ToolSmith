#include <core/WorkspaceJail.h>
#include <stdexcept>

namespace fs = std::filesystem;

static fs::path Canonical(const fs::path& p, bool mustExist) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) return {};
    if (mustExist) {
        fs::path c = fs::canonical(abs, ec);
        return ec ? fs::path{} : c;
    }
    // canonical of existing prefix + remainder
    fs::path c = fs::weakly_canonical(abs, ec);
    return ec ? abs : c;
}

std::filesystem::path WorkspaceJail::DetectRoot(const std::filesystem::path& start) {
    std::error_code ec;
    fs::path cur = fs::absolute(start.empty() ? fs::current_path() : start, ec);
    if (ec) cur = start;
    fs::path walk = cur;
    while (true) {
        if (fs::exists(walk / ".git", ec)) return Canonical(walk, true);
        if (walk == walk.parent_path()) break;
        walk = walk.parent_path();
    }
    return Canonical(cur, false);
}

WorkspaceJail WorkspaceJail::Open(const std::filesystem::path& root,
                                  const std::vector<std::filesystem::path>& allow) {
    WorkspaceJail jail;
    auto resolved = Canonical(root.empty() ? fs::current_path() : root, false);
    if (resolved.empty() || !fs::exists(resolved) || !fs::is_directory(resolved)) {
        throw std::runtime_error("Workspace root does not exist or is not a directory: "
                                 + root.string());
    }
    jail.m_Root = resolved;
    for (const auto& a : allow) {
        auto c = Canonical(a, false);
        if (!c.empty() && fs::exists(c)) jail.m_Allow.push_back(c);
    }
    return jail;
}

bool WorkspaceJail::Contains(const std::filesystem::path& canonical) const {
    auto inside = [](const fs::path& root, const fs::path& p) {
        std::error_code ec;
        auto rel = p.lexically_relative(root);
        if (rel.empty()) return false;
        for (const auto& part : rel) {
            if (part == "..") return false;
        }
        return true;
    };
    if (inside(m_Root, canonical)) return true;
    for (const auto& a : m_Allow) {
        if (inside(a, canonical)) return true;
    }
    return false;
}

WorkspaceJail::ResolveResult WorkspaceJail::Resolve(const std::string& userPath,
                                                    bool forWrite) const {
    ResolveResult out;
    if (userPath.empty()) {
        out.error = "Missing required argument: path";
        return out;
    }
    fs::path raw(userPath);
    fs::path abs = raw.is_absolute() ? raw : (m_Root / raw);
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec) canon = fs::absolute(abs);
    // Reject leftover ".." after weakly_canonical (shouldn't happen) 
    {
        for (const auto& part : canon) {
            if (part == "..") {
                out.error = "Path traversal is not allowed: " + userPath;
                return out;
            }
        }
    }
    const bool inside = Contains(canon);
    if (forWrite && !inside) {
        out.error = "Write path is outside the workspace jail: " + canon.string();
        return out;
    }
    out.path = canon;
    out.outsideWorkspace = !inside;
    return out;
}
