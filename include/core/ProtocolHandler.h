#pragma once
#include <core/IRequestHandler.h>
#include <commands/CommandRegistry.h>
#include <validation/InputSanitizer.h>
#include <security/RateLimiter.h>
#include <security/ApiKeyValidator.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <validation/JsonSchemaValidator.h>

class ProtocolHandler : public IRequestHandler {
public:
    ProtocolHandler(std::shared_ptr<CommandRegistry> registry,
                    std::shared_ptr<RateLimiter> rateLimiter,
                    std::shared_ptr<ApiKeyValidator> apiKeyValidator,
                    size_t maxBodySize = 1048576);

    // IRequestHandler interface
    nlohmann::json Handle(const nlohmann::json& request) override;

    // Full request handling with security checks
    std::string HandleRequest(const std::string& body, const std::string& clientIp,
                               const std::string& authHeader = "");

    bool ValidateRequest(const nlohmann::json& req);
    std::string CreateResponse(const nlohmann::json& data);

private:
    std::shared_ptr<CommandRegistry> m_Registry;
    std::shared_ptr<RateLimiter> m_RateLimiter;
    std::shared_ptr<ApiKeyValidator> m_ApiKeyValidator;
    size_t m_MaxBodySize;
    JsonSchemaValidator m_RequestValidator;
    JsonSchemaValidator m_ResponseValidator;
};
