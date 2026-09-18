#include <gtest/gtest.h>
#include <commands/CommandRegistry.h>
#include <commands/EchoCommand.h>
#include <stdexcept>

TEST(CommandRegistryTest, RegisterAndResolve) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    EXPECT_NE(reg.Resolve("echo"), nullptr);
}

TEST(CommandRegistryTest, ResolveUnknown) {
    CommandRegistry reg;
    EXPECT_EQ(reg.Resolve("nonexistent"), nullptr);
}

TEST(CommandRegistryTest, HasCommand) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    EXPECT_TRUE(reg.HasCommand("echo"));
    EXPECT_FALSE(reg.HasCommand("nonexistent"));
}

TEST(CommandRegistryTest, ListCommands) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    reg.RegisterCommand("test", CreateEchoCommand());
    auto cmds = reg.ListCommands();
    EXPECT_EQ(cmds.size(), 2u);
}

TEST(CommandRegistryTest, EmptyNameThrows) {
    CommandRegistry reg;
    EXPECT_THROW(reg.RegisterCommand("", CreateEchoCommand()), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// ListToolMetadata() tests
// ---------------------------------------------------------------------------

TEST(CommandRegistryTest, MetadataFromSelfDescribingCommand) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());

    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);

    auto& meta = metadata[0];
    EXPECT_EQ(meta.m_Name, "echo");
    EXPECT_EQ(meta.m_Description, "Echo back the input message");
    EXPECT_TRUE(meta.m_InputSchema.contains("properties"));
    EXPECT_TRUE(meta.m_InputSchema["properties"].contains("message"));
}

namespace {

// Bare command that provides minimal metadata
class BareCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        return std::async(std::launch::async, [request]() {
            return nlohmann::json{{"status", "ok"}};
        });
    }

    ToolMetadata GetMetadata() const override {
        return {};
    }
};

// Command with custom defaultModel and defaultParameters
class ModeledCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        return std::async(std::launch::async, [request]() {
            return nlohmann::json{{"status", "ok"}};
        });
    }

    ToolMetadata GetMetadata() const override {
        return {
            "modeled",
            "A command with a default model",
            {
                {"type", "object"},
                {"properties", {
                    {"input", {{"type", "string"}}}
                }}
            },
            "claude-opus",
            {{"temperature", 0.1}, {"max_tokens", 512}}
        };
    }
};

} // anonymous namespace

TEST(CommandRegistryTest, FallbackMetadataForCommandWithoutOverride) {
    CommandRegistry reg;
    reg.RegisterCommand("bare_tool", std::make_shared<BareCommand>());

    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);

    auto& meta = metadata[0];
    EXPECT_EQ(meta.m_Name, "bare_tool");
    EXPECT_EQ(meta.m_Description, "Execute the bare_tool command");
    EXPECT_TRUE(meta.m_InputSchema.contains("additionalProperties"));
    EXPECT_TRUE(meta.m_InputSchema["additionalProperties"].get<bool>());
}

TEST(CommandRegistryTest, DefaultModelAndParametersPassThrough) {
    CommandRegistry reg;
    reg.RegisterCommand("modeled", std::make_shared<ModeledCommand>());

    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);

    auto& meta = metadata[0];
    EXPECT_EQ(meta.m_Name, "modeled");
    EXPECT_EQ(meta.m_DefaultModel, "claude-opus");
    EXPECT_EQ(meta.m_DefaultParameters["temperature"], 0.1);
    EXPECT_EQ(meta.m_DefaultParameters["max_tokens"], 512);
}

// ---------------------------------------------------------------------------
// Hidden tool filtering tests
// ---------------------------------------------------------------------------

namespace {

class HiddenCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        return std::async(std::launch::async, []() {
            return nlohmann::json{{"status", "ok"}};
        });
    }

    ToolMetadata GetMetadata() const override {
        ToolMetadata meta;
        meta.m_Name = "hidden_tool";
        meta.m_Description = "This tool is hidden";
        meta.m_Hidden = true;
        return meta;
    }
};

class SkillSourceCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        return std::async(std::launch::async, []() {
            return nlohmann::json{{"status", "ok"}};
        });
    }

    ToolMetadata GetMetadata() const override {
        ToolMetadata meta;
        meta.m_Name = "my_skill";
        meta.m_Description = "A promoted skill tool";
        meta.m_Source = ToolSource::JsonSkill;
        return meta;
    }
};

class PluginSourceCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        return std::async(std::launch::async, []() {
            return nlohmann::json{{"status", "ok"}};
        });
    }

    ToolMetadata GetMetadata() const override {
        ToolMetadata meta;
        meta.m_Name = "plugin_tool";
        meta.m_Description = "A plugin tool";
        meta.m_Source = ToolSource::Plugin;
        return meta;
    }
};

} // anonymous namespace

TEST(CommandRegistryTest, HiddenToolExcludedFromListToolMetadata) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    reg.RegisterCommand("hidden_tool", std::make_shared<HiddenCommand>());

    auto metadata = reg.ListToolMetadata();
    // Only echo should appear; hidden_tool is filtered out
    ASSERT_EQ(metadata.size(), 1u);
    EXPECT_EQ(metadata[0].m_Name, "echo");
}

TEST(CommandRegistryTest, HiddenToolIsStillResolvable) {
    // Hidden only means "omit from tools/list", not "refuse to execute"
    CommandRegistry reg;
    reg.RegisterCommand("hidden_tool", std::make_shared<HiddenCommand>());

    EXPECT_NE(reg.Resolve("hidden_tool"), nullptr);
    EXPECT_TRUE(reg.HasCommand("hidden_tool"));
}

TEST(CommandRegistryTest, ListCommandsIncludesHiddenTools) {
    // ListCommands() is the internal debug list — includes everything
    CommandRegistry reg;
    reg.RegisterCommand("visible", CreateEchoCommand());
    reg.RegisterCommand("hidden_tool", std::make_shared<HiddenCommand>());

    auto cmds = reg.ListCommands();
    EXPECT_EQ(cmds.size(), 2u);
}

TEST(CommandRegistryTest, ToolSourceDefaultsToBuiltIn) {
    CommandRegistry reg;
    reg.RegisterCommand("bare_tool", std::make_shared<BareCommand>());

    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);
    EXPECT_EQ(metadata[0].m_Source, ToolSource::BuiltIn);
}

TEST(CommandRegistryTest, JsonSkillSourcePreserved) {
    CommandRegistry reg;
    reg.RegisterCommand("my_skill", std::make_shared<SkillSourceCommand>());

    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);
    EXPECT_EQ(metadata[0].m_Source, ToolSource::JsonSkill);
}

TEST(CommandRegistryTest, PluginSourcePreserved) {
    CommandRegistry reg;
    reg.RegisterCommand("plugin_tool", std::make_shared<PluginSourceCommand>());

    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);
    EXPECT_EQ(metadata[0].m_Source, ToolSource::Plugin);
}

TEST(CommandRegistryTest, MixedVisibleAndHiddenTools) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    reg.RegisterCommand("hidden1", std::make_shared<HiddenCommand>());
    reg.RegisterCommand("skill_tool", std::make_shared<SkillSourceCommand>());

    auto metadata = reg.ListToolMetadata();
    // echo + skill_tool visible; hidden1 filtered
    EXPECT_EQ(metadata.size(), 2u);
    for (const auto& m : metadata) {
        EXPECT_NE(m.m_Name, "hidden1");
    }
}

// ---------------------------------------------------------------------------
// ExecuteWithChaining() tests
// ---------------------------------------------------------------------------

namespace {

/// Returns a result that includes a "chain" field pointing to nextTool.
class ChainToCommand : public ICommandStrategy {
    std::string m_NextTool;
public:
    explicit ChainToCommand(std::string nextTool) : m_NextTool(std::move(nextTool)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        return std::async(std::launch::async, [n = m_NextTool]() -> nlohmann::json {
            return {
                {"status", "ok"},
                {"chain", {
                    {"tool", n},
                    {"args", nlohmann::json::object()}
                }}
            };
        });
    }
    ToolMetadata GetMetadata() const override { return {}; }
};

/// Terminal command that returns a plain result with no "chain" field.
class FinalResultCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        return std::async(std::launch::async, []() -> nlohmann::json {
            return {{"status", "ok"}, {"result", "final_value"}};
        });
    }
    ToolMetadata GetMetadata() const override { return {}; }
};

