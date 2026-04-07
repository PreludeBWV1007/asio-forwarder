#pragma once

// 组帧 / 读帧小工具：供 proto_send、proto_recv 等复用（POSIX TCP，无 Boost 依赖）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "fwd/protocol.hpp"

namespace fwd::frame_io {

inline bool send_all(int fd, const void* data, std::size_t len) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::size_t off = 0;
  while (off < len) {
    const auto n = ::send(fd, p + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

inline bool recv_exact(int fd, void* buf, std::size_t len) {
  auto* p = static_cast<std::uint8_t*>(buf);
  std::size_t off = 0;
  while (off < len) {
    const auto n = ::recv(fd, p + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

struct ParsedFrame {
  proto::Header header{};
  std::string body;
};

// 将 Header 与 Body 拼成线上帧（body_len 由 body.size() 覆盖写入 header）。
inline std::string pack_frame(proto::Header h, const std::string& body) {
  h.body_len = static_cast<std::uint32_t>(body.size());
  const auto packed = h.pack_le();
  std::string wire;
  wire.reserve(proto::Header::kHeaderLen + body.size());
  wire.append(reinterpret_cast<const char*>(packed.data()), packed.size());
  wire.append(body);
  return wire;
}

// 从 fd 读一帧：校验 magic/version/header_len（40B v2），且 body_len <= max_body_len。
inline std::optional<ParsedFrame> recv_frame(int fd, std::uint32_t max_body_len) {
  std::array<std::uint8_t, proto::Header::kHeaderLen> hb{};
  if (!recv_exact(fd, hb.data(), hb.size())) return std::nullopt;

  ParsedFrame out;
  out.header = proto::Header::unpack_le(hb.data());
  if (out.header.magic != proto::Header::kMagic || out.header.version != proto::Header::kVersion ||
      out.header.header_len != proto::Header::kHeaderLen) {
    return std::nullopt;
  }
  if (out.header.body_len > max_body_len) return std::nullopt;

  out.body.resize(out.header.body_len);
  if (out.header.body_len > 0 && !recv_exact(fd, out.body.data(), out.header.body_len)) return std::nullopt;
  return out;
}

// 连接 IPv4 TCP，返回 fd（失败 -1）。
inline int tcp_connect(const char* host, std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

inline void set_recv_timeout(int fd, int timeout_sec) {
  if (timeout_sec <= 0) return;
  timeval tv{};
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

}  // namespace fwd::frame_io
