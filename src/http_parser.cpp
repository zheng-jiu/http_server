#include "tiny_http/http_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <charconv>
#include <system_error>

namespace tiny_http {

namespace {

std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

bool HttpRequest::keep_alive() const {
    const std::string connection = lower(header("Connection"));
    if (version == "HTTP/1.1") {
        return connection != "close";
    }
    return connection == "keep-alive";
}

std::string HttpRequest::header(const std::string& key) const {
    const std::map<std::string, std::string>::const_iterator it = headers.find(lower(key));
    return it == headers.end() ? std::string{} : it->second;
}

bool HttpParser::parse_request_line(const std::string& line, HttpRequest& request, std::string& error) {
    std::istringstream parts(line);
    if (!(parts >> request.method >> request.target >> request.version)) {
        error = "malformed request line";
        return false;
    }
    std::string extra;
    if (parts >> extra) {
        error = "too many fields in request line";
        return false;
    }
    if (request.version != "HTTP/1.0" && request.version != "HTTP/1.1") {
        error = "unsupported HTTP version";
        return false;
    }
    if (request.target.empty() || request.target[0] != '/') {
        error = "request target must start with /";
        return false;
    }
    return  true;
}

bool HttpParser::parse_header_line(const std::string& line, HttpRequest& request, std::string& error) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
        error = "malformed header";
        return false;
    }
    const std::string key = lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (key.empty()) {
        error = "empty header key";
        return false;
    }
    if (key == "transfer-encoding") {
        error = "Transfer-Encoding is not supported";
        return false;
    }
    if (key == "content-length" && request.headers.find(key) != request.headers.end()) {
        error = "duplicate Content-Length";
        return false;
    }
    request.headers[key] = value;
    return true;
}

ParseResult HttpParser::parse(const std::string& buffer) const {
    ParseResult result;
    const std::size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (buffer.size() > 8192) {
            result.status = ParseStatus::BadRequest;
            result.error = "request header is too large";
        }
        return result;
    }
    if (header_end + 4 > 8192) {
        result.status = ParseStatus::BadRequest;
        result.error = "request header is too large";
        return result;
    }

    const std::string header_block = buffer.substr(0, header_end);
    std::istringstream stream(header_block);
    std::string line;

    if (!std::getline(stream, line)) {
        result.status = ParseStatus::BadRequest;
        result.error = "missing request line";
        return result;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (!parse_request_line(line, result.request, result.error)) {
        result.status = ParseStatus::BadRequest;
        return result;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        if (!parse_header_line(line, result.request, result.error)) {
            result.status = ParseStatus::BadRequest;
            return result;
        }
    }

    std::size_t body_start = header_end + 4;
    std::size_t content_length = 0;

    const auto it = result.request.headers.find("content-length");

    if (it != result.request.headers.end()) {
        const std::string& text = it->second;
        const char* begin = text.data();
        const char* end = begin + text.size();

        const auto [ptr, ec] = std::from_chars(
            begin, end, content_length);

        if (ec != std::errc{} || ptr != end) {
            result.status = ParseStatus::BadRequest;
            result.error = "invalid Content-Length";
            return result;
        }
    }

    // 先检查客户端声明的正文长度是否超过配置上限
    if (content_length > max_body_size_) {
        result.status = ParseStatus::BadRequest;
        result.error = "request body is too large";
        return result;
    }

    if (content_length > buffer.size() - body_start) {
        result.status = ParseStatus::Incomplete;
        return result;
    }

    result.request.body = buffer.substr(body_start, content_length);
    result.consumed = body_start + content_length;
    result.status = ParseStatus::Complete;
    return result;
}

} // namespace tiny_http