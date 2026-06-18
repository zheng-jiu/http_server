#pragma once

#include <map>
#include <string>

namespace tiny_http {

class HttpResponse {
public:
    explicit HttpResponse(int status = 200, std::string reason = "OK");
    
    void set_header(std::string key, std::string value);
    void set_body(std::string body, std::string content_type = "text/plain; charset=utf-8");
    void set_keep_alive(bool keep_alive);
    std::string serialize() const;

private:
    int status_;
    std::string reason_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool keep_alive_{false};
};

} // namespace tiny_http

