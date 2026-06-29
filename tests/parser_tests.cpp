#include "tiny_http/http_parser.h"
#include "tiny_http/http_response.h"
#include "tiny_http/mime_types.h"

#include <cassert>
#include <iostream>

int main() {
    tiny_http::HttpParser parser;

    const std::string raw = 
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"	
        "\r\n";

    const tiny_http::ParseResult result = parser.parse(raw);
    assert(result.status == tiny_http::ParseStatus::Complete);
    assert(result.request.method == "GET");
    assert(result.request.target == "/index.html");
    assert(result.request.version == "HTTP/1.1");
    assert(result.request.header("host") == "localhost");
    assert(result.request.keep_alive());
    
    // 测试1：不完整请求应返回 Incomplete 状态
    {
        const std::string incomplete_raw = 
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n";
        const tiny_http::ParseResult incomplete = parser.parse(incomplete_raw);
        assert(incomplete.status == tiny_http::ParseStatus::Incomplete);
    }

    // 测试2：相对路径（不以 / 开头）应返回 BadRequest
    {
        const std::string bad_raw = 
            "GET index.html HTTP/1.1\r\n"
            "\r\n";
        const tiny_http::ParseResult bad = parser.parse(bad_raw);
        assert(bad.status == tiny_http::ParseStatus::BadRequest);
    }

    // 测试3：POST 带 Body 应正确解析
    {
        const std::string post_raw = 
            "POST /data HTTP/1.1\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        const tiny_http::ParseResult post = parser.parse(post_raw);
        assert(post.status == tiny_http::ParseStatus::Complete);
        assert(post.request.method == "POST");
        assert(post.request.body == "hello");
    }

    // 测试4：.html 扩展名应返回 text/html
    {
        assert(tiny_http::mime_type_for_path("/index.html") == "text/html; charset=utf-8");
        assert(tiny_http::mime_type_for_path("/path/to/page.HTML") == "text/html; charset=utf-8");
    }
    
    tiny_http::HttpResponse response(200, "OK");
    response.set_body("hello");
    const std::string text = response.serialize();
    assert(text.find("HTTP/1.1 200 OK\r\n") == 0);

    std::cout << "parser tests passed\n";
    return 0;
}

