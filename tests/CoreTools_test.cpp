#include <gtest/gtest.h>
#include <commands/CoreTools.h>
#include <core/Hashline.h>
#include <core/ResultBudget.h>
#include <core/ToolProfile.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iterator>

namespace fs = std::filesystem;

class CoreToolsTest : public ::testing::Test {
protected:
    fs::path root;
    std::shared_ptr<CoreRuntime> rt;
    CommandRegistry reg;

    void SetUp() override {
        root = fs::temp_directory_path()
             / ("ts_core_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        rt = std::make_shared<CoreRuntime>();
        rt->jail = WorkspaceJail::Open(root);
        rt->profile = "cpp";
        rt->advertised = ToolProfile::ToolsForProfile("cpp");
        RegisterCoreTools(reg, rt);
        reg.SetAdvertised(rt->advertised);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    nlohmann::json Call(const std::string& name, const nlohmann::json& payload) {
        nlohmann::json req = {{"command", name}, {"payload", payload}};
        return reg.ExecuteWithChaining(name, req);
    }
};

static nlohmann::json SerializeToolsList(const CommandRegistry& reg) {
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& meta : reg.ListToolMetadata()) {
        tools.push_back({
            {"name", meta.m_Name},
            {"description", meta.m_Description},
            {"inputSchema", meta.m_InputSchema}
        });
    }
    return {{"tools", tools}};
}

TEST_F(CoreToolsTest, CppAdvertisesAtMostTwelveTools) {
    auto meta = reg.ListToolMetadata();
    EXPECT_LE(meta.size(), 12u);
    EXPECT_EQ(meta.size(), 12u);
    auto dumped = SerializeToolsList(reg).dump();
    EXPECT_LE(dumped.size(), kMaxToolsListBytes);
    for (const auto& m : meta) {
        EXPECT_LE(m.m_Description.size(), kDescriptionMaxChars);
        EXPECT_EQ(m.m_Description.find("rules:"), std::string::npos);
        EXPECT_EQ(m.m_Source, ToolSource::BuiltIn);
    }
}

TEST_F(CoreToolsTest, ReadReturnsHashline) {
    std::ofstream(root / "f.cpp") << "int main() { return 0; }\n";
    auto r = Call("read", {{"path", "f.cpp"}});
    EXPECT_EQ(r["status"], "ok");
    std::string content = r["content"].get<std::string>();
    EXPECT_NE(content.find("1|"), std::string::npos);
    EXPECT_NE(content.find("|int main()"), std::string::npos);
    EXPECT_FALSE(r.value("outside_workspace", true));
}

TEST_F(CoreToolsTest, ReadOffsetPastEofFails) {
    std::ofstream(root / "f.cpp") << "one\n";
    auto r = Call("read", {{"path", "f.cpp"}, {"offset", 50}});
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("past end"), std::string::npos);
}

TEST_F(CoreToolsTest, ReadZeroLimitFails) {
    std::ofstream(root / "f.cpp") << "one\n";
    auto r = Call("read", {{"path", "f.cpp"}, {"limit", 0}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, ReadBinaryFails) {
    {
        std::ofstream out(root / "blob.bin", std::ios::binary);
        out << "a\0b";
        out.put('\0');
        out << "c";
    }
    auto r = Call("read", {{"path", "blob.bin"}});
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("binary"), std::string::npos);
}

TEST_F(CoreToolsTest, ReadOutsideJailIsFlagged) {
    auto outside = fs::temp_directory_path()
                 / ("ts_core_outside_read_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count())
                    + ".txt");
    {
        std::ofstream out(outside);
        out << "secret_outside\n";
    }
    auto r = Call("read", {{"path", outside.string()}});
    if (r["status"] == "ok") {
        EXPECT_TRUE(r.value("outside_workspace", false));
    } else {
        EXPECT_EQ(r["status"], "error");
    }
    std::error_code ec;
    fs::remove(outside, ec);
}

TEST_F(CoreToolsTest, EditStaleHashWritesNothing) {
    const fs::path f = root / "edit.cpp";
    std::ofstream(f) << "aaa\nbbb\n";
    auto r = Call("edit", {
        {"path", "edit.cpp"},
        {"hunks", nlohmann::json::array({
            {{"start", "1:dead"}, {"content", "zzz\n"}}
        })}
    });
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("Stale"), std::string::npos);
    std::ifstream in(f);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(got.find("aaa"), std::string::npos);
    EXPECT_EQ(got.find("zzz"), std::string::npos);
}

