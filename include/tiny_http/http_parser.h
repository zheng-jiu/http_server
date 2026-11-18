#pragma once

#include "tiny_http/http_request.h"
#include "tiny_http/chunk_decoder.h"

#include <string>
#include <cstddef>

namespace tiny_http {
 
class HttpParser {
public:
    explicit HttpParser(std::size_t max_body_size = 1048576) 
        : max_body_size_(max_body_size) {}

    ParseResult parse(
        const std::string& buffer,
        ChunkDecoder* chunks = nullptr
    ) const;

private:
    static bool parse_request_line(const std::string& line, HttpRequest& request, std::string& error);
    static bool parse_header_line(const std::string& line, HttpRequest& request, std::string& error);

    std::size_t max_body_size_;
};

} // namespace tiny_http

