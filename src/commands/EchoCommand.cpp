#include <commands/EchoCommand.h>
#include <nlohmann/json.hpp>
#include <future>

class EchoCommand : public ICommandStrategy {
public:
    explicit EchoCommand(bool hidden) : m_Hidden(hidden) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        return std::async(std::launch::deferred, [request]() {
            return nlohmann::json{{"status", "ok"}, {"echo", request}};
        });
    }

    ToolMetadata GetMetadata() const override {
        ToolMetadata meta;
        meta.m_Name = "echo";
        meta.m_Description = "Echo back the input message";
        meta.m_InputSchema = {
            {"type", "object"},
            {"properties", {
                {"message", {{"type", "string"}, {"description", "The message to echo back"}}}
            }},
            {"required", nlohmann::json::array({"message"})}
        };
        meta.m_Hidden = m_Hidden;
        meta.m_Source = ToolSource::BuiltIn;
        return meta;
    }

private:
    bool m_Hidden = false;
};

std::shared_ptr<ICommandStrategy> CreateEchoCommand(bool hidden) {
    return std::make_shared<EchoCommand>(hidden);
}
