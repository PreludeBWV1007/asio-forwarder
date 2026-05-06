#pragma once

// C++ 客户端 SDK（把中继当黑盒）。
// - v3 fixed header (24B little-endian) + msgpack body
// - 上行 msg_type: login(1) / heartbeat(2) / control(3) / data(4)
// - 下行: deliver(200) / server_reply(201) / kick(202)
//
// 身份信息在 DELIVER body 的 src_username / dst_username 中。
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
  int code{0};
  std::string op;      // LOGIN / HEARTBEAT / DATA / CONTROL (best-effort)
  std::string message; // ok=false 时服务端 message；或旧字段 error
  std::string raw;     // debug
  std::string body_raw; // 完整 201 正文体（msgpack），便于解析自定义 CONTROL 回复字段
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

  // 登录；recv_mode: "broadcast" | "round_robin"（由服务端按多连接策略转发 DATA）
  std::uint32_t login(const std::string& username, const std::string& password, const std::string& peer_role,
                        const std::string& recv_mode);
  std::uint32_t heartbeat();

  // DATA：仅 dst_username + payload（bin），路由由对端用户登录时选择的 recv_mode 决定
  std::uint32_t send_data(const std::string& dst_username, const std::string& payload);

  template <class T>
  std::uint32_t send_data_typed(const std::string& dst_username, const std::string& type, const T& obj);

  // CONTROL (admin only)
  std::uint32_t control_list_users();
  std::uint32_t control_kick_user(std::uint64_t target_user_id);
  /** CONTROL 正文为已 pack 好的 msgpack map（须含 action 等键）；seq 由本函数分配 */
  std::uint32_t control_send_raw(const msgpack::sbuffer& body);

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
std::uint32_t RelayClient::send_data_typed(const std::string& dst_username, const std::string& type, const T& obj) {
  return send_data(dst_username, detail::pack_typed_payload(type, obj));
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

