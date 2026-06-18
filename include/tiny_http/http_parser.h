#pragma once

#include "tiny_http/http_request.h"

#include <string>

namespace tiny_http {
 
class HttpParser {
public:
    ParseResult parse(const std::string& buffer) const;

private:
    static bool parse_request_line(const std::string& line, HttpRequest& request, std::string& error);
    static bool parse_header_line(const std::string& line, HttpRequest& request, std::string& error);
};

} // namespace tiny_http

