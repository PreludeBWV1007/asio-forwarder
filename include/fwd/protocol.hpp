#pragma once

// 本文件职责（协议定义）：
// - 定义 wire 格式的 Header（固定 24 字节，小端）
// - 提供 pack_le()/unpack_le()：在“结构体字段”与“线上的字节序列”之间转换
// - 提供 Frame（header+body）数据结构：body 为原始二进制载荷（可含 '\0'）
// 关联文档：`docs/protocol.md`

#include <array>
#include <cstdint>
#include <string>

#include "fwd/endian.hpp"

namespace fwd::proto {

// 固定小端头部：24 bytes
// 字段布局：magic(4) | version(2) | header_len(2) | body_len(4) | msg_type(4) | flags(4) | seq(4)
struct Header {
  static constexpr std::uint32_t kMagic = 0x44574641;  // 'A''F''W''D' little-endian display
  static constexpr std::uint16_t kVersion = 1;
  static constexpr std::uint16_t kHeaderLen = 24;

  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t header_len = kHeaderLen;
  std::uint32_t body_len = 0;
  std::uint32_t msg_type = 0;
  std::uint32_t flags = 0;
  std::uint32_t seq = 0;

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
    return h;
  }

  std::string to_string() const;
};

struct Frame {
  Header header;
  std::string body;  // 二进制载荷（可含 '\0'）
};

}  // 命名空间 fwd::proto

