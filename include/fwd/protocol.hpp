#pragma once

// 本文件职责（协议定义）：
// - 定义 wire 格式的 Header（固定 40 字节，小端，version=2）
// - 提供 pack_le()/unpack_le()：在“结构体字段”与“线上的字节序列”之间转换
// - 提供 Frame（header+body）数据结构：body 为原始二进制载荷（可含 '\0'）
// 关联文档：`docs/protocol.md`

#include <array>
#include <cstdint>
#include <string>

#include "fwd/endian.hpp"

namespace fwd::proto {

// flags 位定义（与 `docs/protocol.md` 一致；未置位则按默认语义解释 dst_user_id）
inline namespace header_flags {
constexpr std::uint32_t kBroadcast = 1u << 0;      // 广播（目标集合由业务/后续路由层解释）
constexpr std::uint32_t kDstIsGroup = 1u << 1;    // dst_user_id 表示群组/主题 id，非单播用户
constexpr std::uint32_t kNeedAck = 1u << 2;       // 期望对端回执（业务层，转发器可忽略）
}  // namespace header_flags

// 固定小端头部：40 bytes（version 2）
// 字段布局：magic(4) | version(2) | header_len(2) | body_len(4) | msg_type(4) | flags(4) | seq(4)
//          | src_user_id(8) | dst_user_id(8)
struct Header {
  static constexpr std::uint32_t kMagic = 0x44574641;  // 'A''F''W''D' little-endian display
  static constexpr std::uint16_t kVersion = 2;
  static constexpr std::uint16_t kHeaderLen = 40;

  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t header_len = kHeaderLen;
  std::uint32_t body_len = 0;
  std::uint32_t msg_type = 0;
  std::uint32_t flags = 0;
  std::uint32_t seq = 0;
  std::uint64_t src_user_id = 0;
  std::uint64_t dst_user_id = 0;

  static constexpr std::size_t wire_size() { return kHeaderLen; }

  std::array<std::uint8_t, kHeaderLen> pack_le() const {
    std::array<std::uint8_t, kHeaderLen> b{};
    endian::write_u32_le(&b[0], magic);
    endian::write_u16_le(&b[4], version);
    endian::write_u16_le(&b[6], header_len);
    endian::write_u32_le(&b[8], body_len);
    endian::write_u32_le(&b[12], msg_type);
    endian::write_u32_le(&b[16], flags);
    endian::write_u32_le(&b[20], seq);
    endian::write_u64_le(&b[24], src_user_id);
    endian::write_u64_le(&b[32], dst_user_id);
    return b;
  }

  static Header unpack_le(const std::uint8_t* p) {
    Header h;
    h.magic = endian::read_u32_le(p + 0);
    h.version = endian::read_u16_le(p + 4);
    h.header_len = endian::read_u16_le(p + 6);
    h.body_len = endian::read_u32_le(p + 8);
    h.msg_type = endian::read_u32_le(p + 12);
    h.flags = endian::read_u32_le(p + 16);
    h.seq = endian::read_u32_le(p + 20);
    h.src_user_id = endian::read_u64_le(p + 24);
    h.dst_user_id = endian::read_u64_le(p + 32);
    return h;
  }

  std::string to_string() const;
};

struct Frame {
  Header header;
  std::string body;  // 二进制载荷（可含 '\0'）
};

}  // 命名空间 fwd::proto

