#include <server/HttplibServer.h>
#include <security/SecurityHeaders.h>
#include <iostream>

HttplibServer::HttplibServer() = default;

void HttplibServer::AddRoute(const std::string& method, const std::string& path,
                               RouteHandler handler) {
    m_Routes.emplace_back(method, path, std::move(handler));
}

void HttplibServer::Listen(const std::string& host, int port) {
    auto secHeaders = SecurityHeaders::GetDefaults();

    for (auto& [method, path, handler] : m_Routes) {
        auto routeHandler = [handler, secHeaders](const httplib::Request& req,
                                                    httplib::Response& res) {
            // Apply security headers
            for (const auto& [k, v] : secHeaders) {
                res.set_header(k, v);
            }
            res.set_header("Content-Type", "application/json");

            std::string clientIp = req.remote_addr;
            std::string auth = req.get_header_value("Authorization");
            handler(req.body, clientIp, auth, [&res](int status, const std::string& response) {
                res.status = status;
                res.set_content(response, "application/json");
            });
        };

        if (method == "POST") {
            m_Server.Post(path, routeHandler);
        } else if (method == "GET") {
            m_Server.Get(path, routeHandler);
        }
    }

    std::cout << "HttplibServer listening on " << host << ":" << port << std::endl;
    m_Server.listen(host, port);
}

void HttplibServer::Stop() {
    m_Server.stop();
}
