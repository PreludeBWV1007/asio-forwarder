// 自测工具：连接 downstream，读若干帧；按 Header.msg_type 分发解析 Protobuf 并打印（外层 Header v2 40B）。

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#include "fwd/frame_io.hpp"
#include "fwd/protocol.hpp"
#include "market.pb.h"

namespace {

constexpr std::uint32_t kMaxBody = 64u * 1024u * 1024u;

void usage() {
  std::cerr << "usage: proto_recv [options]\n"
            << "  --host ADDR     default 127.0.0.1\n"
            << "  --port N        default 19002 (downstream)\n"
            << "  --timeout SEC   recv timeout, default 30；0 表示无限\n"
            << "  --count N       收满 N 帧后退出，默认 1；0 表示一直收直到对端关闭\n"
            << "  --hex-on-fail   无法按已知类型解析时打印 body 十六进制预览\n";
}

void print_hex_preview(const std::string& body, std::size_t max_bytes) {
  std::cerr << "body hex (first " << max_bytes << " bytes):\n";
  const std::size_t n = std::min(max_bytes, body.size());
  for (std::size_t i = 0; i < n; ++i) {
    std::cerr << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned>(static_cast<unsigned char>(body[i])) << std::dec;
    if ((i + 1) % 32 == 0) std::cerr << "\n";
    else std::cerr << ' ';
  }
  if (n % 32 != 0) std::cerr << "\n";
}

bool dispatch(const fwd::proto::Header& h, const std::string& body, bool hex_on_fail) {
  std::cout << "frame msg_type=" << h.msg_type << " flags=" << h.flags << " seq=" << h.seq
            << " src_user_id=" << h.src_user_id << " dst_user_id=" << h.dst_user_id
            << " body_len=" << body.size() << "\n";

  if (h.msg_type == static_cast<std::uint32_t>(fwd::market::STOCK_QUOTE)) {
    fwd::market::StockQuote q;
    if (!q.ParseFromString(body)) {
      std::cerr << "Parse StockQuote failed\n";
      if (hex_on_fail) print_hex_preview(body, 128);
      return false;
    }
    std::cout << "  StockQuote: symbol=" << q.symbol() << " last_price=" << q.last_price()
              << " volume=" << q.volume() << " ts_ms=" << q.ts_ms() << " exchange=" << q.exchange()
              << "\n";
    return true;
  }

  if (h.msg_type == static_cast<std::uint32_t>(fwd::market::HEARTBEAT)) {
    fwd::market::Heartbeat hb;
    if (!hb.ParseFromString(body)) {
      std::cerr << "Parse Heartbeat failed\n";
      if (hex_on_fail) print_hex_preview(body, 128);
      return false;
    }
    std::cout << "  Heartbeat: ts_ms=" << hb.ts_ms() << " node_id=" << hb.node_id() << "\n";
    return true;
  }

  if (h.msg_type == static_cast<std::uint32_t>(fwd::market::MARKET_PAYLOAD)) {
    fwd::market::MarketPayload mp;
    if (!mp.ParseFromString(body)) {
      std::cerr << "Parse MarketPayload failed\n";
      if (hex_on_fail) print_hex_preview(body, 128);
      return false;
    }
    switch (mp.payload_case()) {
      case fwd::market::MarketPayload::kStockQuote:
        std::cout << "  MarketPayload.stock_quote: symbol=" << mp.stock_quote().symbol()
                  << " last_price=" << mp.stock_quote().last_price() << "\n";
        break;
      case fwd::market::MarketPayload::kHeartbeat:
        std::cout << "  MarketPayload.heartbeat: node_id=" << mp.heartbeat().node_id() << "\n";
        break;
      default:
        std::cout << "  MarketPayload: (empty oneof)\n";
        break;
    }
    return true;
  }

  // 容错：msg_type 未标对但 body 能认成单一消息时仍尝试打印
  {
    fwd::market::StockQuote q;
    if (q.ParseFromString(body)) {
      std::cout << "  (hint) parsed as StockQuote despite msg_type=" << h.msg_type << "\n";
      std::cout << "  StockQuote: symbol=" << q.symbol() << " last_price=" << q.last_price() << "\n";
      return true;
    }
  }
  {
    fwd::market::Heartbeat hb;
    if (hb.ParseFromString(body)) {
      std::cout << "  (hint) parsed as Heartbeat despite msg_type=" << h.msg_type << "\n";
      return true;
    }
  }
  {
    fwd::market::MarketPayload mp;
    if (mp.ParseFromString(body)) {
      std::cout << "  (hint) parsed as MarketPayload despite msg_type=" << h.msg_type << "\n";
      switch (mp.payload_case()) {
        case fwd::market::MarketPayload::kStockQuote:
          std::cout << "    stock_quote.symbol=" << mp.stock_quote().symbol() << "\n";
          break;
        case fwd::market::MarketPayload::kHeartbeat:
          std::cout << "    heartbeat.node_id=" << mp.heartbeat().node_id() << "\n";
          break;
        default:
          break;
      }
      return true;
    }
  }

  std::cerr << "unknown msg_type and no heuristic parse; body_len=" << body.size() << "\n";
  if (hex_on_fail) print_hex_preview(body, 128);
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 19002;
  int timeout_sec = 30;
  int count = 1;
  bool hex_on_fail = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    if (a == "--host" && i + 1 < argc)
      host = argv[++i];
    else if (a == "--port" && i + 1 < argc)
      port = std::stoi(argv[++i]);
    else if (a == "--timeout" && i + 1 < argc)
      timeout_sec = std::stoi(argv[++i]);
    else if (a == "--count" && i + 1 < argc)
      count = std::stoi(argv[++i]);
    else if (a == "--hex-on-fail")
      hex_on_fail = true;
    else {
      usage();
      return 2;
    }
  }

  const int fd = fwd::frame_io::tcp_connect(host.c_str(), static_cast<std::uint16_t>(port));
  if (fd < 0) {
    std::perror("connect");
    return 1;
  }
  fwd::frame_io::set_recv_timeout(fd, timeout_sec);

  int received = 0;
  while (true) {
    if (count > 0 && received >= count) break;
    auto fr = fwd::frame_io::recv_frame(fd, kMaxBody);
    if (!fr) {
      if (received == 0) std::cerr << "recv_frame failed or EOF\n";
      break;
    }
    dispatch(fr->header, fr->body, hex_on_fail);
    ++received;
  }

  ::close(fd);
  return 0;
}
