#include <gtest/gtest.h>
#include <core/Hashline.h>

TEST(HashlineTest, LineHashHelloIsFnv1aLow16) {
    EXPECT_EQ(Hashline::LineHash("hello"), "2cab");
}

TEST(HashlineTest, NormalizeStripsAndCollapsesTabs) {
    EXPECT_EQ(Hashline::NormalizeLine("  hello  "), "hello");
    EXPECT_EQ(Hashline::NormalizeLine("a\tb"), "a b");
    EXPECT_EQ(Hashline::LineHash("  hello  "), Hashline::LineHash("hello"));
    EXPECT_EQ(Hashline::LineHash("a\tb"), Hashline::LineHash("a b"));
}

TEST(HashlineTest, EmptyLineHasStableHash) {
    EXPECT_EQ(Hashline::LineHash(""), "9dc5");
}

TEST(HashlineTest, ParseFileSplitsLfAndCrlf) {
    auto a = Hashline::ParseFile("one\ntwo\n");
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].number, 1);
    EXPECT_EQ(a[0].text, "one");
    EXPECT_EQ(a[1].text, "two");
    auto b = Hashline::ParseFile("one\r\ntwo\r\n");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0].text, "one");
}

TEST(HashlineTest, ParseFileNoTrailingEmptyLine) {
    auto lines = Hashline::ParseFile("only");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, "only");
}

TEST(HashlineTest, FormatReadHashlineAndLimit) {
    auto lines = Hashline::ParseFile("alpha\nbeta\ngamma\n");
    bool truncated = false;
    int next = 0;
    std::string body = Hashline::FormatRead(lines, 0, 2, true, truncated, next);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(next, 3);
    EXPECT_NE(body.find("1|"), std::string::npos);
    EXPECT_NE(body.find("|alpha"), std::string::npos);
    EXPECT_EQ(body.find("gamma"), std::string::npos);
}

TEST(HashlineTest, FormatReadOffsetPastEndEmitsNothing) {
    auto lines = Hashline::ParseFile("alpha\n");
    bool truncated = false;
    int next = 0;
    std::string body = Hashline::FormatRead(lines, 5, 10, true, truncated, next);
    EXPECT_TRUE(body.empty());
    EXPECT_FALSE(truncated);
}

TEST(HashlineTest, ApplyHunksSuccessReplacesRange) {
    auto lines = Hashline::ParseFile("aaa\nbbb\nccc\n");
    Hashline::Hunk h;
    h.startLine = 2;
    h.startHash = Hashline::LineHash("bbb");
    h.content = "BBB\n";
    auto [next, err] = Hashline::ApplyHunks(lines, {h});
    EXPECT_TRUE(err.empty());
    EXPECT_NE(next.find("BBB"), std::string::npos);
    EXPECT_EQ(next.find("bbb"), std::string::npos);
}

TEST(HashlineTest, ApplyHunksStaleHashWritesNothing) {
    auto lines = Hashline::ParseFile("aaa\nbbb\n");
    Hashline::Hunk h;
    h.startLine = 1;
    h.startHash = "dead";
    h.content = "nope\n";
    auto [next, err] = Hashline::ApplyHunks(lines, {h});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Stale"), std::string::npos);
    EXPECT_TRUE(next.empty());
}

TEST(HashlineTest, ApplyHunksOverlapFails) {
    auto lines = Hashline::ParseFile("a\nb\nc\nd\n");
    Hashline::Hunk a, b;
    a.startLine = 1; a.startHash = Hashline::LineHash("a"); a.endLine = 2;
    a.endHash = Hashline::LineHash("b"); a.content = "x\n";
    b.startLine = 2; b.startHash = Hashline::LineHash("b"); b.endLine = 3;
    b.endHash = Hashline::LineHash("c"); b.content = "y\n";
    auto [next, err] = Hashline::ApplyHunks(lines, {a, b});
    EXPECT_EQ(err, "Overlapping hunks are not allowed");
    EXPECT_TRUE(next.empty());
}

TEST(HashlineTest, ApplyHunksEmptyFails) {
    auto lines = Hashline::ParseFile("a\n");
    auto [next, err] = Hashline::ApplyHunks(lines, {});
    EXPECT_EQ(err, "hunks must not be empty");
}

TEST(HashlineTest, ApplyHunksInvalidStartLineFails) {
    auto lines = Hashline::ParseFile("a\n");
    Hashline::Hunk h;
    h.startLine = 0;
    h.startHash = "0000";
    h.content = "x";
    auto [next, err] = Hashline::ApplyHunks(lines, {h});
    EXPECT_FALSE(err.empty());
}

TEST(HashlineTest, ApplyHunksEndBeforeStartFails) {
    auto lines = Hashline::ParseFile("a\nb\n");
    Hashline::Hunk h;
    h.startLine = 2;
    h.startHash = Hashline::LineHash("b");
    h.endLine = 1;
    h.endHash = Hashline::LineHash("a");
    h.content = "x";
    auto [next, err] = Hashline::ApplyHunks(lines, {h});
    EXPECT_EQ(err, "Hunk end line is before start line");
}

TEST(HashlineTest, ApplyHunksMissingLineFails) {
    auto lines = Hashline::ParseFile("a\n");
    Hashline::Hunk h;
    h.startLine = 9;
    h.startHash = "abcd";
    h.content = "x";
    auto [next, err] = Hashline::ApplyHunks(lines, {h});
    EXPECT_NE(err.find("does not exist"), std::string::npos);
}
