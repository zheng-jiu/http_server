#pragma once

#include <map>
#include <string>

namespace tiny_http {

enum class ParseStatus {
    Complete,
    Incomplete,
    BadRequest
};

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    
    bool keep_alive() const;
    std::string header(const std::string& key) const;
};

struct ParseResult {
    ParseStatus status{ParseStatus::Incomplete};
    HttpRequest request;
    std::string error;
    std::size_t consumed{0};
};

} // namespace tiny_http