TEST_F(CoreToolsTest, EditSuccessAppliesHunk) {
    const fs::path f = root / "edit.cpp";
    std::ofstream(f) << "aaa\nbbb\n";
    std::string h = Hashline::LineHash("aaa");
    auto r = Call("edit", {
        {"path", "edit.cpp"},
        {"hunks", nlohmann::json::array({
            {{"start", "1:" + h}, {"content", "AAA\n"}}
        })}
    });
    EXPECT_EQ(r["status"], "ok");
    std::ifstream in(f);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(got.find("AAA"), std::string::npos);
    EXPECT_EQ(got.find("aaa"), std::string::npos);
}

TEST_F(CoreToolsTest, EditEmptyHunksFails) {
    std::ofstream(root / "edit.cpp") << "aaa\n";
    auto r = Call("edit", {{"path", "edit.cpp"}, {"hunks", nlohmann::json::array()}});
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("hunks"), std::string::npos);
}

TEST_F(CoreToolsTest, EditInvalidAnchorFails) {
    std::ofstream(root / "edit.cpp") << "aaa\n";
    auto r = Call("edit", {
        {"path", "edit.cpp"},
        {"hunks", nlohmann::json::array({
            {{"start", "not-an-anchor"}, {"content", "x"}}
        })}
    });
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, EditOutsideJailFails) {
    auto outside = fs::temp_directory_path()
                 / ("ts_core_outside_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count())
                    + ".txt");
    {
        std::ofstream out(outside);
        out << "secret\n";
    }
    auto r = Call("edit", {
        {"path", outside.string()},
        {"hunks", nlohmann::json::array({
            {{"start", "1:" + Hashline::LineHash("secret")}, {"content", "x\n"}}
        })}
    });
    EXPECT_EQ(r["status"], "error");
    {
        std::ifstream in(outside);
        std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(got.find("secret"), std::string::npos);
    }
    std::error_code ec;
    fs::remove(outside, ec);
}

TEST_F(CoreToolsTest, SearchReturnsSnippetsNotBodies) {
    std::ofstream(root / "a.cpp") << "unique_token_abc unique_token_abc\nthis_line_should_not_dump_whole_file_XXXX\n";
    auto r = Call("search", {{"pattern", "unique_token_abc"}});
    EXPECT_EQ(r["status"], "ok");
    std::string content = r["content"].get<std::string>();
    EXPECT_NE(content.find("a.cpp:1:"), std::string::npos);
    EXPECT_EQ(content.find("this_line_should_not_dump_whole_file_XXXX"), std::string::npos);
}

