#include <gtest/gtest.h>
#include <skills/PluginLoader.h>
#include <skills/SkillEngine.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ParseSkillMd unit tests (no filesystem)
// ---------------------------------------------------------------------------

TEST(PluginLoaderTest, ParseMinimalSkillMd) {
    std::string content = R"(---
name: my_skill
description: Does something useful
---

Do something with {{input}}.
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Name, "my_skill");
    EXPECT_EQ(skill.m_Description, "Does something useful");
    EXPECT_NE(skill.m_PromptTemplate.find("{{input}}"), std::string::npos);
}

TEST(PluginLoaderTest, ParseSkillMdWithVariablesList) {
    std::string content = R"(---
name: code_review
description: Review code
variables:
  - code
  - language
---

Review {{language}} code: {{code}}
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Name, "code_review");
    ASSERT_EQ(skill.m_RequiredVariables.size(), 2u);
    EXPECT_EQ(skill.m_RequiredVariables[0], "code");
    EXPECT_EQ(skill.m_RequiredVariables[1], "language");
}

TEST(PluginLoaderTest, ParseSkillMdFallbackName) {
    std::string content = R"(---
description: No name in frontmatter
---

Body text.
)";
    auto skill = PluginLoader::ParseSkillMd(content, "fallback");
    EXPECT_EQ(skill.m_Name, "fallback");
}

TEST(PluginLoaderTest, ParseSkillMdThrowsWithoutNameAndFallback) {
    std::string content = R"(---
description: No name in frontmatter
---

Body.
)";
    EXPECT_THROW(PluginLoader::ParseSkillMd(content, ""), std::runtime_error);
}

TEST(PluginLoaderTest, ParseSkillMdThrowsWithoutFrontmatter) {
    std::string content = "# Just a markdown file\n\nNo frontmatter here.\n";
    EXPECT_THROW(PluginLoader::ParseSkillMd(content), std::runtime_error);
}

TEST(PluginLoaderTest, ParseSkillMdThrowsWithUnclosedFrontmatter) {
    std::string content = R"(---
name: broken
description: missing closing dashes
Body starts here without closing ---
)";
    EXPECT_THROW(PluginLoader::ParseSkillMd(content), std::runtime_error);
}

TEST(PluginLoaderTest, ParseSkillMdBodyPreservesPlaceholders) {
    std::string content = R"(---
name: template_skill
description: Template test
variables:
  - var1
  - var2
---

Start {{var1}} middle {{var2}} end.
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_NE(skill.m_PromptTemplate.find("{{var1}}"), std::string::npos);
    EXPECT_NE(skill.m_PromptTemplate.find("{{var2}}"), std::string::npos);
}

TEST(PluginLoaderTest, ParseSkillMdDefaultModelIsEmpty) {
    std::string content = R"(---
name: agnostic
description: LLM-agnostic skill
---

Do {{task}}.
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    // Plugin skills have no baked-in model — resolved at runtime by LiteLLM
    EXPECT_TRUE(skill.m_DefaultModel.empty());
}

TEST(PluginLoaderTest, ParseSkillMdWindowsLineEndings) {
    // Simulate CRLF line endings
    std::string content = "---\r\nname: crlf_skill\r\ndescription: Windows style\r\n---\r\n\r\nBody.\r\n";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Name, "crlf_skill");
    EXPECT_EQ(skill.m_Description, "Windows style");
}

TEST(PluginLoaderTest, ParseSkillMdUnquotedColonInDescription) {
    std::string content = R"(---
name: code-review
description: Use when: the user asks for a review
---

Review the code.
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Name, "code-review");
    EXPECT_EQ(skill.m_Description, "Use when: the user asks for a review");
}

TEST(PluginLoaderTest, ParseSkillMdMetadataMap) {
    std::string content = R"(---
name: mapped
description: Has metadata
metadata:
  author: rakesh
  version: 1
---

Body.
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Name, "mapped");
    EXPECT_EQ(skill.m_Description, "Has metadata");
}

TEST(PluginLoaderTest, ParseSkillMdUtf8Bom) {
    std::string content = "\xEF\xBB\xBF---\nname: bom-skill\ndescription: Has BOM\n---\n\nBody.\n";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Name, "bom-skill");
    EXPECT_EQ(skill.m_Description, "Has BOM");
}

