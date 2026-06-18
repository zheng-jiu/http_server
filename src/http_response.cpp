#include "tiny_http/http_response.h"

#include <sstream>

namespace tiny_http {
   
HttpResponse::HttpResponse(int status, std::string reason)
    : status_(status), reason_(std::move(reason)) {}

void HttpResponse::set_header(std::string key, std::string value) {
    headers_[std::move(key)] = std::move(value);
}

void HttpResponse::set_body(std::string body, std::string content_type) {
    body_ = std::move(body);
    set_header("Content-Type", std::move(content_type));
    set_header("Content-Length", std::to_string(body_.size()));
}

void HttpResponse::set_keep_alive(bool keep_alive) {
    keep_alive_ = keep_alive;
}

std::string HttpResponse::serialize() const {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_ << ' ' << reason_ << "\r\n";
    for (const auto& [key, value] : headers_) {
        response << key << ": " << value << "\r\n";
    }
    response << "Connection: " << (keep_alive_ ? "keep-alive" : "close") << "\r\n";
    response << "\r\n";
    response << body_;
    return response.str();
}

} // namespace tiny_http
