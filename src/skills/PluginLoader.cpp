#include <skills/PluginLoader.h>
#include <skills/YamlFrontmatter.h>
#include <core/Logger.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

}  // namespace

SkillDefinition PluginLoader::ParseSkillMd(const std::string& content,
                                            const std::string& fallbackName,
                                            const std::string& pluginDir) {
    YamlFrontmatter fm = ParseYamlFrontmatter(content);
    if (!fm.ok()) {
        throw std::runtime_error(fm.error);
    }

    SkillDefinition skill;
    std::string name = fm.GetString("name");
    skill.m_Name             = name.empty() ? fallbackName : name;
    skill.m_Description      = fm.GetString("description");
    skill.m_PromptTemplate   = fm.body;
    skill.m_RequiredVariables = fm.GetStringList("variables");
    skill.m_Rules            = fm.GetStringList("rules");
    skill.m_DefaultModel     = "";

    std::string type = fm.GetString("type");
    skill.m_Type = (type == "command") ? SkillType::Command : SkillType::LLM;
    if (skill.m_Type == SkillType::Command) {
        std::string ct = fm.GetString("command_template");
        if (!pluginDir.empty()) {
            const std::string marker = "${PLUGIN_DIR}";
            size_t pos;
            while ((pos = ct.find(marker)) != std::string::npos)
                ct.replace(pos, marker.size(), pluginDir);
        }
        skill.m_CommandTemplate = std::move(ct);
    }

    if (skill.m_Name.empty()) {
        throw std::runtime_error("SKILL.md has no 'name' field and no fallback name was given");
    }

    return skill;
}

std::vector<std::string> PluginLoader::LoadIntoEngine(const std::string& pluginsDir,
                                                       SkillEngine& engine) {
    std::vector<std::string> loadedNames;
    if (!fs::exists(pluginsDir) || !fs::is_directory(pluginsDir)) {
        Logger::GetInstance().Log("Plugins directory not found: " + pluginsDir + " (skipping)");
        return loadedNames;
    }

    for (const auto& pluginEntry : fs::directory_iterator(pluginsDir)) {
        if (!pluginEntry.is_directory()) continue;

        fs::path pluginDir = pluginEntry.path();
        fs::path skillsDir = pluginDir / "skills";

        if (!fs::exists(skillsDir) || !fs::is_directory(skillsDir)) continue;

        for (const auto& skillEntry : fs::directory_iterator(skillsDir)) {
            if (!skillEntry.is_directory()) continue;

            fs::path skillMdPath = skillEntry.path() / "SKILL.md";
            if (!fs::exists(skillMdPath)) continue;

            std::string fallbackName = skillEntry.path().filename().string();

            try {
                std::string content = ReadFile(skillMdPath);
                SkillDefinition skill = ParseSkillMd(content, fallbackName, pluginDir.string());
                engine.LoadSkill(skill);
                loadedNames.push_back(skill.m_Name);
                Logger::GetInstance().Log("Loaded plugin skill: " + skill.m_Name
                                          + " (from " + skillMdPath.string() + ")");
            } catch (const std::exception& e) {
                Logger::GetInstance().Log("Failed to load plugin skill from "
                                          + skillMdPath.string() + ": " + e.what());
            }
        }
    }

    Logger::GetInstance().Log("PluginLoader: loaded " + std::to_string(loadedNames.size())
                              + " plugin skill(s)");
    return loadedNames;
}
