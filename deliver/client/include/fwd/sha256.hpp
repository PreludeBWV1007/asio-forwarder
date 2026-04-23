#pragma once

// 轻量 SHA-256（纯头文件，用于本地中继口令摘要；非 FIPS 场景）

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace fwd::sha256 {

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline void compress_block(std::uint32_t state[8], const std::uint8_t block[64]) {
  static constexpr std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | (static_cast<std::uint32_t>(block[i * 4 + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

inline std::array<std::uint8_t, 32> hash_bytes(const void* data, std::size_t len) {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint64_t bitlen = static_cast<std::uint64_t>(len) * 8ULL;

  std::uint8_t buf[64];
  std::size_t offset = 0;
  while (len - offset >= 64) {
    compress_block(h, p + offset);
    offset += 64;
  }
  std::size_t rem = len - offset;
  std::memcpy(buf, p + offset, rem);
  buf[rem++] = 0x80;
  if (rem <= 56) {
    std::memset(buf + rem, 0, 56 - rem);
  } else {
    std::memset(buf + rem, 0, 64 - rem);
    compress_block(h, buf);
    std::memset(buf, 0, 56);
  }
  for (int i = 0; i < 8; ++i) {
    buf[56 + i] = static_cast<std::uint8_t>((bitlen >> (56 - i * 8)) & 0xFF);
  }
  compress_block(h, buf);

  std::array<std::uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4 + 0] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
  }
  return out;
}

inline std::string to_hex(const std::array<std::uint8_t, 32>& d) {
  static const char* hx = "0123456789abcdef";
  std::string s;
  s.resize(64);
  for (std::size_t i = 0; i < 32; ++i) {
    s[i * 2] = hx[(d[i] >> 4) & 0xF];
    s[i * 2 + 1] = hx[d[i] & 0xF];
  }
  return s;
}

inline std::string hash_hex(const void* data, std::size_t len) { return to_hex(hash_bytes(data, len)); }

}  // namespace fwd::sha256
