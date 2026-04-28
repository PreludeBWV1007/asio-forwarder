#pragma once

// 中继业务：Header.msg_type 取值约定（v2 帧头见 include/fwd/protocol.hpp）。
// 字段语义、登录/DATA/CONTROL 的 body 键、200 投递信封格式等见 docs/protocol.md（与 src/main.cpp 一致）。

#include <cstdint>

namespace fwd::relay {

// 客户端 → 服务器：Header.msg_type（body 均为 msgpack map）
inline constexpr std::uint32_t kClientLogin = 1;
inline constexpr std::uint32_t kClientHeartbeat = 2;
inline constexpr std::uint32_t kClientControl = 3;
inline constexpr std::uint32_t kClientData = 4;

// 服务器 → 客户端
inline constexpr std::uint32_t kMsgDeliver = 200;    // Body 为 msgpack：{payload, src_conn_id, dst_conn_id, src_username, dst_username}
inline constexpr std::uint32_t kMsgServerReply = 201;  // Body 为 msgpack：ACK/错误/CONTROL 回复
inline constexpr std::uint32_t kMsgKick = 202;  // Body 为 msgpack：{op:"KICK", reason}

// 201 / 错误 应答中 code（可选；与 message 成对使用）
namespace errc {
inline constexpr int kIpNotAllowed = 1001;
inline constexpr int kAuthFailed = 1002;
inline constexpr int kInvalidLoginBody = 1003;
inline constexpr int kRoleMismatch = 1004;
inline constexpr int kProtocol = 1005;
}  // namespace errc

}  // namespace fwd::relay
