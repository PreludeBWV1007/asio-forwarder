#pragma once

// =============================================================================
// asio_forwarder 对外主用客户端 API（本头文件为「业务/集成方」主入口）
//
// 为什么还有 relay_client.hpp？
//   · RelayClient 仍在同一 libasio_forwarder_sdk.a 里，负责线协议与阻塞 recv，属于实现细节。
//   · 本头在 RelayClient 之上做：连接配置、注册/登录、DATA 发送、收投递、心跳、管理员 CONTROL 等
//     —— 一般业务只 #include 本头即可，不必再直接包含 relay_client.hpp。
// =============================================================================

#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

#include <msgpack.hpp>

#include "fwd/relay_client.hpp"

namespace fwd::asio_forwarder_client {

// ---------- 连接配置（对中继业务 TCP 口）----------
struct ConnectionConfig {
  std::string host;
  std::uint16_t port{0};
};

// ---------- 发送：模式 + 可选项（目标连接、round_robin 间隔、是否等待 201 受理）----------
enum class SendMode : std::uint8_t { Unicast = 0, Broadcast = 1, RoundRobin = 2 };

struct SendOptions {
  std::uint64_t dst_conn_id{0};
  std::uint64_t round_robin_interval_ms{0};
  bool wait_server_accept{true};
};

// ---------- 仅开发/CI：本机 fork 起 asio_forwarder（Linux）。生产由运维起进程。----------
class LocalForwarder {
 public:
  LocalForwarder();
  ~LocalForwarder();
  LocalForwarder(LocalForwarder&&) noexcept;
  LocalForwarder& operator=(LocalForwarder&&) noexcept;
  LocalForwarder(const LocalForwarder&) = delete;
  LocalForwarder& operator=(const LocalForwarder&) = delete;

  void start();
  void stop();
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::string_view loopback_host() const;
  [[nodiscard]] std::uint16_t client_tcp_port() const;
  [[nodiscard]] std::uint16_t admin_http_port() const;

 private:
  struct Inner;
  std::unique_ptr<Inner> inner_;
};

// ---------- 与一条 TCP + 一个账号（对齐 Web 桥：建连 → 身份 → 收发车）----------
class Client {
 public:
  Client() = default;
  ~Client() = default;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // 1) 建立 TCP
  void open(const ConnectionConfig& cfg);
  // 2) 注册新账号 或 登录已有（register_new=true 为注册首登）
  void sign_on(std::string_view username, std::string_view password, bool register_new_account, std::string_view peer_role = "user");
  [[nodiscard]] const std::string& local_username() const;

  // 3) 手动发心跳（长连接、压测时按需调用）
  void heartbeat();

  // 4) 发 DATA：模式 + 目标用户名 + 负载字节（中继不解析业务语义）
  [[nodiscard]] std::uint32_t send(SendMode mode, std::string_view target_username, const std::string& payload,
                                  const SendOptions& opt = {});

  // Web 多态：{ kind, type, data }
  [[nodiscard]] std::uint32_t send_poly(SendMode mode, std::string_view target_username, int kind,
                                       std::string_view type_name, const msgpack::object& data, const SendOptions& opt = {});

  template <class T>
  [[nodiscard]] std::uint32_t send_poly(SendMode mode, std::string_view target_username, int kind,
                                         std::string_view type_name, const T& data, const SendOptions& opt = {});

  // SDK 约定：{ type, data }，方便 C++ 结构体
  template <class T>
  [[nodiscard]] std::uint32_t send_typed(SendMode mode, std::string_view target_username, std::string_view type_name,
                                        const T& data, const SendOptions& opt = {});

  // 5) 收：阻塞直到下一条 DELIVER
  [[nodiscard]] sdk::Deliver recv_deliver();

  // 6) 管理员 CONTROL（仅 peer_role=admin 的账号可用）
  [[nodiscard]] std::uint32_t control_list_users(bool wait_server_reply = true);
  [[nodiscard]] std::uint32_t control_kick_user(std::uint64_t target_user_id, bool wait_server_reply = true);

  static void log_deliver(const sdk::Deliver& d, std::ostream& os = std::cout);
  [[nodiscard]] sdk::RelayClient& raw();

 private:
  sdk::RelayClient client_;
  std::string username_;
  // 单流按序的下行帧：等 201 时先到达的 200 暂存，供随后 recv_deliver 按原序消耗
  std::deque<sdk::Event> in_order_;
  std::deque<sdk::Deliver> pre_delivers_;
  void drain_server_ack(std::uint32_t seq, const char* op);  // "DATA" 或 "CONTROL"
  void expect_heartbeat_ok(std::uint32_t seq);
};

// ---------- 管理面 HTTP（与中继线协议独立）----------
[[nodiscard]] bool admin_health_ok(std::string_view host, std::uint16_t admin_http_port);

// ---------- 调试用：一次性探测（边界测试用，业务代码通常不需要）----------
[[nodiscard]] bool try_login(std::string_view host, std::uint16_t port, std::string_view user, std::string_view password,
                            std::string_view peer_role = "user");
[[nodiscard]] bool try_register(std::string_view host, std::uint16_t port, std::string_view user, std::string_view password,
                               std::string_view peer_role = "user");

}  // namespace fwd::asio_forwarder_client

namespace fwd::asio_forwarder_client {

template <class T>
std::uint32_t Client::send_poly(SendMode mode, std::string_view target_username, int kind, std::string_view type_name, const T& data,
                                const SendOptions& opt) {
  msgpack::sbuffer sb;
  msgpack::packer<msgpack::sbuffer> pk(&sb);
  pk.pack_map(3);
  pk.pack("kind");
  pk.pack(kind);
  pk.pack("type");
  pk.pack(std::string(type_name));
  pk.pack("data");
  pk.pack(data);
  return send(mode, target_username, std::string(sb.data(), sb.size()), opt);
}

template <class T>
std::uint32_t Client::send_typed(SendMode mode, std::string_view target_username, std::string_view type_name, const T& data,
                                const SendOptions& opt) {
  const std::string t(target_username);
  const std::string ty(type_name);
  std::uint32_t seq = 0;
  switch (mode) {
    case SendMode::Unicast:
      seq = client_.send_unicast_typed(t, ty, data, opt.dst_conn_id);
      break;
    case SendMode::Broadcast:
      seq = client_.send_broadcast_typed(t, ty, data);
      break;
    case SendMode::RoundRobin:
      seq = client_.send_round_robin_typed(t, ty, data, opt.round_robin_interval_ms);
      break;
  }
  if (opt.wait_server_accept) drain_server_ack(seq, "DATA");
  return seq;
}

}  // namespace fwd::asio_forwarder_client
