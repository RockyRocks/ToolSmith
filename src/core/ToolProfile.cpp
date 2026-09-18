#include <core/ToolProfile.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace fs = std::filesystem;

namespace ToolProfile {

static std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Detect(const std::string& workspaceRoot) {
    std::error_code ec;
    fs::path root = workspaceRoot.empty() ? fs::current_path(ec) : fs::path(workspaceRoot);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return "core";

    bool cpp = fs::exists(root / "CMakeLists.txt", ec)
            || fs::exists(root / "compile_commands.json", ec);
    bool csharp = false;

    for (fs::directory_iterator it(root, ec); it != fs::directory_iterator() && !ec; ++it) {
        if (!it->is_regular_file(ec)) continue;
        auto ext = Lower(it->path().extension().string());
        if (ext == ".vcxproj") cpp = true;
        if (ext == ".csproj" || ext == ".sln") csharp = true;
    }
    if (cpp && csharp) return "fullstack";
    if (cpp) return "cpp";
    if (csharp) return "csharp";
    return "core";
}

std::string Normalize(std::string raw) {
    raw = Lower(std::move(raw));
    if (raw.empty()) return "auto";
    if (raw == "auto" || raw == "core" || raw == "cpp" || raw == "csharp"
        || raw == "fullstack" || raw == "all") {
        return raw;
    }
    return {};
}

std::unordered_set<std::string> ToolsForProfile(const std::string& profile) {
    std::unordered_set<std::string> tools;
    if (profile == "all") return tools; // empty means unrestricted
    for (const char* t : kCoreTools) tools.insert(t);
    if (profile == "cpp" || profile == "csharp" || profile == "fullstack") {
        for (const char* t : kLanguageExtra) tools.insert(t);
    }
    return tools;
}

bool IsOptionalPack(const std::string& packId) {
    static const std::unordered_set<std::string> kOptional = {
        "github", "github-actions", "jira", "unity", "unreal", "skills",
        "llm", "remote", "memory", "desktop-notify", "entrian", "everything",
        "git_write", "filesystem", "shell-bg"
    };
    return kOptional.count(packId) > 0;
}

static const std::unordered_map<std::string, std::vector<std::string>> kPackTools = {
    {"git_write", {"git_write"}},
    {"github", {}},
    {"skills", {"skills", "skill"}},
    {"llm", {"llm"}},
    {"remote", {"remote"}},
};

ResolveOutput Resolve(const ResolveInput& in) {
    ResolveOutput out;
    std::string profile = Normalize(in.profile.empty() ? "auto" : in.profile);
    if (profile.empty()) {
        out.error = "Unknown profile: " + in.profile
                    + " (want auto|core|cpp|csharp|fullstack|all)";
        return out;
    }
    if (profile == "auto") profile = Detect(in.workspaceRoot);
    out.profile = profile;
    out.loadAllPlugins = (profile == "all");

    if (profile == "all") {
        if (!in.pinTools.empty()) {
            for (const auto& t : in.pinTools) {
                if (t.empty()) {
                    out.error = "Empty tool name in --tools";
                    return out;
                }
                out.advertised.insert(t);
            }
        }
        return out;
    }

    out.advertised = ToolsForProfile(profile);

    for (const auto& pack : in.enable) {
        if (pack.empty()) {
            out.error = "Empty pack id in tools.enable";
            return out;
        }
        auto it = kPackTools.find(pack);
        if (it != kPackTools.end()) {
            for (const auto& t : it->second) out.advertised.insert(t);
        } else if (IsOptionalPack(pack)) {
            // Known optional pack with no in-process tools yet (script packs).
            continue;
        } else {
            out.error = "Unknown pack id: " + pack;
            return out;
        }
    }
    for (const auto& pack : in.disable) {
        auto it = kPackTools.find(pack);
        if (it != kPackTools.end()) {
            for (const auto& t : it->second) out.advertised.erase(t);
        }
    }

    if (!in.pinTools.empty()) {
        std::unordered_set<std::string> pin;
        for (const auto& t : in.pinTools) {
            if (t.empty()) {
                out.error = "Empty tool name in --tools";
                return out;
            }
            if (!out.advertised.count(t) && t != "echo") {
                // pin must be subset of installed/advertised candidates OR core
                bool known = false;
                for (const char* c : kCoreTools) if (t == c) known = true;
                for (const char* c : kLanguageExtra) if (t == c) known = true;
                if (t == "skills" || t == "skill" || t == "llm" || t == "remote" || t == "echo")
                    known = true;
                if (!known) {
                    out.error = "Unknown tool in --tools: " + t;
                    return out;
                }
            }
            pin.insert(t);
        }
        out.advertised = std::move(pin);
    }
    return out;
}

}  // namespace ToolProfile