/// Always returns a chain back to itself ("loop_chain") — used to test max depth.
class SelfChainCommand : public ICommandStrategy {
public:
    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        return std::async(std::launch::async, []() -> nlohmann::json {
            return {
                {"status", "ok"},
                {"data",   "loop"},
                {"chain",  {{"tool", "loop_chain"}, {"args", nlohmann::json::object()}}}
            };
        });
    }
    ToolMetadata GetMetadata() const override { return {}; }
};

} // anonymous namespace

TEST(CommandRegistryTest, ExecuteWithChaining_NoChainField_ReturnsNormally) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());

    nlohmann::json req = {{"command", "echo"}, {"payload", {{"msg", "test"}}}};
    auto result = reg.ExecuteWithChaining("echo", req);

    EXPECT_EQ(result["status"], "ok");
    EXPECT_FALSE(result.contains("chain"));
}

TEST(CommandRegistryTest, ExecuteWithChaining_SingleChain_ReturnsFinalResult) {
    CommandRegistry reg;
    reg.RegisterCommand("tool_a", std::make_shared<ChainToCommand>("tool_b"));
    reg.RegisterCommand("tool_b", std::make_shared<FinalResultCommand>());

    nlohmann::json req = {{"command", "tool_a"}, {"payload", nlohmann::json::object()}};
    auto result = reg.ExecuteWithChaining("tool_a", req);

    // Must be tool_b's result, not tool_a's chain response
    EXPECT_EQ(result["status"], "ok");
    EXPECT_EQ(result["result"], "final_value");
    EXPECT_FALSE(result.contains("chain"));
}

TEST(CommandRegistryTest, ExecuteWithChaining_MaxDepthExceeded_StopsGracefully) {
    CommandRegistry reg;
    // loop_chain always chains back to itself
    reg.RegisterCommand("loop_chain", std::make_shared<SelfChainCommand>());

    nlohmann::json req = {{"command", "loop_chain"}, {"payload", nlohmann::json::object()}};

    // Must not hang, throw, or recurse infinitely
    auto result = reg.ExecuteWithChaining("loop_chain", req);

    // The command's base result is returned once the depth limit is hit
    EXPECT_EQ(result["status"], "ok");
    EXPECT_EQ(result["data"],   "loop");
    // The "chain" field is still in the returned JSON — we stopped following it,
    // not stripped it
    EXPECT_TRUE(result.contains("chain"));
}

TEST(CommandRegistryTest, DuplicateRegisterThrows) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    EXPECT_THROW(reg.RegisterCommand("echo", CreateEchoCommand()), std::runtime_error);
}

TEST(CommandRegistryTest, ReplaceCommandAllowsHotReload) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    EXPECT_NO_THROW(reg.ReplaceCommand("echo", CreateEchoCommand()));
    EXPECT_TRUE(reg.HasCommand("echo"));
}

TEST(CommandRegistryTest, AdvertisedFilterHidesInstalledTools) {
    CommandRegistry reg;
    reg.RegisterCommand("echo", CreateEchoCommand());
    reg.RegisterCommand("other", CreateEchoCommand());
    reg.SetAdvertised({"echo"});
    auto metadata = reg.ListToolMetadata();
    ASSERT_EQ(metadata.size(), 1u);
    EXPECT_EQ(metadata[0].m_Name, "echo");
    EXPECT_TRUE(reg.HasCommand("other"));
}

TEST(CommandRegistryTest, ExecuteWithChaining_Disabled_DoesNotFollowChain) {
    CommandRegistry reg;
    reg.SetChainingEnabled(false);
    reg.RegisterCommand("tool_a", std::make_shared<ChainToCommand>("tool_b"));
    reg.RegisterCommand("tool_b", std::make_shared<FinalResultCommand>());

    nlohmann::json req = {{"command", "tool_a"}, {"payload", nlohmann::json::object()}};
    auto result = reg.ExecuteWithChaining("tool_a", req);

    EXPECT_EQ(result["status"], "ok");
    EXPECT_TRUE(result.contains("chain"));
    EXPECT_FALSE(result.contains("result"));
}
