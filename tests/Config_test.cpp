#include <gtest/gtest.h>
#include <core/Config.h>
#include <fstream>

TEST(ConfigTest, DefaultValues) {
    Config cfg = Config::LoadFromEnv();
    EXPECT_EQ(cfg.GetServerPort(), 9001);
    EXPECT_EQ(cfg.GetLiteLlmBaseUrl(), "http://localhost:4000");
    EXPECT_GT(cfg.GetThreadPoolSize(), 0u);
    EXPECT_EQ(cfg.GetMaxRequestBodySize(), 1048576u);
    EXPECT_EQ(cfg.GetRateLimitRequestsPerMinute(), 60u);
    EXPECT_FALSE(cfg.IsAuthEnabled());
}

TEST(ConfigTest, LoadFromFile) {
    std::string path = "test_config_tmp.json";
    {
        std::ofstream f(path);
        f << R"({
            "server": {"port": 8080},
            "litellm": {"base_url": "http://localhost:5000", "default_model": "gpt-4"},
            "rate_limit": {"requests_per_minute": 30}
        })";
    }

    Config cfg = Config::LoadFromFile(path);
    EXPECT_EQ(cfg.GetServerPort(), 8080);
    EXPECT_EQ(cfg.GetLiteLlmBaseUrl(), "http://localhost:5000");
    EXPECT_EQ(cfg.GetDefaultModel(), "gpt-4");
    EXPECT_EQ(cfg.GetRateLimitRequestsPerMinute(), 30u);

    std::remove(path.c_str());
}

TEST(ConfigTest, MissingFileThrows) {
    EXPECT_THROW(Config::LoadFromFile("nonexistent.json"), std::runtime_error);
}

TEST(ConfigTest, ToolsDefaultsAreLean) {
    Config cfg;
    EXPECT_EQ(cfg.GetToolsProfile(), "auto");
    EXPECT_TRUE(cfg.GetToolsEnable().empty());
    EXPECT_TRUE(cfg.GetToolsDisable().empty());
    EXPECT_FALSE(cfg.IsChainingEnabled());
    EXPECT_FALSE(cfg.AllowCommandSkills());
    EXPECT_EQ(cfg.GetWorkspaceRoot(), "auto");
}

TEST(ConfigTest, LoadToolsAndWorkspaceFromFile) {
    std::string path = "test_config_tools_tmp.json";
    {
        std::ofstream f(path);
        f << R"({
            "tools": {
                "profile": "cpp",
                "enable": ["skills"],
                "disable": ["llm"],
                "chaining": true
            },
            "workspace": {"root": "/tmp", "allow": ["/opt/sdk"]}
        })";
    }
    Config cfg = Config::LoadFromFile(path);
    EXPECT_EQ(cfg.GetToolsProfile(), "cpp");
    ASSERT_EQ(cfg.GetToolsEnable().size(), 1u);
    EXPECT_EQ(cfg.GetToolsEnable()[0], "skills");
    ASSERT_EQ(cfg.GetToolsDisable().size(), 1u);
    EXPECT_EQ(cfg.GetToolsDisable()[0], "llm");
    EXPECT_TRUE(cfg.IsChainingEnabled());
    EXPECT_EQ(cfg.GetWorkspaceRoot(), "/tmp");
    ASSERT_EQ(cfg.GetWorkspaceAllow().size(), 1u);
    EXPECT_EQ(cfg.GetWorkspaceAllow()[0], "/opt/sdk");
    std::remove(path.c_str());
}
