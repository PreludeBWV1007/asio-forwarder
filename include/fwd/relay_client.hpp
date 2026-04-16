#pragma once

// C++ 客户端 SDK（把中继当黑盒）。
// - v2 fixed header (40B little-endian) + msgpack body
// - 上行 msg_type: login(1) / heartbeat(2) / control(3) / data(4)
// - 下行: deliver(200) / server_reply(201) / kick(202)
//
// 约定：当前实现下行 Header.src_user_id/dst_user_id 均为 0，身份信息在 DELIVER body 信封中。
//
// 目标：给业务方提供一个简单 C++ API：connect/login/send/control/recv。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace fwd::sdk {

struct Deliver {
  std::uint32_t seq{0};
  std::string payload;  // raw bytes
  std::uint64_t src_conn_id{0};
  std::uint64_t dst_conn_id{0};
  std::string src_username;
  std::string dst_username;
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

  // CONTROL (admin only)
  std::uint32_t control_list_users();
  std::uint32_t control_kick_user(std::uint64_t target_user_id);

  // Blocking receive one event. Returns std::nullopt if socket closed.
  std::optional<Event> recv();

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace fwd::sdk

