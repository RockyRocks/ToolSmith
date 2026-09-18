#include <gtest/gtest.h>
#include <skills/SkillToolAdapter.h>
#include <skills/SkillEngine.h>
#include <llm/ILLMProvider.h>

// ---------------------------------------------------------------------------
// Shared mock LLM provider
// ---------------------------------------------------------------------------

namespace {

class MockLLMProvider : public ILLMProvider {
public:
    mutable LLMRequest lastRequest;
    std::string responseContent = "mock LLM response";

    std::future<LLMResponse> Complete(const LLMRequest& req) override {
        lastRequest = req;
        std::string content = responseContent;
        return std::async(std::launch::async, [content]() {
            return LLMResponse{content, 10, 5, "stop", {}};
        });
    }

    std::string GetProviderName() const override { return "mock"; }
    bool IsAvailable() const override { return true; }
};

SkillDefinition MakeSkill(const std::string& name = "greet",
                           const std::string& tmpl = "Hello {{name}}!",
                           const std::vector<std::string>& vars = {"name"}) {
    SkillDefinition s;
    s.m_Name = name;
    s.m_Description = "Greet someone";
    s.m_PromptTemplate = tmpl;
    s.m_DefaultModel = "test-model";
    s.m_RequiredVariables = vars;
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Metadata tests
// ---------------------------------------------------------------------------

TEST(SkillToolAdapterTest, MetadataUsesSkillName) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);

    auto meta = adapter.GetMetadata();
    EXPECT_EQ(meta.m_Name, "greet");
    EXPECT_EQ(meta.m_Description, "Greet someone");
    EXPECT_EQ(meta.m_DefaultModel, "test-model");
    EXPECT_FALSE(meta.m_Hidden);
}

TEST(SkillToolAdapterTest, MetadataSchemaContainsRequiredVariables) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill("review", "Review {{code}}", {"code"}), provider);

    auto meta = adapter.GetMetadata();
    ASSERT_TRUE(meta.m_InputSchema.contains("properties"));
    EXPECT_TRUE(meta.m_InputSchema["properties"].contains("code"));

    // required array should list "code"
    ASSERT_TRUE(meta.m_InputSchema.contains("required"));
    auto req = meta.m_InputSchema["required"];
    EXPECT_EQ(req.size(), 1u);
    EXPECT_EQ(req[0], "code");
}

TEST(SkillToolAdapterTest, MetadataAlwaysIncludesModelAndParametersOverrides) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);

    auto meta = adapter.GetMetadata();
    EXPECT_TRUE(meta.m_InputSchema["properties"].contains("model"));
    EXPECT_TRUE(meta.m_InputSchema["properties"].contains("parameters"));
}

TEST(SkillToolAdapterTest, MetadataSourceDefaultsToJsonSkill) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);
    EXPECT_EQ(adapter.GetMetadata().m_Source, ToolSource::JsonSkill);
}

TEST(SkillToolAdapterTest, MetadataSourceCanBeSetToPlugin) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider, ToolSource::Plugin);
    EXPECT_EQ(adapter.GetMetadata().m_Source, ToolSource::Plugin);
}

// ---------------------------------------------------------------------------
// Execution tests
// ---------------------------------------------------------------------------

TEST(SkillToolAdapterTest, ExecuteRendersPromptAndCallsLLM) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);

    nlohmann::json request = {
        {"command", "greet"},
        {"payload", {{"name", "World"}}}
    };

    auto result = adapter.ExecuteAsync(request).get();

    EXPECT_EQ(result["status"], "ok");
    EXPECT_EQ(result["skill"], "greet");
    EXPECT_EQ(result["content"], "mock LLM response");

    // LLM should have received the rendered prompt
    ASSERT_GE(provider->lastRequest.m_Messages.size(), 1u);
    auto userMsg = provider->lastRequest.m_Messages.back();
    EXPECT_EQ(userMsg["role"], "user");
    EXPECT_EQ(userMsg["content"], "Hello World!");
}

