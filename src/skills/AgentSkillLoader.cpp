#include <skills/AgentSkillLoader.h>
#include <skills/YamlFrontmatter.h>
#include <core/Logger.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

bool IsSpecSkillName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '-' || name.back() == '-') return false;
    if (name.find("--") != std::string::npos) return false;
    for (unsigned char c : name) {
        if (!(std::islower(c) || std::isdigit(c) || c == '-')) return false;
    }
    return true;
}

static std::string ReadFile(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool SkipDirName(const std::string& name) {
    return name == ".git" || name == "node_modules" || name == ".cursor"
        || name == "__pycache__";
}

static void LoadOneSkill(const fs::path& skillDir,
                         AgentSkillDiscovery& out,
                         std::unordered_set<std::string>& seen) {
    std::error_code ec;
    const fs::path skillMd = skillDir / "SKILL.md";
    if (!fs::exists(skillMd, ec) || !fs::is_regular_file(skillMd, ec)) return;

    const std::string dirName = skillDir.filename().string();
    YamlFrontmatter fm = ParseYamlFrontmatter(ReadFile(skillMd));
    if (!fm.ok()) {
        out.skipped.push_back(skillMd.string() + ": " + fm.error);
        return;
    }
    std::string desc = fm.GetString("description");
    if (desc.empty()) {
        out.skipped.push_back(skillMd.string() + ": description is required");
        return;
    }
    std::string name = fm.GetString("name");
    if (name.empty()) {
        out.skipped.push_back(skillMd.string() + ": name is required");
        return;
    }
    if (seen.count(name)) {
        Logger::GetInstance().Log("[AgentSkills] '" + name
            + "' shadowed by an earlier discovery scope");
        return;
    }

    AgentSkill skill;
    skill.m_Name = name;
    skill.m_Description = desc.size() > 1024 ? desc.substr(0, 1024) : desc;
    skill.m_Location = fs::weakly_canonical(skillMd, ec).string();
    if (skill.m_Location.empty()) skill.m_Location = skillMd.string();
    skill.m_Root = skillDir.string();
    skill.m_License = fm.GetString("license");
    skill.m_Compatibility = fm.GetString("compatibility");
    skill.m_AllowedTools = fm.GetString("allowed-tools");
    if (skill.m_AllowedTools.empty()) {
        auto list = fm.GetStringList("allowed-tools");
        std::ostringstream joined;
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) joined << ' ';
            joined << list[i];
        }
        skill.m_AllowedTools = joined.str();
    }
    skill.m_Metadata = fm.GetStringMap("metadata");
    if (!IsSpecSkillName(name)) {
        skill.m_Warnings.push_back(
            "name does not match agentskills.io charset/length rules");
        Logger::GetInstance().Log("[AgentSkills] warning: " + skill.m_Warnings.back()
                                  + " (" + name + ")");
    }
    if (name != dirName) {
        skill.m_Warnings.push_back("name '" + name + "' does not match directory '"
                                   + dirName + "'");
        Logger::GetInstance().Log("[AgentSkills] warning: " + skill.m_Warnings.back());
    }
    if (desc.size() > 1024) {
        skill.m_Warnings.push_back("description truncated to 1024 characters");
    }
    seen.insert(name);
    out.skills.push_back(std::move(skill));
}

static void ScanSkillsDir(const fs::path& dir,
                          AgentSkillDiscovery& out,
                          std::unordered_set<std::string>& seen,
                          int depth = 0) {
    std::error_code ec;
    if (depth > 4) return;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

    // A skills root may itself contain SKILL.md (unusual); only treat nested dirs.
    for (fs::directory_iterator it(dir, ec); it != fs::directory_iterator() && !ec; ++it) {
        if (!it->is_directory(ec)) continue;
        const fs::path skillDir = it->path();
        const std::string dirName = skillDir.filename().string();
        if (SkipDirName(dirName)) continue;
        LoadOneSkill(skillDir, out, seen);
        ScanSkillsDir(skillDir, out, seen, depth + 1);
    }
}

AgentSkillDiscovery DiscoverAgentSkills(const std::string& workspaceRoot,
                                        const std::string& homeDir,
                                        const std::string& bundledDir) {
    AgentSkillDiscovery out;
    std::unordered_set<std::string> seen;
    fs::path ws(workspaceRoot);
    fs::path home(homeDir);

    // Project (client-native, then cross-client, then .claude) then user, then bundled.
    if (!ws.empty()) {
        ScanSkillsDir(ws / ".toolsmith" / "skills", out, seen);
        ScanSkillsDir(ws / ".agents" / "skills", out, seen);
        ScanSkillsDir(ws / ".claude" / "skills", out, seen);
    }
    if (!home.empty()) {
        ScanSkillsDir(home / ".toolsmith" / "skills", out, seen);
        ScanSkillsDir(home / ".agents" / "skills", out, seen);
        ScanSkillsDir(home / ".claude" / "skills", out, seen);
    }
    if (!bundledDir.empty()) ScanSkillsDir(fs::path(bundledDir), out, seen);

    return out;
}

AgentSkillBody LoadAgentSkillBody(const AgentSkillDiscovery& catalog,
                                  const std::string& name) {
    AgentSkillBody out;
    if (name.empty()) {
        out.error = "Missing required argument: name";
        return out;
    }
    const AgentSkill* found = nullptr;
    for (const auto& s : catalog.skills) {
        if (s.m_Name == name) { found = &s; break; }
    }
    if (!found) {
        out.error = "Unknown skill: " + name;
        return out;
    }
    std::ifstream in(found->m_Location);
    if (!in) {
        out.error = "Cannot read " + found->m_Location;
        return out;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    YamlFrontmatter fm = ParseYamlFrontmatter(ss.str());
    if (!fm.ok()) {
        out.error = fm.error;
        return out;
    }
    out.body = fm.body;
    out.root = found->m_Root;
    out.allowedTools = found->m_AllowedTools;
    return out;
}
