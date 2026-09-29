#pragma once

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tiny_room {

// 固定头部：4 字节正文长度 + 2 字节消息类型
inline constexpr std::size_t kHeaderSize = 6;
inline constexpr std::size_t kMaxBodySize = 64 * 1024;

enum class MessageType : std::uint16_t {
    Echo = 1,
    EchoReply = 2,

    JoinRoom = 3,
    JoinRoomReply = 4,

    RoomMessage = 5,
    RoomBroadcast = 6,

    Error = 255
};

struct Message {
    MessageType type {MessageType::Echo};
    std::string body;
};

enum class DecodeStatus {
    Complete,
    Incomplete,
    TooLarge
};

// 将消息编码成可以交给 send() 的字节串
inline std::string encode_message(const Message& message)
{
    if (message.body.size() > kMaxBodySize) {
        throw std::length_error("message body too large");
    }

    const std::uint32_t length = ::htonl(
        static_cast<std::uint32_t>(message.body.size()));

    const std::uint16_t type = ::htons(
        static_cast<std::uint16_t>(message.type));

    std::string output(kHeaderSize, '\0');

    // 分别写入字段，不直接发送整个 C++ 结构体
    std::memcpy(output.data(), &length, sizeof(length));
    std::memcpy(output.data() + sizeof(length), &type, sizeof(type));

    output.append(message.body);
    return output;
}

// 从输入缓冲区取出一条完整消息
// 不完整或超长时不修改 input 和 message
inline DecodeStatus try_decode_message(
    std::string& input, Message& message)
{
    // 连固定头部都没收齐
    if (input.size() < kHeaderSize) {
        return DecodeStatus::Incomplete;
    }

    // 先读取正文长度，并转换回主机字节序
    std::uint32_t network_length = 0;
    std::memcpy(
        &network_length,
        input.data(),
        sizeof(network_length));

    const std::size_t body_size = ::ntohl(network_length);

    // 在等待正文或分配正文空间之前检查长度
    if (body_size > kMaxBodySize) {
        return DecodeStatus::TooLarge;
    }

    const std::size_t total_size = kHeaderSize + body_size;

    if (input.size() < total_size) {
        return DecodeStatus::Incomplete;
    }

    // 读取消息类型
    std::uint16_t network_type = 0;
    std::memcpy(
        &network_type,
        input.data() + sizeof(network_length),
        sizeof(network_type));
    
    message.type = static_cast<MessageType>(::ntohs(network_type));

    message.body.assign(input.data() + kHeaderSize, body_size);

    // 只移除当前消息，后续消息继续留在缓冲区中
    input.erase(0, total_size);

    return DecodeStatus::Complete;
}

} // namespace tiny_room