#include <gtest/gtest.h>
#include <core/ToolProfile.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class ToolProfileTest : public ::testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        root = fs::temp_directory_path() / "ts_profile";
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }
};

TEST_F(ToolProfileTest, DetectCoreWhenNoMarkers) {
    EXPECT_EQ(ToolProfile::Detect(root.string()), "core");
}

TEST_F(ToolProfileTest, DetectCppFromCMakeLists) {
    std::ofstream(root / "CMakeLists.txt") << "project(x)\n";
    EXPECT_EQ(ToolProfile::Detect(root.string()), "cpp");
}

TEST_F(ToolProfileTest, DetectCsharpFromCsproj) {
    std::ofstream(root / "App.csproj") << "<Project/>\n";
    EXPECT_EQ(ToolProfile::Detect(root.string()), "csharp");
}

TEST_F(ToolProfileTest, DetectFullstackWhenBoth) {
    std::ofstream(root / "CMakeLists.txt") << "project(x)\n";
    std::ofstream(root / "App.csproj") << "<Project/>\n";
    EXPECT_EQ(ToolProfile::Detect(root.string()), "fullstack");
}

TEST_F(ToolProfileTest, NormalizeUnknownIsEmpty) {
    EXPECT_TRUE(ToolProfile::Normalize("nope").empty());
    EXPECT_EQ(ToolProfile::Normalize("CPP"), "cpp");
    EXPECT_EQ(ToolProfile::Normalize(""), "auto");
}

TEST_F(ToolProfileTest, ResolveUnknownProfileFails) {
    ToolProfile::ResolveInput in;
    in.profile = "fortran";
    auto out = ToolProfile::Resolve(in);
    EXPECT_FALSE(out.error.empty());
}

TEST_F(ToolProfileTest, ResolveUnknownPackFails) {
    ToolProfile::ResolveInput in;
    in.profile = "core";
    in.enable = {"not-a-pack"};
    auto out = ToolProfile::Resolve(in);
    EXPECT_NE(out.error.find("Unknown pack"), std::string::npos);
}

TEST_F(ToolProfileTest, ResolveUnknownToolPinFails) {
    ToolProfile::ResolveInput in;
    in.profile = "cpp";
    in.pinTools = {"not_a_tool"};
    auto out = ToolProfile::Resolve(in);
    EXPECT_NE(out.error.find("Unknown tool"), std::string::npos);
}

TEST_F(ToolProfileTest, ResolveEmptyToolNameFails) {
    ToolProfile::ResolveInput in;
    in.profile = "core";
    in.pinTools = {""};
    auto out = ToolProfile::Resolve(in);
    EXPECT_NE(out.error.find("Empty tool"), std::string::npos);
}

TEST_F(ToolProfileTest, CppAdvertisesLanguageSet) {
    auto tools = ToolProfile::ToolsForProfile("cpp");
    EXPECT_EQ(tools.size(), 12u);
    EXPECT_TRUE(tools.count("read"));
    EXPECT_TRUE(tools.count("git"));
    EXPECT_TRUE(tools.count("diagnose"));
    EXPECT_FALSE(tools.count("skills"));
    EXPECT_FALSE(tools.count("llm"));
}

TEST_F(ToolProfileTest, CoreDoesNotIncludeGit) {
    auto tools = ToolProfile::ToolsForProfile("core");
    EXPECT_EQ(tools.size(), 8u);
    EXPECT_FALSE(tools.count("git"));
}

TEST_F(ToolProfileTest, EnableSkillsAddsTools) {
    ToolProfile::ResolveInput in;
    in.profile = "core";
    in.enable = {"skills"};
    auto out = ToolProfile::Resolve(in);
    EXPECT_TRUE(out.error.empty());
    EXPECT_TRUE(out.advertised.count("skills"));
    EXPECT_TRUE(out.advertised.count("skill"));
}

TEST_F(ToolProfileTest, PinReplacesAdvertisedSet) {
    ToolProfile::ResolveInput in;
    in.profile = "cpp";
    in.pinTools = {"read", "edit"};
    auto out = ToolProfile::Resolve(in);
    EXPECT_EQ(out.advertised.size(), 2u);
    EXPECT_TRUE(out.advertised.count("read"));
    EXPECT_FALSE(out.advertised.count("git"));
}

TEST_F(ToolProfileTest, OptionalPacksNeverAutoEnable) {
    EXPECT_TRUE(ToolProfile::IsOptionalPack("jira"));
    EXPECT_TRUE(ToolProfile::IsOptionalPack("github"));
    EXPECT_TRUE(ToolProfile::IsOptionalPack("skills"));
    auto tools = ToolProfile::ToolsForProfile("fullstack");
    EXPECT_FALSE(tools.count("jira"));
}

TEST_F(ToolProfileTest, ProfileAllLoadPlugins) {
    ToolProfile::ResolveInput in;
    in.profile = "all";
    auto out = ToolProfile::Resolve(in);
    EXPECT_TRUE(out.loadAllPlugins);
    EXPECT_TRUE(out.advertised.empty());
}
