#include <gtest/gtest.h>
#include <plugins/SubprocessPipe.h>
#include <plugins/StdioMCPAdapter.h>
#include <commands/CommandRegistry.h>
#include <plugins/ScriptPluginLoader.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static bool IsPythonAvailable() {
#ifdef _WIN32
    FILE* p = _popen("python --version 2>NUL", "r");
#else
    FILE* p = popen("python3 --version 2>/dev/null", "r");
#endif
    if (!p) return false;
    char buf[64];
    bool got = (fgets(buf, sizeof(buf), p) != nullptr);
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return got;
}

static const std::string kTestPluginsDir = TEST_PLUGIN_DIR;

// ---------------------------------------------------------------------------
// SubprocessPipe tests
// ---------------------------------------------------------------------------

TEST(SubprocessPipe, SpawnEcho) {
#ifdef _WIN32
    auto pipe = SubprocessPipe::Spawn("cmd.exe", {"/s", "/c", "echo hello_pipe"});
#else
    auto pipe = SubprocessPipe::Spawn("/bin/sh", {"-c", "echo hello_pipe"});
#endif
    ASSERT_NE(pipe, nullptr);
    std::string line;
    ASSERT_TRUE(pipe->ReadLine(line, 5000)) << "no stdout from echo";
    EXPECT_NE(line.find("hello_pipe"), std::string::npos) << line;
}

TEST(SubprocessPipe, SpawnAndReadLine) {
    if (!IsPythonAvailable()) GTEST_SKIP() << "Python not available";

#ifdef _WIN32
    std::string pyExe = "python";
#else
    std::string pyExe = "python3";
#endif
    auto pipe = SubprocessPipe::Spawn(pyExe, {"-c", "print('hello_pipe')"});
    ASSERT_NE(pipe, nullptr);
    std::string line;
    ASSERT_TRUE(pipe->ReadLine(line, 5000));
    EXPECT_NE(line.find("hello_pipe"), std::string::npos);
}

TEST(SubprocessPipe, WriteAndRead) {
    if (!IsPythonAvailable()) GTEST_SKIP() << "Python not available";

#ifdef _WIN32
    std::string pyExe = "python";
#else
    std::string pyExe = "python3";
#endif

    auto pipe = SubprocessPipe::Spawn(pyExe,
        {"-c", "import sys; line=sys.stdin.readline(); sys.stdout.write('GOT:'+line); sys.stdout.flush()"});
    ASSERT_NE(pipe, nullptr);

    ASSERT_TRUE(pipe->WriteLine("test_input"));
    std::string line;
    ASSERT_TRUE(pipe->ReadLine(line, 5000));
    EXPECT_EQ(line, "GOT:test_input");
}

TEST(SubprocessPipe, InvalidCommand) {
    EXPECT_THROW(
        SubprocessPipe::Spawn("__nonexistent_binary_42__", {}),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// StdioMCPAdapter discovery tests
// ---------------------------------------------------------------------------

TEST(StdioMCPAdapter, DiscoverTools_StdioEcho) {
    if (!IsPythonAvailable()) GTEST_SKIP() << "Python not available";

    std::string serverPath = (fs::path(kTestPluginsDir)
        / "stdio-echo" / "scripts" / "server.py").string();

#ifdef _WIN32
    std::string pyExe = "python";
#else
    std::string pyExe = "python3";
#endif

    auto tools = StdioMCPAdapter::DiscoverTools("stdio-echo", pyExe, {serverPath});
    ASSERT_EQ(tools.size(), 2u);

    std::vector<std::string> names;
    for (const auto& t : tools) names.push_back(t.m_Name);

    EXPECT_NE(std::find(names.begin(), names.end(), "echo"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "add"),  names.end());
}

// ---------------------------------------------------------------------------
// StdioMCPAdapter execution tests
// ---------------------------------------------------------------------------

TEST(StdioMCPAdapter, ExecuteEcho_ReturnsMessage) {
    if (!IsPythonAvailable()) GTEST_SKIP() << "Python not available";

    std::string serverPath = (fs::path(kTestPluginsDir)
        / "stdio-echo" / "scripts" / "server.py").string();

#ifdef _WIN32
    std::string pyExe = "python";
#else
    std::string pyExe = "python3";
#endif

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"message", {{"type", "string"}}}}}
    };

    auto adapter = std::make_shared<StdioMCPAdapter>(
        "stdio-echo", pyExe, std::vector<std::string>{serverPath},
        "echo", "Echoes input", schema);

    nlohmann::json request = {
        {"command", "echo"},
        {"payload", {{"message", "hello_mcp"}}}
    };
    auto result = adapter->ExecuteAsync(request).get();

    EXPECT_FALSE(result.value("isError", false));
    ASSERT_TRUE(result.contains("content"));
    ASSERT_FALSE(result["content"].empty());
    std::string text = result["content"][0].value("text", "");
    EXPECT_NE(text.find("hello_mcp"), std::string::npos);
}

TEST(StdioMCPAdapter, ExecuteAdd_ReturnsSum) {
    if (!IsPythonAvailable()) GTEST_SKIP() << "Python not available";

    std::string serverPath = (fs::path(kTestPluginsDir)
        / "stdio-echo" / "scripts" / "server.py").string();

#ifdef _WIN32
    std::string pyExe = "python";
#else
    std::string pyExe = "python3";
#endif

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}
    };

    auto adapter = std::make_shared<StdioMCPAdapter>(
        "stdio-echo", pyExe, std::vector<std::string>{serverPath},
        "add", "Adds two numbers", schema);

    nlohmann::json request = {
        {"command", "add"},
        {"payload", {{"a", 17}, {"b", 25}}}
    };
    auto result = adapter->ExecuteAsync(request).get();

    EXPECT_FALSE(result.value("isError", false));
    ASSERT_TRUE(result.contains("content"));
    std::string text = result["content"][0].value("text", "");
    EXPECT_EQ(text, "42");
}
