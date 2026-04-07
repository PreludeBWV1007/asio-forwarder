// 自测工具：按 --kind 构造 Protobuf Body，打包外层 Header(40, v2) + Body，发往 upstream。

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "fwd/frame_io.hpp"
#include "fwd/protocol.hpp"
#include "market.pb.h"

namespace {

void usage() {
  std::cerr
      << "usage: proto_send [options]\n"
      << "  --host ADDR          default 127.0.0.1\n"
      << "  --port N             default 19001 (upstream)\n"
      << "  --kind stock|heartbeat|envelope   default stock\n"
      << "  --type U32           override Header.msg_type (default 按 kind 取 WireMsgType)\n"
      << "  --flags U32          default 0\n"
      << "  --seq U32            default 1\n"
      << "  --src-user U64       Header.src_user_id，default 0\n"
      << "  --dst-user U64       Header.dst_user_id，default 0\n"
      << "  stock fields: --symbol --price --volume --ts_ms --exchange\n"
      << "  heartbeat: --node-id --ts_ms\n"
      << "  envelope: --inner quote|heartbeat  (+ 对应字段同上)\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 19001;
  std::string kind = "stock";
  bool type_override = false;
  std::uint32_t msg_type = 0;
  std::uint32_t flags = 0;
  std::uint32_t seq = 1;
  std::uint64_t src_user_id = 0;
  std::uint64_t dst_user_id = 0;
  std::string inner = "quote";

  fwd::market::StockQuote q;
  q.set_symbol("600000.SH");
  q.set_last_price(10.5);
  q.set_volume(1000000);
  q.set_ts_ms(0);
  q.set_exchange("SSE");

  fwd::market::Heartbeat hb;
  hb.set_ts_ms(0);
  hb.set_node_id("proto_send");

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (a != name || i + 1 >= argc) {
        std::cerr << "missing value after " << name << "\n";
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    if (a == "--host")
      host = need("--host");
    else if (a == "--port")
      port = std::stoi(need("--port"));
    else if (a == "--kind")
      kind = need("--kind");
    else if (a == "--type") {
      type_override = true;
      msg_type = static_cast<std::uint32_t>(std::stoul(need("--type")));
    } else if (a == "--flags")
      flags = static_cast<std::uint32_t>(std::stoul(need("--flags")));
    else if (a == "--seq")
      seq = static_cast<std::uint32_t>(std::stoul(need("--seq")));
    else if (a == "--src-user")
      src_user_id = std::stoull(need("--src-user"));
    else if (a == "--dst-user")
      dst_user_id = std::stoull(need("--dst-user"));
    else if (a == "--symbol")
      q.set_symbol(need("--symbol"));
    else if (a == "--price")
      q.set_last_price(std::stod(need("--price")));
    else if (a == "--volume")
      q.set_volume(std::stoll(need("--volume")));
    else if (a == "--ts_ms") {
      const auto v = std::stoll(need("--ts_ms"));
      q.set_ts_ms(v);
      hb.set_ts_ms(v);
    } else if (a == "--exchange")
      q.set_exchange(need("--exchange"));
    else if (a == "--node-id")
      hb.set_node_id(need("--node-id"));
    else if (a == "--inner")
      inner = need("--inner");
    else {
      std::cerr << "unknown arg: " << a << "\n";
      usage();
      return 2;
    }
  }

  const auto now_ms = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  if (q.ts_ms() == 0) q.set_ts_ms(now_ms());
  if (hb.ts_ms() == 0) hb.set_ts_ms(now_ms());

  std::string body;
  if (kind == "stock") {
    if (!type_override) msg_type = static_cast<std::uint32_t>(fwd::market::STOCK_QUOTE);
    if (!q.SerializeToString(&body)) {
      std::cerr << "SerializeToString StockQuote failed\n";
      return 1;
    }
  } else if (kind == "heartbeat") {
    if (!type_override) msg_type = static_cast<std::uint32_t>(fwd::market::HEARTBEAT);
    if (!hb.SerializeToString(&body)) {
      std::cerr << "SerializeToString Heartbeat failed\n";
      return 1;
    }
  } else if (kind == "envelope") {
    if (!type_override) msg_type = static_cast<std::uint32_t>(fwd::market::MARKET_PAYLOAD);
    fwd::market::MarketPayload mp;
    if (inner == "quote" || inner == "stock") {
      *mp.mutable_stock_quote() = q;
    } else if (inner == "heartbeat" || inner == "hb") {
      *mp.mutable_heartbeat() = hb;
    } else {
      std::cerr << "envelope --inner must be quote|heartbeat\n";
      return 2;
    }
    if (!mp.SerializeToString(&body)) {
      std::cerr << "SerializeToString MarketPayload failed\n";
      return 1;
    }
  } else {
    std::cerr << "unknown --kind (use stock|heartbeat|envelope)\n";
    return 2;
  }

  fwd::proto::Header h{};
  h.msg_type = msg_type;
  h.flags = flags;
  h.seq = seq;
  h.src_user_id = src_user_id;
  h.dst_user_id = dst_user_id;

  const std::string wire = fwd::frame_io::pack_frame(h, body);
  const int fd = fwd::frame_io::tcp_connect(host.c_str(), static_cast<std::uint16_t>(port));
  if (fd < 0) {
    std::perror("connect");
    return 1;
  }
  if (!fwd::frame_io::send_all(fd, wire.data(), wire.size())) {
    std::perror("send");
    ::close(fd);
    return 1;
  }
  ::close(fd);

  std::cout << "sent kind=" << kind << " to " << host << ":" << port << " body_len=" << body.size()
            << " msg_type=" << msg_type << " seq=" << seq << "\n";
  return 0;
}