TEST(SkillToolAdapterTest, ExecuteReturnsErrorOnMissingVariable) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);

    nlohmann::json request = {
        {"command", "greet"},
        {"payload", nlohmann::json::object()} // missing "name"
    };

    auto result = adapter.ExecuteAsync(request).get();
    EXPECT_EQ(result["status"], "error");
    EXPECT_NE(result["error"].get<std::string>().find("name"), std::string::npos);
}

TEST(SkillToolAdapterTest, ExecuteInjectsSystemPromptAndRules) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill = MakeSkill();
    skill.m_SystemPrompt = "You are a greeter.";
    skill.m_Rules = {"Always be polite", "Use the person's name"};

    SkillToolAdapter adapter(skill, provider);
    nlohmann::json request = {
        {"command", "greet"},
        {"payload", {{"name", "Alice"}}}
    };

    auto result = adapter.ExecuteAsync(request).get();
    EXPECT_EQ(result["status"], "ok");

    ASSERT_GE(provider->lastRequest.m_Messages.size(), 2u);
    auto sysMsg = provider->lastRequest.m_Messages[0]["content"].get<std::string>();
    EXPECT_NE(sysMsg.find("You are a greeter."), std::string::npos);
    EXPECT_NE(sysMsg.find("1. Always be polite"), std::string::npos);
    EXPECT_NE(sysMsg.find("2. Use the person's name"), std::string::npos);
}

TEST(SkillToolAdapterTest, ExecuteUsesSkillDefaultModel) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);

    nlohmann::json request = {
        {"command", "greet"},
        {"payload", {{"name", "Bob"}}}
    };

    adapter.ExecuteAsync(request).get();
    EXPECT_EQ(provider->lastRequest.m_Model, "test-model");
}

TEST(SkillToolAdapterTest, ExecuteAllowsModelOverrideInPayload) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeSkill(), provider);

    nlohmann::json request = {
        {"command", "greet"},
        {"payload", {{"name", "Carol"}, {"model", "override-model"}}}
    };

    adapter.ExecuteAsync(request).get();
    EXPECT_EQ(provider->lastRequest.m_Model, "override-model");
}

TEST(SkillToolAdapterTest, ExecuteAppliesDefaultParameters) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill = MakeSkill();
    skill.m_DefaultParameters = {{"temperature", 0.3}, {"max_tokens", 512}};

    SkillToolAdapter adapter(skill, provider);
    nlohmann::json request = {
        {"command", "greet"},
        {"payload", {{"name", "Dave"}}}
    };

    adapter.ExecuteAsync(request).get();
    EXPECT_EQ(provider->lastRequest.m_Parameters["temperature"], 0.3);
    EXPECT_EQ(provider->lastRequest.m_Parameters["max_tokens"], 512);
}

TEST(SkillToolAdapterTest, ExecuteAllowsParameterOverride) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill = MakeSkill();
    skill.m_DefaultParameters = {{"temperature", 0.3}};

    SkillToolAdapter adapter(skill, provider);
    nlohmann::json request = {
        {"command", "greet"},
        {"payload", {{"name", "Eve"}, {"parameters", {{"temperature", 0.9}}}}}
    };

    adapter.ExecuteAsync(request).get();
    EXPECT_EQ(provider->lastRequest.m_Parameters["temperature"], 0.9);
}

// ---------------------------------------------------------------------------
// Command skill tests
// ---------------------------------------------------------------------------

namespace {

SkillDefinition MakeCommandSkill(const std::string& name = "cmd_test",
                                  const std::string& cmdTmpl = "echo {{msg}}",
                                  const std::vector<std::string>& vars = {"msg"}) {
    SkillDefinition s;
    s.m_Name             = name;
    s.m_Description      = "A command skill";
    s.m_Type             = SkillType::Command;
    s.m_CommandTemplate  = cmdTmpl;
    s.m_RequiredVariables = vars;
    return s;
}

} // anonymous namespace

