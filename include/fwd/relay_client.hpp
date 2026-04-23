#pragma once

// C++ 客户端 SDK（把中继当黑盒）。
// - v2 fixed header (40B little-endian) + msgpack body
// - 上行 msg_type: login(1) / heartbeat(2) / control(3) / data(4)
// - 下行: deliver(200) / server_reply(201) / kick(202)
//
// 约定：当前实现下行 Header.src_user_id/dst_user_id 均为 0，身份信息在 DELIVER body 信封中。
//
// 目标：给业务方提供一个简单 C++ API：connect/login/send/control/recv。
// 对外主用高层 API 见 fwd/asio_forwarder_client.hpp（本类为其实现依赖）。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

#include <msgpack.hpp>

namespace fwd::sdk {

struct TypedPayload {
  struct Holder {
    msgpack::object_handle oh;
    msgpack::object data;  // points into oh.zone()
    explicit Holder(msgpack::object_handle&& h, msgpack::object d) : oh(std::move(h)), data(d) {}
  };

  std::string type;
  std::shared_ptr<Holder> holder;

  bool valid() const { return static_cast<bool>(holder); }

  template <class T>
  T as() const;
};

struct Deliver {
  std::uint32_t seq{0};
  std::string payload;  // raw bytes (payload envelope bytes)
  std::uint64_t src_conn_id{0};
  std::uint64_t dst_conn_id{0};
  std::string src_username;
  std::string dst_username;
  // Optional business payload decoded from payload bytes:
  // If payload bytes follow {type,data} msgpack envelope, this will be set.
  std::optional<TypedPayload> typed;
};

struct ServerReply {
  std::uint32_t seq{0};
  bool ok{false};
  std::string op;     // LOGIN / HEARTBEAT / DATA / CONTROL (best-effort)
  std::string error;  // when ok=false
  std::string raw;    // debug string (best-effort)
};

struct Kick {
  std::string reason;
};

using Event = std::variant<Deliver, ServerReply, Kick>;

class RelayClient {
 public:
  RelayClient();
  ~RelayClient();

  RelayClient(const RelayClient&) = delete;
  RelayClient& operator=(const RelayClient&) = delete;

  // Connect to relay TCP endpoint.
  void connect(const std::string& host, std::uint16_t port);
  void close();

  // login/register; peer_role: "user" | "admin"
  std::uint32_t login(const std::string& username, const std::string& password, const std::string& peer_role, bool register_user);
  std::uint32_t heartbeat();

  // DATA: mode unicast/broadcast/round_robin by dst_username
  std::uint32_t send_unicast(const std::string& dst_username, const std::string& payload, std::uint64_t dst_conn_id = 0);
  std::uint32_t send_broadcast(const std::string& dst_username, const std::string& payload);
  std::uint32_t send_round_robin(const std::string& dst_username, const std::string& payload, std::uint64_t interval_ms = 0);

  // DATA with typed payload: payload bytes are msgpack map {"type":<str>,"data":<obj>}
  template <class T>
  std::uint32_t send_unicast_typed(const std::string& dst_username, const std::string& type, const T& obj,
                                  std::uint64_t dst_conn_id = 0);
  template <class T>
  std::uint32_t send_broadcast_typed(const std::string& dst_username, const std::string& type, const T& obj);
  template <class T>
  std::uint32_t send_round_robin_typed(const std::string& dst_username, const std::string& type, const T& obj,
                                      std::uint64_t interval_ms = 0);

  // CONTROL (admin only)
  std::uint32_t control_list_users();
  std::uint32_t control_kick_user(std::uint64_t target_user_id);

  // Blocking receive one event. Returns std::nullopt if socket closed.
  std::optional<Event> recv();

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

// ---- inline template helpers ----
namespace detail {
std::string pack_typed_payload(const std::string& type, const msgpack::object& obj);

template <class T>
std::string pack_typed_payload(const std::string& type, const T& v) {
  msgpack::sbuffer sb;
  msgpack::pack(sb, v);
  auto oh = msgpack::unpack(sb.data(), sb.size());
  return pack_typed_payload(type, oh.get());
}
}  // namespace detail

template <class T>
std::uint32_t RelayClient::send_unicast_typed(const std::string& dst_username, const std::string& type, const T& obj,
                                              std::uint64_t dst_conn_id) {
  return send_unicast(dst_username, detail::pack_typed_payload(type, obj), dst_conn_id);
}
template <class T>
std::uint32_t RelayClient::send_broadcast_typed(const std::string& dst_username, const std::string& type, const T& obj) {
  return send_broadcast(dst_username, detail::pack_typed_payload(type, obj));
}
template <class T>
std::uint32_t RelayClient::send_round_robin_typed(const std::string& dst_username, const std::string& type, const T& obj,
                                                  std::uint64_t interval_ms) {
  return send_round_robin(dst_username, detail::pack_typed_payload(type, obj), interval_ms);
}

template <class T>
T TypedPayload::as() const {
  static_assert(!std::is_reference_v<T>);
  if (!valid()) throw std::runtime_error("TypedPayload invalid");
  T out{};
  holder->data.convert(out);
  return out;
}

}  // namespace fwd::sdk