TEST_F(CoreToolsTest, SearchOutsideJailFails) {
    auto outsideDir = fs::temp_directory_path()
                    / ("ts_core_outside_search_" + std::to_string(
                           std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(outsideDir);
    {
        std::ofstream out(outsideDir / "x.txt");
        out << "needle_outside\n";
    }
    auto r = Call("search", {{"pattern", "needle_outside"}, {"path", outsideDir.string()}});
    EXPECT_EQ(r["status"], "error");
    std::error_code ec;
    fs::remove_all(outsideDir, ec);
}

TEST_F(CoreToolsTest, SearchEmptyPatternFails) {
    auto r = Call("search", {{"pattern", ""}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, ShellRunsCommand) {
    auto r = Call("shell", {{"command", "echo hello_from_core"}});
    ASSERT_EQ(r["status"], "ok") << r.dump();
    ASSERT_TRUE(r["content"].is_string()) << r.dump();
    EXPECT_NE(r["content"].get<std::string>().find("hello_from_core"), std::string::npos)
        << r.dump();
}

TEST_F(CoreToolsTest, ShellTimeoutKills) {
#ifdef _WIN32
    // timeout.exe cannot be used: it exits immediately when stdin is a pipe.
    const char* hang = "ping -n 8 127.0.0.1";
#else
    const char* hang = "sleep 5";
#endif
    auto r = Call("shell", {{"command", hang}, {"timeout", 1}});
    ASSERT_EQ(r["status"], "error") << r.dump();
    ASSERT_TRUE(r["error"].is_string()) << r.dump();
    EXPECT_NE(r["error"].get<std::string>().find("timed out"), std::string::npos)
        << r.dump();
}

TEST_F(CoreToolsTest, ShellEmptyCommandFails) {
    auto r = Call("shell", {{"command", ""}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, ProjectIsCompact) {
    std::ofstream(root / "CMakeLists.txt") << "project(x)\n";
    auto r = Call("project", nlohmann::json::object());
    EXPECT_EQ(r["status"], "ok");
    EXPECT_LE(r["content"].get<std::string>().size(), 1024u);
    EXPECT_NE(r["content"].get<std::string>().find("profile: cpp"), std::string::npos);
}

TEST_F(CoreToolsTest, GitUnknownActionFails) {
    auto r = Call("git", {{"action", "push"}});
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("Unknown git action"), std::string::npos);
}

TEST_F(CoreToolsTest, GitFullDiffRequiresPath) {
    auto r = Call("git", {{"action", "diff"}, {"full", true}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, DiagnoseRequiresPath) {
    auto r = Call("diagnose", nlohmann::json::object());
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, DeactivateCoreToolFails) {
    auto r = Call("deactivate", {{"tool", "read"}});
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("Cannot deactivate core"), std::string::npos);
    EXPECT_TRUE(reg.IsAdvertised("read"));
}

TEST_F(CoreToolsTest, DeactivateCorePackFails) {
    auto r = Call("deactivate", {{"pack", "core"}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, SkillsPackIsGated) {
    auto r = Call("skills", nlohmann::json::object());
    EXPECT_EQ(r["status"], "error");
    EXPECT_NE(r["error"].get<std::string>().find("not advertised"), std::string::npos);

    auto a = Call("activate", {{"pack", "skills"}});
    EXPECT_EQ(a["status"], "ok");
    EXPECT_TRUE(reg.IsAdvertised("skills"));
    auto list = Call("skills", nlohmann::json::object());
    EXPECT_EQ(list["status"], "ok");
}

TEST_F(CoreToolsTest, SkillBodyGatedUntilActivate) {
    auto r = Call("skill", {{"name", "missing"}});
    EXPECT_EQ(r["status"], "error");
    Call("activate", {{"pack", "skills"}});
    auto r2 = Call("skill", {{"name", "missing"}});
    EXPECT_EQ(r2["status"], "error");
    EXPECT_NE(r2["error"].get<std::string>().find("Unknown skill"), std::string::npos);
}

TEST_F(CoreToolsTest, ActivateUnknownPackFails) {
    auto r = Call("activate", {{"pack", "does-not-exist"}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, ActivateEmptyPackFails) {
    auto r = Call("activate", {{"pack", ""}});
    EXPECT_EQ(r["status"], "error");
}

TEST_F(CoreToolsTest, CatalogListsPacksWithoutSchemas) {
    auto r = Call("catalog", nlohmann::json::object());
    EXPECT_EQ(r["status"], "ok");
    EXPECT_EQ(r["content"].get<std::string>().find("inputSchema"), std::string::npos);
    EXPECT_NE(r["content"].get<std::string>().find("\"id\": \"jira\""), std::string::npos);
}
