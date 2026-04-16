// C++ RelayClient SDK implementation (sync, blocking).

#include "fwd/relay_client.hpp"

#include <boost/asio.hpp>
#include <msgpack.hpp>

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "fwd/protocol.hpp"
#include "fwd/relay_constants.hpp"

namespace fwd::sdk {

using boost::asio::ip::tcp;

struct RelayClient::Impl {
  boost::asio::io_context io;
  tcp::resolver resolver{io};
  tcp::socket sock{io};
  std::uint32_t seq{1};

  std::uint32_t next_seq() {
    seq = (seq + 1u) & 0x7FFFFFFFu;
    if (seq == 0) seq = 1;
    return seq;
  }

  void write_all(const void* data, std::size_t n) {
    boost::asio::write(sock, boost::asio::buffer(data, n));
  }

  void read_all(void* data, std::size_t n) {
    boost::asio::read(sock, boost::asio::buffer(data, n));
  }

  void send_msgpack(std::uint32_t msg_type, std::uint32_t seq, const msgpack::sbuffer& buf) {
    proto::Header h{};
    h.msg_type = msg_type;
    h.seq = seq;
    h.src_user_id = 0;
    h.dst_user_id = 0;
    h.body_len = static_cast<std::uint32_t>(buf.size());
    auto hb = h.pack_le();
    write_all(hb.data(), hb.size());
    if (buf.size()) write_all(buf.data(), buf.size());
  }
};

RelayClient::RelayClient() : impl_(new Impl) {}
RelayClient::~RelayClient() {
  try {
    close();
  } catch (...) {
  }
  delete impl_;
  impl_ = nullptr;
}

void RelayClient::connect(const std::string& host, std::uint16_t port) {
  auto eps = impl_->resolver.resolve(host, std::to_string(port));
  boost::asio::connect(impl_->sock, eps);
}

void RelayClient::close() {
  if (!impl_->sock.is_open()) return;
  boost::system::error_code ec;
  impl_->sock.shutdown(tcp::socket::shutdown_both, ec);
  impl_->sock.close(ec);
}

std::uint32_t RelayClient::login(const std::string& username, const std::string& password, const std::string& peer_role,
                                 bool register_user) {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(4);
  pk.pack("username");
  pk.pack(username);
  pk.pack("password");
  pk.pack(password);
  pk.pack("peer_role");
  pk.pack(peer_role);
  pk.pack("register");
  pk.pack(register_user);
  impl_->send_msgpack(relay::kClientLogin, s, buf);
  return s;
}

std::uint32_t RelayClient::heartbeat() {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(0);
  impl_->send_msgpack(relay::kClientHeartbeat, s, buf);
  return s;
}

std::uint32_t RelayClient::send_unicast(const std::string& dst_username, const std::string& payload, std::uint64_t dst_conn_id) {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(4);
  pk.pack("mode");
  pk.pack("unicast");
  pk.pack("dst_username");
  pk.pack(dst_username);
  pk.pack("dst_conn_id");
  pk.pack(dst_conn_id);
  pk.pack("payload");
  pk.pack_bin(static_cast<std::uint32_t>(payload.size()));
  pk.pack_bin_body(payload.data(), static_cast<std::uint32_t>(payload.size()));
  impl_->send_msgpack(relay::kClientData, s, buf);
  return s;
}

std::uint32_t RelayClient::send_broadcast(const std::string& dst_username, const std::string& payload) {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(3);
  pk.pack("mode");
  pk.pack("broadcast");
  pk.pack("dst_username");
  pk.pack(dst_username);
  pk.pack("payload");
  pk.pack_bin(static_cast<std::uint32_t>(payload.size()));
  pk.pack_bin_body(payload.data(), static_cast<std::uint32_t>(payload.size()));
  impl_->send_msgpack(relay::kClientData, s, buf);
  return s;
}

std::uint32_t RelayClient::send_round_robin(const std::string& dst_username, const std::string& payload, std::uint64_t interval_ms) {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(4);
  pk.pack("mode");
  pk.pack("round_robin");
  pk.pack("dst_username");
  pk.pack(dst_username);
  pk.pack("interval_ms");
  pk.pack(interval_ms);
  pk.pack("payload");
  pk.pack_bin(static_cast<std::uint32_t>(payload.size()));
  pk.pack_bin_body(payload.data(), static_cast<std::uint32_t>(payload.size()));
  impl_->send_msgpack(relay::kClientData, s, buf);
  return s;
}

std::uint32_t RelayClient::control_list_users() {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(1);
  pk.pack("action");
  pk.pack("list_users");
  impl_->send_msgpack(relay::kClientControl, s, buf);
  return s;
}

std::uint32_t RelayClient::control_kick_user(std::uint64_t target_user_id) {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(2);
  pk.pack("action");
  pk.pack("kick_user");
  pk.pack("target_user_id");
  pk.pack(target_user_id);
  impl_->send_msgpack(relay::kClientControl, s, buf);
  return s;
}

static std::string msgpack_to_debug_string(const msgpack::object& o) {
  std::stringstream ss;
  ss << o;
  return ss.str();
}

std::optional<Event> RelayClient::recv() {
  if (!impl_->sock.is_open()) return std::nullopt;
  try {
    std::array<std::uint8_t, proto::Header::kHeaderLen> hb{};
    impl_->read_all(hb.data(), hb.size());
    const auto h = proto::Header::unpack_le(hb.data());
    std::string body;
    body.resize(h.body_len);
    if (h.body_len) impl_->read_all(body.data(), body.size());

    if (h.msg_type == relay::kMsgDeliver) {
      Deliver d;
      d.seq = h.seq;
      auto oh = msgpack::unpack(body.data(), body.size());
      msgpack::object obj = oh.get();
      if (obj.type != msgpack::type::MAP) return Event{d};
      const auto& m = obj.via.map;
      for (std::uint32_t i = 0; i < m.size; ++i) {
        if (m.ptr[i].key.type != msgpack::type::STR) continue;
        std::string k;
        m.ptr[i].key.convert(k);
        if (k == "src_conn_id") m.ptr[i].val.convert(d.src_conn_id);
        else if (k == "dst_conn_id") m.ptr[i].val.convert(d.dst_conn_id);
        else if (k == "src_username") m.ptr[i].val.convert(d.src_username);
        else if (k == "dst_username") m.ptr[i].val.convert(d.dst_username);
        else if (k == "payload") {
          if (m.ptr[i].val.type == msgpack::type::BIN) {
            d.payload.assign(reinterpret_cast<const char*>(m.ptr[i].val.via.bin.ptr), m.ptr[i].val.via.bin.size);
          } else if (m.ptr[i].val.type == msgpack::type::STR) {
            d.payload.assign(m.ptr[i].val.via.str.ptr, m.ptr[i].val.via.str.size);
          }
        }
      }
      return Event{std::move(d)};
    }

    if (h.msg_type == relay::kMsgKick) {
      Kick k;
      auto oh = msgpack::unpack(body.data(), body.size());
      msgpack::object obj = oh.get();
      if (obj.type == msgpack::type::MAP) {
        const auto& m = obj.via.map;
        for (std::uint32_t i = 0; i < m.size; ++i) {
          if (m.ptr[i].key.type != msgpack::type::STR) continue;
          std::string kk;
          m.ptr[i].key.convert(kk);
          if (kk == "reason") m.ptr[i].val.convert(k.reason);
        }
      }
      return Event{std::move(k)};
    }

    if (h.msg_type == relay::kMsgServerReply) {
      ServerReply r;
      r.seq = h.seq;
      auto oh = msgpack::unpack(body.data(), body.size());
      msgpack::object obj = oh.get();
      r.raw = msgpack_to_debug_string(obj);
      if (obj.type == msgpack::type::MAP) {
        const auto& m = obj.via.map;
        for (std::uint32_t i = 0; i < m.size; ++i) {
          if (m.ptr[i].key.type != msgpack::type::STR) continue;
          std::string k;
          m.ptr[i].key.convert(k);
          if (k == "ok") {
            if (m.ptr[i].val.type == msgpack::type::BOOLEAN) r.ok = m.ptr[i].val.via.boolean;
          } else if (k == "op") {
            if (m.ptr[i].val.type == msgpack::type::STR) m.ptr[i].val.convert(r.op);
          } else if (k == "error") {
            if (m.ptr[i].val.type == msgpack::type::STR) m.ptr[i].val.convert(r.error);
          }
        }
      }
      return Event{std::move(r)};
    }

    // ignore unknown frames
    return std::nullopt;
  } catch (...) {
    close();
    return std::nullopt;
  }
}

}  // namespace fwd::sdk

