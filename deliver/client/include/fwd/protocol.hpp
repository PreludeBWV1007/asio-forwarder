#pragma once

// 线协议：固定 Header + Body（v3：24B 小端，version=3；已移除 v2 的 flags 与 user_id 字段）
// 见 `docs/protocol.md`

#include <array>
#include <cstdint>
#include <string>

#include "fwd/endian.hpp"

namespace fwd::proto {

// 固定小端头部：24 bytes（version 3）
// magic(4) | version(2) | header_len(2) | body_len(4) | msg_type(4) | seq(4) | reserved(4, 必为 0)
struct Header {
  static constexpr std::uint32_t kMagic = 0x44574641;  // 'A''F''W''D' little-endian
  static constexpr std::uint16_t kVersion = 3;
  static constexpr std::uint16_t kHeaderLen = 24;

  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t header_len = kHeaderLen;
  std::uint32_t body_len = 0;
  std::uint32_t msg_type = 0;
  std::uint32_t seq = 0;
  std::uint32_t reserved0 = 0;

  static constexpr std::size_t wire_size() { return kHeaderLen; }

  std::array<std::uint8_t, kHeaderLen> pack_le() const {
    std::array<std::uint8_t, kHeaderLen> b{};
    endian::write_u32_le(&b[0], magic);
    endian::write_u16_le(&b[4], version);
    endian::write_u16_le(&b[6], header_len);
    endian::write_u32_le(&b[8], body_len);
    endian::write_u32_le(&b[12], msg_type);
    endian::write_u32_le(&b[16], seq);
    endian::write_u32_le(&b[20], reserved0);
    return b;
  }

  static Header unpack_le(const std::uint8_t* p) {
    Header h;
    h.magic = endian::read_u32_le(p + 0);
    h.version = endian::read_u16_le(p + 4);
    h.header_len = endian::read_u16_le(p + 6);
    h.body_len = endian::read_u32_le(p + 8);
    h.msg_type = endian::read_u32_le(p + 12);
    h.seq = endian::read_u32_le(p + 16);
    h.reserved0 = endian::read_u32_le(p + 20);
    return h;
  }

  std::string to_string() const;
};

struct Frame {
  Header header;
  std::string body;  // 对 DATA 为 msgpack 或其它；透明字节流
};

}  // namespace fwd::proto