TEST(SkillToolAdapterTest, CommandSkill_ExecutesAndCapturesOutput) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeCommandSkill(), provider);

    nlohmann::json request = {{"payload", {{"msg", "hello_from_test"}}}};
    auto result = adapter.ExecuteAsync(request).get();

    EXPECT_EQ(result["status"], "ok");
    EXPECT_EQ(result["skill"], "cmd_test");
    // echo output should contain the argument
    EXPECT_NE(result["content"].get<std::string>().find("hello_from_test"),
              std::string::npos);
}

TEST(SkillToolAdapterTest, CommandSkill_MissingVariable_ReturnsError) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillToolAdapter adapter(MakeCommandSkill(), provider);

    nlohmann::json request = {{"payload", nlohmann::json::object()}}; // no "msg"
    auto result = adapter.ExecuteAsync(request).get();

    EXPECT_EQ(result["status"], "error");
    EXPECT_NE(result["error"].get<std::string>().find("msg"), std::string::npos);
}

TEST(SkillToolAdapterTest, CommandSkill_GetMetadata_DoesNotInjectRulesIntoDescription) {
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill = MakeCommandSkill();
    skill.m_Description = "Base description.";
    skill.m_Rules = {"Rule alpha", "Rule beta"};

    SkillToolAdapter adapter(skill, provider);
    auto meta = adapter.GetMetadata();

    EXPECT_EQ(meta.m_Description, "Base description.");
    EXPECT_EQ(meta.m_Description.find("Rule alpha"), std::string::npos);
    EXPECT_EQ(meta.m_Description.find("Rule beta"), std::string::npos);
    EXPECT_EQ(meta.m_Description.find("Usage rules:"), std::string::npos);
}

TEST(SkillToolAdapterTest, LlmSkill_GetMetadata_RulesNotInDescription) {
    // LLM skills inject rules via system prompt, not description — description stays clean
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill = MakeSkill();
    skill.m_Rules = {"Rule only for system prompt"};

    SkillToolAdapter adapter(skill, provider);
    auto meta = adapter.GetMetadata();

    EXPECT_EQ(meta.m_Description.find("Rule only for system prompt"), std::string::npos);
}

TEST(SkillToolAdapterTest, CommandSkill_NoVariables_ExecutesDirectly) {
    // A zero-variable command skill: no payload needed, template runs as-is
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill;
    skill.m_Name            = "no_vars";
    skill.m_Type            = SkillType::Command;
    skill.m_CommandTemplate = "echo static_output";
    // m_RequiredVariables intentionally empty

    SkillToolAdapter adapter(skill, provider);
    nlohmann::json request = {{"payload", nlohmann::json::object()}};
    auto result = adapter.ExecuteAsync(request).get();

    EXPECT_EQ(result["status"], "ok");
    EXPECT_EQ(result["skill"], "no_vars");
    EXPECT_NE(result["content"].get<std::string>().find("static_output"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Parallel execution test
// ---------------------------------------------------------------------------

TEST(SkillToolAdapterTest, MultipleAdaptersRunInParallel) {
    // Launch multiple adapters concurrently — each future should run independently.
    auto provider = std::make_shared<MockLLMProvider>();
    SkillDefinition skill = MakeSkill();

    std::vector<std::future<nlohmann::json>> futures;
    for (int i = 0; i < 5; ++i) {
        SkillToolAdapter adapter(skill, provider);
        nlohmann::json request = {
            {"command", "greet"},
            {"payload", {{"name", "User" + std::to_string(i)}}}
        };
        futures.push_back(adapter.ExecuteAsync(request));
    }

    for (auto& f : futures) {
        auto result = f.get();
        EXPECT_EQ(result["status"], "ok");
    }
}
