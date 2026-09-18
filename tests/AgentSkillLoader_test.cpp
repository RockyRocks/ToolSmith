#include <gtest/gtest.h>
#include <skills/AgentSkillLoader.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class AgentSkillLoaderTest : public ::testing::Test {
protected:
    fs::path ws;
    fs::path home;

    void SetUp() override {
        ws = fs::temp_directory_path() / "ts_skills_ws";
        home = fs::temp_directory_path() / "ts_skills_home";
        fs::remove_all(ws);
        fs::remove_all(home);
        fs::create_directories(ws);
        fs::create_directories(home);
    }
    void TearDown() override {
        fs::remove_all(ws);
        fs::remove_all(home);
    }

    void WriteSkill(const fs::path& skillsRoot, const std::string& dir,
                    const std::string& content) {
        fs::path d = skillsRoot / dir;
        fs::create_directories(d);
        std::ofstream(d / "SKILL.md") << content;
    }
};

TEST_F(AgentSkillLoaderTest, SpecHyphenNameLoads) {
    WriteSkill(ws / ".agents" / "skills", "code-review",
               "---\nname: code-review\ndescription: Review C++\n---\n\nBe thorough.\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    ASSERT_EQ(disc.skills.size(), 1u);
    EXPECT_EQ(disc.skills[0].m_Name, "code-review");
    EXPECT_TRUE(disc.skills[0].m_Warnings.empty());
}

TEST_F(AgentSkillLoaderTest, SkipMissingDescription) {
    WriteSkill(ws / ".agents" / "skills", "no-desc",
               "---\nname: no-desc\n---\n\nBody\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    EXPECT_TRUE(disc.skills.empty());
    ASSERT_FALSE(disc.skipped.empty());
    EXPECT_NE(disc.skipped[0].find("description"), std::string::npos);
}

TEST_F(AgentSkillLoaderTest, SkipMissingNameNoFallback) {
    WriteSkill(ws / ".agents" / "skills", "fallback-dir",
               "---\ndescription: Has desc only\n---\n\nBody\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    EXPECT_TRUE(disc.skills.empty());
    ASSERT_FALSE(disc.skipped.empty());
    EXPECT_NE(disc.skipped[0].find("name is required"), std::string::npos);
}

TEST_F(AgentSkillLoaderTest, SkipUnparseable) {
    WriteSkill(ws / ".agents" / "skills", "broken", "not frontmatter\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    EXPECT_TRUE(disc.skills.empty());
    EXPECT_FALSE(disc.skipped.empty());
}

TEST_F(AgentSkillLoaderTest, UnderscoreNameWarnsButLoads) {
    WriteSkill(ws / ".agents" / "skills", "my_skill",
               "---\nname: my_skill\ndescription: Compat\n---\n\nBody\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    ASSERT_EQ(disc.skills.size(), 1u);
    ASSERT_FALSE(disc.skills[0].m_Warnings.empty());
}

TEST_F(AgentSkillLoaderTest, DirectoryMismatchWarns) {
    WriteSkill(ws / ".agents" / "skills", "other-dir",
               "---\nname: code-review\ndescription: d\n---\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    ASSERT_EQ(disc.skills.size(), 1u);
    bool found = false;
    for (const auto& w : disc.skills[0].m_Warnings) {
        if (w.find("does not match directory") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(AgentSkillLoaderTest, ToolsmithOverridesAgents) {
    WriteSkill(ws / ".agents" / "skills", "code-review",
               "---\nname: code-review\ndescription: from agents\n---\n");
    WriteSkill(ws / ".toolsmith" / "skills", "code-review",
               "---\nname: code-review\ndescription: from toolsmith\n---\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    ASSERT_EQ(disc.skills.size(), 1u);
    EXPECT_EQ(disc.skills[0].m_Description, "from toolsmith");
}

TEST_F(AgentSkillLoaderTest, ProjectOverridesUser) {
    WriteSkill(home / ".agents" / "skills", "shared",
               "---\nname: shared\ndescription: user\n---\n");
    WriteSkill(ws / ".agents" / "skills", "shared",
               "---\nname: shared\ndescription: project\n---\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    ASSERT_EQ(disc.skills.size(), 1u);
    EXPECT_EQ(disc.skills[0].m_Description, "project");
}

TEST_F(AgentSkillLoaderTest, MetadataAndAllowedTools) {
    WriteSkill(ws / ".agents" / "skills", "mapped",
               "---\nname: mapped\ndescription: d\n"
               "allowed-tools: read edit\n"
               "metadata:\n  author: rakesh\n---\n\nBody here\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    ASSERT_EQ(disc.skills.size(), 1u);
    EXPECT_EQ(disc.skills[0].m_AllowedTools, "read edit");
    EXPECT_EQ(disc.skills[0].m_Metadata["author"], "rakesh");
}

TEST_F(AgentSkillLoaderTest, LoadBodyUnknownFails) {
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    auto body = LoadAgentSkillBody(disc, "nope");
    EXPECT_NE(body.error.find("Unknown skill"), std::string::npos);
}

TEST_F(AgentSkillLoaderTest, LoadBodyEmptyNameFails) {
    AgentSkillDiscovery disc;
    auto body = LoadAgentSkillBody(disc, "");
    EXPECT_NE(body.error.find("name"), std::string::npos);
}

TEST_F(AgentSkillLoaderTest, LoadBodyReturnsMarkdown) {
    WriteSkill(ws / ".agents" / "skills", "code-review",
               "---\nname: code-review\ndescription: d\n---\n\nBe thorough.\n");
    auto disc = DiscoverAgentSkills(ws.string(), home.string());
    auto body = LoadAgentSkillBody(disc, "code-review");
    EXPECT_TRUE(body.error.empty());
    EXPECT_NE(body.body.find("Be thorough."), std::string::npos);
}

TEST(AgentSkillLoaderTest, IsSpecSkillNameRules) {
    EXPECT_TRUE(IsSpecSkillName("code-review"));
    EXPECT_TRUE(IsSpecSkillName("a1"));
    EXPECT_FALSE(IsSpecSkillName("my_skill"));
    EXPECT_FALSE(IsSpecSkillName("Bad"));
    EXPECT_FALSE(IsSpecSkillName("-lead"));
    EXPECT_FALSE(IsSpecSkillName("trail-"));
    EXPECT_FALSE(IsSpecSkillName("has--double"));
    EXPECT_FALSE(IsSpecSkillName(""));
    EXPECT_FALSE(IsSpecSkillName(std::string(65, 'a')));
}
