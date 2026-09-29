#include "tiny_room/protocol.h"

#include <cstdlib>
#include <iostream>

static void check(bool condition, const char* name)
{
    if (!condition) {
        std::cerr << "测试失败：" << name << '\n';
        std::exit(1);
    }
}

int main() {
    const std::string first = 
        tiny_room::encode_message({tiny_room::MessageType::Echo, "hello"});
    
    const std::string second = 
        tiny_room::encode_message({tiny_room::MessageType::Echo, "world"});

    tiny_room::Message message;

    // 1. 模拟消息分批到达：先收到 3 字节，头部不完整
    std::string input = first.substr(0, 3);

    check(
        tiny_room::try_decode_message(input, message) == tiny_room::DecodeStatus::Incomplete,
        "头部不完整");

    check(input.size() == 3, "不完整消息保留原数据");

    // 再收到 4 字节：头部完整，但正文只有 1 字节
    input += first.substr(3, 4);

    check(
        tiny_room::try_decode_message(input, message) == tiny_room::DecodeStatus::Incomplete,
        "正文不完整");

    // 2. 剩余正文和第二条消息一起到达
    input += first.substr(7);
    input += second;

    check(
        tiny_room::try_decode_message(input, message) == tiny_room::DecodeStatus::Complete &&
        message.type == tiny_room::MessageType::Echo &&
        message.body == "hello",
        "解析第一条消息");
       
    check(input == second, "保留第二条消息");

    check(
        tiny_room::try_decode_message(input, message) == tiny_room::DecodeStatus::Complete &&
        message.body == "world" &&
        input.empty(),
        "解析第二条消息");
    
    // 3. 模拟客户端声明一个超过上限的正文长度
    input.assign(tiny_room::kHeaderSize, '\0');

    const std::uint32_t bad_length = ::htonl(
        static_cast<std::uint32_t>(tiny_room::kMaxBodySize + 1));
    
    std::memcpy(input.data(), &bad_length, sizeof(bad_length));

    check(
        tiny_room::try_decode_message(input, message) == tiny_room::DecodeStatus::TooLarge,
        "拒绝超长消息");
    
    std::cout << "room protocol tests passed\n";
}