// ---------------------------------------------------------------------------
// LoadIntoEngine filesystem tests
// ---------------------------------------------------------------------------

class PluginLoaderFsTest : public ::testing::Test {
protected:
    fs::path tempDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / "mcp_plugin_test";
        fs::remove_all(tempDir);
        fs::create_directories(tempDir);
    }

    void TearDown() override {
        fs::remove_all(tempDir);
    }

    void CreateSkillMd(const std::string& pluginName,
                       const std::string& skillName,
                       const std::string& content) {
        fs::path skillDir = tempDir / pluginName / "skills" / skillName;
        fs::create_directories(skillDir);
        std::ofstream f(skillDir / "SKILL.md");
        f << content;
    }
};

TEST_F(PluginLoaderFsTest, LoadsSinglePlugin) {
    CreateSkillMd("my_plugin", "greet", R"(---
name: greet
description: Greeting skill
variables:
  - name
---

Hello {{name}}!
)");

    SkillEngine engine;
    PluginLoader::LoadIntoEngine(tempDir.string(), engine);

    auto skill = engine.Resolve("greet");
    ASSERT_TRUE(skill.has_value());
    EXPECT_EQ(skill->m_Description, "Greeting skill");
    ASSERT_EQ(skill->m_RequiredVariables.size(), 1u);
    EXPECT_EQ(skill->m_RequiredVariables[0], "name");
}

TEST_F(PluginLoaderFsTest, LoadsMultiplePluginsAndSkills) {
    CreateSkillMd("plugin_a", "skill_a1", "---\nname: skill_a1\ndescription: A1\n---\nBody A1\n");
    CreateSkillMd("plugin_a", "skill_a2", "---\nname: skill_a2\ndescription: A2\n---\nBody A2\n");
    CreateSkillMd("plugin_b", "skill_b1", "---\nname: skill_b1\ndescription: B1\n---\nBody B1\n");

    SkillEngine engine;
    PluginLoader::LoadIntoEngine(tempDir.string(), engine);

    auto skills = engine.ListSkills();
    EXPECT_EQ(skills.size(), 3u);
    EXPECT_TRUE(engine.Resolve("skill_a1").has_value());
    EXPECT_TRUE(engine.Resolve("skill_a2").has_value());
    EXPECT_TRUE(engine.Resolve("skill_b1").has_value());
}

TEST_F(PluginLoaderFsTest, UsesDirNameAsFallbackWhenNoNameInFrontmatter) {
    CreateSkillMd("plugin_x", "my_fallback_skill", R"(---
description: No name field
---

Body.
)");

    SkillEngine engine;
    PluginLoader::LoadIntoEngine(tempDir.string(), engine);

    EXPECT_TRUE(engine.Resolve("my_fallback_skill").has_value());
}

TEST_F(PluginLoaderFsTest, SkipsMalformedSkillMd) {
    // One malformed, one good — should load only the good one
    CreateSkillMd("plugin_ok", "good_skill", "---\nname: good_skill\ndescription: OK\n---\nBody.\n");
    CreateSkillMd("plugin_bad", "bad_skill", "This is not valid frontmatter at all");

    SkillEngine engine;
    // Should not throw
    EXPECT_NO_THROW(PluginLoader::LoadIntoEngine(tempDir.string(), engine));

    EXPECT_TRUE(engine.Resolve("good_skill").has_value());
    EXPECT_FALSE(engine.Resolve("bad_skill").has_value());
}

TEST_F(PluginLoaderFsTest, MissingPluginsDirIsNoOp) {
    SkillEngine engine;
    EXPECT_NO_THROW(PluginLoader::LoadIntoEngine(
        (tempDir / "nonexistent").string(), engine));
    EXPECT_TRUE(engine.ListSkills().empty());
}

TEST_F(PluginLoaderFsTest, SkipEntriesWithoutSkillMd) {
    // Create a skills dir with a subdirectory that has no SKILL.md
    fs::create_directories(tempDir / "plugin_empty" / "skills" / "no_skill_md");

    SkillEngine engine;
    EXPECT_NO_THROW(PluginLoader::LoadIntoEngine(tempDir.string(), engine));
    EXPECT_TRUE(engine.ListSkills().empty());
}

