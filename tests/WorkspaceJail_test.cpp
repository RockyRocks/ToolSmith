#include <gtest/gtest.h>
#include <core/WorkspaceJail.h>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

class WorkspaceJailTest : public ::testing::Test {
protected:
    fs::path root;
    fs::path outside;

    void SetUp() override {
        root = fs::temp_directory_path() / "ts_jail_root";
        outside = fs::temp_directory_path() / "ts_jail_outside";
        fs::remove_all(root);
        fs::remove_all(outside);
        fs::create_directories(root);
        fs::create_directories(outside);
        std::ofstream(root / "inside.txt") << "ok\n";
        std::ofstream(outside / "secret.txt") << "nope\n";
    }
    void TearDown() override {
        fs::remove_all(root);
        fs::remove_all(outside);
    }
};

TEST_F(WorkspaceJailTest, OpenMissingRootThrows) {
    EXPECT_THROW(WorkspaceJail::Open(root / "missing"), std::runtime_error);
}

TEST_F(WorkspaceJailTest, RelativePathResolvesInside) {
    auto jail = WorkspaceJail::Open(root);
    auto r = jail.Resolve("inside.txt", false);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.outsideWorkspace);
}

TEST_F(WorkspaceJailTest, WriteOutsideAbsoluteFails) {
    auto jail = WorkspaceJail::Open(root);
    auto r = jail.Resolve((outside / "secret.txt").string(), true);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.find("outside"), std::string::npos);
}

TEST_F(WorkspaceJailTest, ReadOutsideAbsoluteIsFlagged) {
    auto jail = WorkspaceJail::Open(root);
    auto r = jail.Resolve((outside / "secret.txt").string(), false);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.outsideWorkspace);
}

TEST_F(WorkspaceJailTest, TraversalDotDotWriteFails) {
    auto jail = WorkspaceJail::Open(root);
    auto r = jail.Resolve("../ts_jail_outside/secret.txt", true);
    EXPECT_FALSE(r.ok());
}

TEST_F(WorkspaceJailTest, EmptyPathFails) {
    auto jail = WorkspaceJail::Open(root);
    auto r = jail.Resolve("", false);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.find("path"), std::string::npos);
}

TEST_F(WorkspaceJailTest, DetectRootFindsGit) {
    fs::create_directories(root / ".git");
    auto nested = root / "src";
    fs::create_directories(nested);
    auto detected = WorkspaceJail::DetectRoot(nested);
    EXPECT_EQ(fs::equivalent(detected, root), true);
}

TEST_F(WorkspaceJailTest, AllowListPermitsWrite) {
    auto jail = WorkspaceJail::Open(root, {outside});
    auto r = jail.Resolve((outside / "secret.txt").string(), true);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.outsideWorkspace);
}
