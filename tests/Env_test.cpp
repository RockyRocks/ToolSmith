#include <gtest/gtest.h>
#include <core/Env.h>
#include <cstdlib>

TEST(EnvTest, MissingReturnsEmpty) {
    EXPECT_TRUE(GetEnvVar("TOOLSMITH_ENV_TEST_MISSING_ZZZ").empty());
}

TEST(EnvTest, NullOrEmptyNameReturnsEmpty) {
    EXPECT_TRUE(GetEnvVar(nullptr).empty());
    EXPECT_TRUE(GetEnvVar("").empty());
}

TEST(EnvTest, RoundTrip) {
#ifdef _MSC_VER
    ASSERT_EQ(_putenv_s("TOOLSMITH_ENV_TEST_VALUE", "abc"), 0);
#else
    ASSERT_EQ(setenv("TOOLSMITH_ENV_TEST_VALUE", "abc", 1), 0);
#endif
    EXPECT_EQ(GetEnvVar("TOOLSMITH_ENV_TEST_VALUE"), "abc");
#ifdef _MSC_VER
    _putenv_s("TOOLSMITH_ENV_TEST_VALUE", "");
#else
    unsetenv("TOOLSMITH_ENV_TEST_VALUE");
#endif
    EXPECT_TRUE(GetEnvVar("TOOLSMITH_ENV_TEST_VALUE").empty());
}