// ---------------------------------------------------------------------------
// Command skill tests (ParseSkillMd)
// ---------------------------------------------------------------------------

TEST(PluginLoaderTest, CommandSkill_ParsedFromSkillMd) {
    std::string content = R"(---
name: cmd_skill
description: A command skill
type: command
command_template: echo {{msg}}
variables:
  - msg
---

Documentation body.
)";
    auto skill = PluginLoader::ParseSkillMd(content, "cmd_skill");
    EXPECT_EQ(skill.m_Type, SkillType::Command);
    EXPECT_EQ(skill.m_CommandTemplate, "echo {{msg}}");
    ASSERT_EQ(skill.m_RequiredVariables.size(), 1u);
    EXPECT_EQ(skill.m_RequiredVariables[0], "msg");
}

TEST(PluginLoaderTest, CommandSkill_PluginDirSubstituted) {
    std::string content = R"(---
name: es_skill
description: ES search
type: command
command_template: ${PLUGIN_DIR}/scripts/es.exe -n 50 "{{query}}"
variables:
  - query
---
)";
    auto skill = PluginLoader::ParseSkillMd(content, "es_skill", "C:\\plugins\\everything-search");
    EXPECT_EQ(skill.m_CommandTemplate,
              "C:\\plugins\\everything-search/scripts/es.exe -n 50 \"{{query}}\"");
}

TEST(PluginLoaderTest, CommandSkill_RulesLoadedFromFrontmatter) {
    std::string content = R"(---
name: rule_skill
description: Has rules
type: command
command_template: echo {{x}}
variables:
  - x
rules:
  - Rule one
  - Rule two
  - Rule three
---
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    ASSERT_EQ(skill.m_Rules.size(), 3u);
    EXPECT_EQ(skill.m_Rules[0], "Rule one");
    EXPECT_EQ(skill.m_Rules[1], "Rule two");
    EXPECT_EQ(skill.m_Rules[2], "Rule three");
}

TEST(PluginLoaderTest, CommandSkill_LlmSkillUnchanged) {
    // No "type:" field — must default to LLM for backward compat
    std::string content = R"(---
name: llm_skill
description: An LLM skill
variables:
  - input
---

Do {{input}}.
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Type, SkillType::LLM);
    EXPECT_TRUE(skill.m_CommandTemplate.empty());
}

TEST(PluginLoaderTest, CommandSkill_NoRequiredVariables) {
    // A command skill with no variables: section at all — should parse with empty list
    std::string content = R"(---
name: no_vars
description: No variables needed
type: command
command_template: echo hello
---
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    EXPECT_EQ(skill.m_Type, SkillType::Command);
    EXPECT_EQ(skill.m_CommandTemplate, "echo hello");
    EXPECT_TRUE(skill.m_RequiredVariables.empty());
}

TEST(PluginLoaderTest, CommandSkill_VariablesAndRulesCoexist) {
    std::string content = R"(---
name: mixed
description: Has both variables and rules
type: command
command_template: run {{a}} {{b}}
variables:
  - a
  - b
rules:
  - Always quote paths
---
)";
    auto skill = PluginLoader::ParseSkillMd(content);
    ASSERT_EQ(skill.m_RequiredVariables.size(), 2u);
    ASSERT_EQ(skill.m_Rules.size(), 1u);
    EXPECT_EQ(skill.m_Rules[0], "Always quote paths");
}

// ---------------------------------------------------------------------------
// Command skill filesystem integration test
// ---------------------------------------------------------------------------

TEST_F(PluginLoaderFsTest, LoadedSkillCanBeRendered) {
    CreateSkillMd("plugin", "render_test", R"(---
name: render_test
description: Render test
variables:
  - subject
---

Summarize {{subject}} in one sentence.
)");

    SkillEngine engine;
    PluginLoader::LoadIntoEngine(tempDir.string(), engine);

    auto skill = engine.Resolve("render_test");
    ASSERT_TRUE(skill.has_value());

    nlohmann::json vars = {{"subject", "climate change"}};
    std::string rendered = engine.RenderPrompt(*skill, vars);
    EXPECT_NE(rendered.find("climate change"), std::string::npos);
}
