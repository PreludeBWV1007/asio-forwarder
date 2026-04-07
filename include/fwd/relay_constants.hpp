#pragma once

#include <cstdint>

namespace fwd::relay {

// Header.msg_type：业务载荷由客户端互发；以下为服务器生成帧的类型。
inline constexpr std::uint32_t kMsgDeliver = 200;   // Body 为对端 TRANSFER 的 payload 原样字节
inline constexpr std::uint32_t kMsgServerReply = 201;  // Body 为 msgpack：ACK/错误/CONTROL 回复

}  // namespace fwd::relay
