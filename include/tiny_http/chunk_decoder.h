#pragma once

#include "tiny_http/http_request.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace tiny_http {

class ChunkDecoder {
public:
    static constexpr std::size_t metadata_limit = 64 * 1024;

    std::size_t used {0}; // 已消费的编码字节数，包含分块格式
    std::string body;     // 解码后的正文

    // wire 从本条消息的第一个块开始，每次传入累计收到的数据
    ParseStatus read(std::string_view wire, std::size_t limit)
    {
        for (;;) {
            if (stage_ == Stage::Done) {
                return ParseStatus::Complete;
            }

            // 等当前块的数据收齐，再追加到正文
            if (stage_ == Stage::Data) {
                if (wire.size() - used < left_) {
                    return ParseStatus::Incomplete;
                }

                body.append(wire.data() + used, left_);
                used += left_;
                stage_ = Stage::DataEnd;
            }

            // 每个非空块的数据后面都有一个 CRLF
            if (stage_ == Stage::DataEnd) {
                if (wire.size() - used < 2) {
                    return ParseStatus::Incomplete;
                }

                if (wire.substr(used, 2) != "\r\n" ||
                    metadata_ > metadata_limit - 2) {
                    return ParseStatus::BadRequest;
                }

                metadata_ += 2;
                used += 2;
                scan_ = used;
                stage_ = Stage::Size;
            }

            // 块大小和尾部字段都是按行读取
            const auto end = wire.find("\r\n", scan_);

            if (end == std::string_view::npos) {
                const auto pending = wire.size() - used;

                // 一行最多 8192 字节，允许末尾已经收到一个 '\r'
                if (pending > 8193 || pending > metadata_limit - metadata_) {
                    return ParseStatus::BadRequest;
                }

                scan_ = pending ? wire.size() - 1 : used;
                return ParseStatus::Incomplete;
            }

            const auto line = wire.substr(used, end - used);

            if (line.size() > 8192 ||
                line.size() + 2 > metadata_limit - metadata_) {
                return ParseStatus::BadRequest;
            }

            metadata_ += line.size() + 2;
            used = end + 2;
            scan_ = used;

            if (stage_ == Stage::Trailers) {
                if (line.empty()) {
                    stage_ = Stage::Done;
                    return ParseStatus::Complete;
                }

                if (!valid_trailer(line)) {
                    return ParseStatus::BadRequest;
                }

                continue;
            }

            if (!size_line(line, left_) || left_ > limit - body.size()) {
                return ParseStatus::BadRequest;
            }

            stage_ = left_ == 0 ? Stage::Trailers : Stage::Data;
        }
    }

private:
    enum class Stage { Size, Data, DataEnd, Trailers, Done };

    Stage stage_ {Stage::Size};
    std::size_t left_ {0};     // 当前块声明的正文长度
    std::size_t scan_ {0};     // 下一次从哪里继续找 CRLF
    std::size_t metadata_ {0}; // 块大小行、分隔符和尾部字段占用的字节

    static bool token_char(unsigned char c)
    {
        constexpr std::string_view extra = "!#$%&'*+-.^_`|~";

        return (c >= '0' && c <= '9') ||
               (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') ||
               extra.find(c) != extra.npos;
    }

    static bool value_char(unsigned char c)
    {
        return c == '\t' || (c >= 32 && c != 127);
    }

    static bool valid_trailer(std::string_view line)
    {
        const auto colon = line.find(':');

        if (colon == 0 || colon == line.npos) {
            return false;
        }

        const auto name = line.substr(0, colon);
        const auto value = line.substr(colon + 1);

        return std::all_of(name.begin(), name.end(), token_char) &&
               std::all_of(value.begin(), value.end(), value_char);
    }

    // 读取十六进制块大小，识别并跳过可选扩展
    static bool size_line(std::string_view s, std::size_t& size)
    {
        if (s.empty()) {
            return false;
        }

        const auto [p, ec] = std::from_chars(
            s.data(), s.data() + s.size(), size, 16
        );

        if (ec != std::errc{}) {
            return false;
        }

        s.remove_prefix(p - s.data());

        auto spaces = [&] {
            while (s.starts_with(' ') || s.starts_with('\t')) {
                s.remove_prefix(1);
            }
        };

        auto take = [&](char c) {
            if (!s.starts_with(c)) {
                return false;
            }
            s.remove_prefix(1);
            return true;
        };

        auto token = [&] {
            const auto n = s.size();
            while (!s.empty() && token_char(s.front())) {
                s.remove_prefix(1);
            }
            return s.size() != n;
        };

        while (!s.empty()) {
            spaces();
            if (!take(';')) return false;
            spaces();
            if (!token()) return false;
            spaces();
            if (!take('=')) continue;
            spaces();

            if (!take('"')) {
                if (!token()) return false;
                continue;
            }

            while (!s.empty() && s.front() != '"') {
                if (take('\\') && s.empty()) return false;
                if (!value_char(s.front())) return false;
                s.remove_prefix(1);
            }

            if (!take('"')) return false;
        }

        return true;
    }
};

} // namespace tiny_http
