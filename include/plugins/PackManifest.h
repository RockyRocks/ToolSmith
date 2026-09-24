#pragma once

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

/// Fields read from a plugin directory's plugin.json.
/// `id` defaults to the directory name. `packs` lists profile ids that must
/// be active before the binary or script is loaded. Empty `packs` means the
/// plugin is not part of the default session (opt-in only) when gating is on.
struct PackManifest {
    std::string id;
    std::string description;
    std::vector<std::string> packs;
    std::vector<std::string> os;
    std::vector<std::string> tools;
    std::string runtime;
    bool present = false;

    static PackManifest FromDirectory(const fs::path& pluginDir) {
        PackManifest m;
        m.id = pluginDir.filename().string();
        fs::path jsonPath = pluginDir / "plugin.json";
        std::ifstream in(jsonPath);
        if (!in) return m;
        nlohmann::json j;
        try { in >> j; } catch (...) { return m; }
        m.present = true;
        if (j.contains("id") && j["id"].is_string()) m.id = j["id"].get<std::string>();
        else if (j.contains("name") && j["name"].is_string()) m.id = j["name"].get<std::string>();
        if (j.contains("description") && j["description"].is_string())
            m.description = j["description"].get<std::string>();
        auto takeList = [&](const char* key, std::vector<std::string>& out) {
            if (!j.contains(key)) return;
            if (j[key].is_string()) out.push_back(j[key].get<std::string>());
            else if (j[key].is_array()) {
                for (const auto& v : j[key])
                    if (v.is_string()) out.push_back(v.get<std::string>());
            }
        };
        takeList("packs", m.packs);
        if (m.packs.empty()) takeList("profile", m.packs);
        takeList("os", m.os);
        if (m.os.empty()) takeList("platforms", m.os);
        takeList("tools", m.tools);
        if (j.contains("runtime") && j["runtime"].is_string())
            m.runtime = j["runtime"].get<std::string>();
        return m;
    }

    bool OsMatches() const {
        if (os.empty()) return true;
        for (const auto& name : os) {
            if (name == "any" || name == "*") return true;
#ifdef _WIN32
            if (name == "win32" || name == "windows") return true;
#elif defined(__APPLE__)
            if (name == "darwin" || name == "macos") return true;
#else
            if (name == "linux") return true;
#endif
        }
        return false;
    }

    /// When `gate` is false every manifest loads. When true, the plugin loads
    /// only if one of its pack ids is in `active`.
    bool Allowed(const std::unordered_set<std::string>& active, bool gate) const {
        if (!OsMatches()) return false;
        if (!gate) return true;
        if (packs.empty()) return false;
        for (const auto& p : packs) {
            if (active.count(p)) return true;
        }
        return false;
    }
};
