#pragma once

// asio_forwarder 对外主用客户端 API（业务集成主入口）

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>

#include <msgpack.hpp>

#include "fwd/relay_client.hpp"

namespace fwd::asio_forwarder_client {

struct ConnectionConfig {
  std::string host;
  std::uint16_t port{0};
};

/// 登录时选择的接收策略（须与协议 recv_mode 一致）
enum class RecvMode : std::uint8_t { Broadcast = 0, RoundRobin = 1 };

inline std::string_view recv_mode_str(RecvMode m) {
  return m == RecvMode::Broadcast ? "broadcast" : "round_robin";
}

struct SendOptions {
  bool wait_server_accept{true};
};

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

class Client {
 public:
  Client() = default;
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /** 默认开启：登录成功后后台线程按固定间隔仅发送 HEARTBEAT（不写 socket 读端）；须在 sign_on 前调用。 */
  void set_auto_heartbeat(bool enable);
  /** 默认 20s；小于 1s 按 1s；须在 sign_on 前调用。 */
  void set_auto_heartbeat_interval(std::chrono::seconds interval);

  void open(const ConnectionConfig& cfg);
  void sign_on(std::string_view username, std::string_view password, RecvMode recv_mode,
                std::string_view peer_role = "user");
  [[nodiscard]] const std::string& local_username() const;

  void heartbeat();

  [[nodiscard]] std::uint32_t send(std::string_view target_username, const std::string& payload, const SendOptions& opt = {});

  [[nodiscard]] std::uint32_t send_poly(std::string_view target_username, int kind, std::string_view type_name,
                                        const msgpack::object& data, const SendOptions& opt = {});

  template <class T>
  [[nodiscard]] std::uint32_t send_poly(std::string_view target_username, int kind, std::string_view type_name, const T& data,
                                        const SendOptions& opt = {});

  template <class T>
  [[nodiscard]] std::uint32_t send_typed(std::string_view target_username, std::string_view type_name, const T& data,
                                         const SendOptions& opt = {});

  [[nodiscard]] sdk::Deliver recv_deliver();

  /** list_users CONTROL：wait 为 true 时返回 201 正文 unpack（含 users 等）；否则仅发出请求并返回 nullopt */
  [[nodiscard]] std::optional<msgpack::object_handle> control_list_users(bool wait_server_reply = true);
  [[nodiscard]] std::uint32_t control_kick_user(std::uint64_t target_user_id, bool wait_server_reply = true);

  /** 管理员 CONTROL：body 为完整 msgpack map（须含 action）；返回服务端 201 正文的 unpack（含 ok/op 及自定义字段） */
  [[nodiscard]] msgpack::object_handle control_request(const msgpack::sbuffer& control_body);

  static void log_deliver(const sdk::Deliver& d, std::ostream& os = std::cout);
  [[nodiscard]] sdk::RelayClient& raw();

 private:
  sdk::RelayClient client_;
  std::string username_;
  std::deque<sdk::Event> in_order_;
  std::deque<sdk::Deliver> pre_delivers_;
  std::thread hb_th_;
  std::atomic<bool> hb_stop_{true};
  bool auto_hb_enabled_{true};
  std::chrono::seconds auto_hb_interval_{20};

  void auto_heartbeat_loop_();
  void start_auto_heartbeat_worker_();
  void stop_auto_heartbeat_worker_();
  void drain_server_ack(std::uint32_t seq, const char* op);
  sdk::ServerReply drain_control_full(std::uint32_t seq);
  void expect_heartbeat_ok(std::uint32_t seq);
};

[[nodiscard]] bool admin_health_ok(std::string_view host, std::uint16_t admin_http_port);

[[nodiscard]] bool try_login(std::string_view host, std::uint16_t port, std::string_view user, std::string_view password,
                            RecvMode recv_mode = RecvMode::Broadcast, std::string_view peer_role = "user");

}  // namespace fwd::asio_forwarder_client

namespace fwd::asio_forwarder_client {

template <class T>
std::uint32_t Client::send_poly(std::string_view target_username, int kind, std::string_view type_name, const T& data,
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
  return send(target_username, std::string(sb.data(), sb.size()), opt);
}

template <class T>
std::uint32_t Client::send_typed(std::string_view target_username, std::string_view type_name, const T& data,
                                 const SendOptions& opt) {
  const std::string t(target_username);
  const std::string ty(type_name);
  const std::uint32_t seq = client_.send_data_typed(t, ty, data);
  if (opt.wait_server_accept) drain_server_ack(seq, "DATA");
  return seq;
}

}  // namespace fwd::asio_forwarder_client
