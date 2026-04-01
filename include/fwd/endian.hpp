#pragma once

// 本文件职责（小端读写工具）：
// - 提供 read_u16_le/read_u32_le/read_u64_le：从字节指针读取小端整数
// - 提供 write_u16_le/write_u32_le/write_u64_le：将整数写入字节指针（小端）
// 说明：这里不依赖平台端序假设，避免 UB；用于协议 pack/unpack 的底层工具。

#include <array>
#include <cstdint>
#include <cstring>

namespace fwd::endian {

inline std::uint16_t read_u16_le(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) |
         (static_cast<std::uint16_t>(p[1]) << 8);
}

inline std::uint32_t read_u32_le(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t read_u64_le(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(p[0]) |
         (static_cast<std::uint64_t>(p[1]) << 8) |
         (static_cast<std::uint64_t>(p[2]) << 16) |
         (static_cast<std::uint64_t>(p[3]) << 24) |
         (static_cast<std::uint64_t>(p[4]) << 32) |
         (static_cast<std::uint64_t>(p[5]) << 40) |
         (static_cast<std::uint64_t>(p[6]) << 48) |
         (static_cast<std::uint64_t>(p[7]) << 56);
}

inline void write_u16_le(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xff);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
}

inline void write_u32_le(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xff);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
}

inline void write_u64_le(std::uint8_t* p, std::uint64_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xff);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
  p[4] = static_cast<std::uint8_t>((v >> 32) & 0xff);
  p[5] = static_cast<std::uint8_t>((v >> 40) & 0xff);
  p[6] = static_cast<std::uint8_t>((v >> 48) & 0xff);
  p[7] = static_cast<std::uint8_t>((v >> 56) & 0xff);
}

}  // 命名空间 fwd::endian

