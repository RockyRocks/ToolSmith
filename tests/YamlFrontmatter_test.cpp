#include <gtest/gtest.h>
#include <skills/YamlFrontmatter.h>

TEST(YamlFrontmatterTest, MissingOpeningDashesFails) {
    auto fm = ParseYamlFrontmatter("# just markdown\n");
    EXPECT_FALSE(fm.ok());
    EXPECT_NE(fm.error.find("frontmatter"), std::string::npos);
}

TEST(YamlFrontmatterTest, UnclosedFrontmatterFails) {
    auto fm = ParseYamlFrontmatter("---\nname: x\ndescription: y\nBody without close\n");
    EXPECT_FALSE(fm.ok());
    EXPECT_NE(fm.error.find("unclosed"), std::string::npos);
}

TEST(YamlFrontmatterTest, UnquotedColonInDescription) {
    auto fm = ParseYamlFrontmatter(
        "---\nname: code-review\ndescription: Use when: the user asks\n---\n\nBody\n");
    ASSERT_TRUE(fm.ok());
    EXPECT_EQ(fm.GetString("name"), "code-review");
    EXPECT_EQ(fm.GetString("description"), "Use when: the user asks");
    EXPECT_NE(fm.body.find("Body"), std::string::npos);
}

TEST(YamlFrontmatterTest, NestedMetadataMap) {
    auto fm = ParseYamlFrontmatter(
        "---\nname: mapped\ndescription: d\nmetadata:\n  author: rakesh\n  os: linux\n---\n");
    ASSERT_TRUE(fm.ok());
    auto meta = fm.GetStringMap("metadata");
    EXPECT_EQ(meta["author"], "rakesh");
    EXPECT_EQ(meta["os"], "linux");
}

TEST(YamlFrontmatterTest, StringList) {
    auto fm = ParseYamlFrontmatter(
        "---\nname: n\ndescription: d\nvariables:\n  - code\n  - language\n---\n");
    ASSERT_TRUE(fm.ok());
    auto vars = fm.GetStringList("variables");
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "code");
    EXPECT_EQ(vars[1], "language");
}

TEST(YamlFrontmatterTest, AllowedToolsString) {
    auto fm = ParseYamlFrontmatter(
        "---\nname: n\ndescription: d\nallowed-tools: read edit\n---\n");
    ASSERT_TRUE(fm.ok());
    EXPECT_EQ(fm.GetString("allowed-tools"), "read edit");
}

TEST(YamlFrontmatterTest, Utf8Bom) {
    std::string content = "\xEF\xBB\xBF---\nname: bom\ndescription: d\n---\nHi\n";
    auto fm = ParseYamlFrontmatter(content);
    ASSERT_TRUE(fm.ok());
    EXPECT_EQ(fm.GetString("name"), "bom");
}

TEST(YamlFrontmatterTest, QuotedValues) {
    auto fm = ParseYamlFrontmatter(
        "---\nname: \"quoted-name\"\ndescription: 'a desc'\n---\n");
    ASSERT_TRUE(fm.ok());
    EXPECT_EQ(fm.GetString("name"), "quoted-name");
    EXPECT_EQ(fm.GetString("description"), "a desc");
}
