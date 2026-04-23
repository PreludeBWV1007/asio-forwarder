// asio_forwarder_client：在 fwd::sdk::RelayClient 之上的薄封装；RelayClient 为库内实现。

#include "fwd/asio_forwarder_client.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace fwd::asio_forwarder_client {

struct LocalForwarder::Inner {
  pid_t pid{-1};
  std::string cfg_path;
  std::uint16_t client_port{0};
  std::uint16_t admin_port{0};
};

static int pick_free_port() {
  asio::io_context ioc;
  tcp::acceptor acc(ioc, tcp::endpoint(tcp::v4(), 0));
  const int p = static_cast<int>(acc.local_endpoint().port());
  acc.close();
  return p;
}

static std::string make_temp_config_json(int client_port, int admin_port) {
  std::ostringstream o;
  o << R"({"client":{"listen":{"host":"127.0.0.1","port":)"
    << client_port << R"(,"backlog":1024}},"admin":{"listen":{"host":"127.0.0.1","port":)"
    << admin_port << R"(,"backlog":128},"events_max":200},"threads":{"io":2,"biz":2},)"
       R"("timeouts":{"read_ms":120000,"idle_ms":120000},"limits":{"max_body_len":67108864},)"
       R"("metrics":{"interval_ms":3600000},"flow":{"send_queue":{"high_water_bytes":67108864,)"
       R"("hard_limit_bytes":268435456,"on_high_water":"drop"}},"session":{)"
       R"("max_connections_per_user":8,"heartbeat_timeout_ms":30000,)"
       R"("round_robin_default_interval_ms":50,"broadcast_max_recipients":10000}})";
  return o.str();
}

static std::filesystem::path resolve_server_executable() {
  namespace fs = std::filesystem;
  const fs::path self = fs::read_symlink("/proc/self/exe");
  const fs::path dir = self.parent_path();
  if (fs::exists(dir / "asio_forwarder")) return dir / "asio_forwarder";
  throw std::runtime_error("LocalForwarder: 找不到 asio_forwarder 可执行文件");
}

static void wait_tcp(const std::string& host, std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      asio::io_context ioc;
      tcp::socket s(ioc);
      asio::connect(s, tcp::resolver(ioc).resolve(host, std::to_string(static_cast<int>(port))));
      return;
    } catch (...) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  throw std::runtime_error("LocalForwarder: 等待端口超时");
}

LocalForwarder::LocalForwarder() = default;
LocalForwarder::~LocalForwarder() { stop(); }
LocalForwarder::LocalForwarder(LocalForwarder&& o) noexcept : inner_(std::move(o.inner_)) {}
LocalForwarder& LocalForwarder::operator=(LocalForwarder&& o) noexcept {
  if (this != &o) {
    stop();
    inner_ = std::move(o.inner_);
  }
  return *this;
}

void LocalForwarder::start() {
#if !defined(__linux__)
  throw std::runtime_error("LocalForwarder::start 仅支持 Linux");
#else
  stop();
  inner_ = std::make_unique<Inner>();
  inner_->client_port = static_cast<std::uint16_t>(pick_free_port());
  inner_->admin_port = static_cast<std::uint16_t>(pick_free_port());
  inner_->cfg_path = std::string("/tmp/asio_fwd_lcl_") + std::to_string(getpid()) + ".json";
  {
    std::ofstream out(inner_->cfg_path);
    if (!out) throw std::runtime_error("LocalForwarder: 写配置失败");
    out << make_temp_config_json(static_cast<int>(inner_->client_port), static_cast<int>(inner_->admin_port));
  }
  const auto exe = resolve_server_executable();
  inner_->pid = fork();
  if (inner_->pid < 0) throw std::runtime_error("LocalForwarder: fork 失败");
  if (inner_->pid == 0) {
    ::execl(exe.c_str(), "asio_forwarder", inner_->cfg_path.c_str(), static_cast<char*>(nullptr));
    std::perror("execl");
    _exit(127);
  }
  wait_tcp("127.0.0.1", inner_->client_port);
  wait_tcp("127.0.0.1", inner_->admin_port);
#endif
}

void LocalForwarder::stop() {
  if (!inner_) return;
  if (inner_->pid > 0) {
    ::kill(inner_->pid, SIGTERM);
    int st = 0;
    (void)::waitpid(inner_->pid, &st, 0);
    inner_->pid = -1;
  }
  if (!inner_->cfg_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(inner_->cfg_path, ec);
    inner_->cfg_path.clear();
  }
  inner_.reset();
}

bool LocalForwarder::running() const { return static_cast<bool>(inner_); }
std::string_view LocalForwarder::loopback_host() const { return "127.0.0.1"; }
std::uint16_t LocalForwarder::client_tcp_port() const { return inner_ ? inner_->client_port : 0; }
std::uint16_t LocalForwarder::admin_http_port() const { return inner_ ? inner_->admin_port : 0; }

bool admin_health_ok(std::string_view host, std::uint16_t admin_http_port) {
  try {
    asio::io_context ioc;
    tcp::socket s(ioc);
    asio::connect(s, tcp::resolver(ioc).resolve(std::string(host), std::to_string(static_cast<int>(admin_http_port))));
    const std::string req = "GET /api/health HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    asio::write(s, asio::buffer(req));
    std::string buf(4096, '\0');
    boost::system::error_code ec;
    const std::size_t n = s.read_some(asio::buffer(buf), ec);
    buf.resize(n);
    for (char& ch : buf) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return buf.find("ok") != std::string::npos;
  } catch (...) {
    return false;
  }
}

void Client::open(const ConnectionConfig& cfg) { client_.connect(cfg.host, cfg.port); }

void Client::sign_on(std::string_view username, std::string_view password, bool register_new_account, std::string_view peer_role) {
  username_ = std::string(username);
  client_.login(username_, std::string(password), std::string(peer_role), register_new_account);
  for (;;) {
    if (in_order_.empty()) {
      auto ev = client_.recv();
      if (!ev) throw std::runtime_error("sign_on: LOGIN 前 EOF");
      in_order_.push_back(std::move(*ev));
    }
    auto e = std::move(in_order_.front());
    in_order_.pop_front();
    if (const auto* k = std::get_if<fwd::sdk::Kick>(&e)) {
      throw std::runtime_error(std::string("sign_on: KICK ") + k->reason);
    }
    if (std::get_if<fwd::sdk::Deliver>(&e)) {
      throw std::runtime_error("sign_on: 登录阶段不应收到 DELIVER");
    }
    if (const auto* r = std::get_if<fwd::sdk::ServerReply>(&e)) {
      if (r->op == "LOGIN") {
        if (!r->ok) throw std::runtime_error("sign_on: " + r->error);
        return;
      }
      throw std::runtime_error("sign_on: 未预期的 201: op=" + r->op);
    }
  }
}

const std::string& Client::local_username() const { return username_; }

void Client::drain_server_ack(std::uint32_t seq, const char* op) {
  for (;;) {
    if (in_order_.empty()) {
      auto ev = client_.recv();
      if (!ev) throw std::runtime_error(std::string("等待 ") + op + " 应答时 EOF");
      in_order_.push_back(std::move(*ev));
    }
    auto e = std::move(in_order_.front());
    in_order_.pop_front();
    if (const auto* k = std::get_if<fwd::sdk::Kick>(&e)) {
      throw std::runtime_error(std::string("KICK ") + k->reason);
    }
    if (auto* d = std::get_if<fwd::sdk::Deliver>(&e)) {
      pre_delivers_.push_back(std::move(*d));
      continue;
    }
    if (const auto* r = std::get_if<fwd::sdk::ServerReply>(&e)) {
      if (r->seq == seq) {
        // 成功应答均带 op；仅 ok:false 的 err 应答（见 send_err_reply）通常不含 op
        if (!r->ok) {
          throw std::runtime_error(std::string(op) + " 未受理: " + r->error);
        }
        if (r->op == op) {
          return;
        }
        throw std::runtime_error(std::string("等待 ") + op + " 但收到 seq 相同、op 不符的 201: " + r->op);
      }
      throw std::runtime_error(std::string("等待 ") + op + " 时先收到不匹配的 201: seq=" + std::to_string(r->seq) + " op=" + r->op);
    }
  }
}

void Client::expect_heartbeat_ok(std::uint32_t seq) {
  for (;;) {
    if (in_order_.empty()) {
      auto ev = client_.recv();
      if (!ev) throw std::runtime_error("heartbeat: EOF");
      in_order_.push_back(std::move(*ev));
    }
    auto e = std::move(in_order_.front());
    in_order_.pop_front();
    if (const auto* k = std::get_if<fwd::sdk::Kick>(&e)) {
      throw std::runtime_error(std::string("heartbeat: KICK ") + k->reason);
    }
    if (auto* d = std::get_if<fwd::sdk::Deliver>(&e)) {
      pre_delivers_.push_back(std::move(*d));
      continue;
    }
    if (const auto* r = std::get_if<fwd::sdk::ServerReply>(&e)) {
      if (r->seq == seq && r->op == "HEARTBEAT" && r->ok) return;
      throw std::runtime_error("heartbeat: 对 HEARTBEAT 先收到不匹配的 201: seq=" + std::to_string(r->seq) + " op=" + r->op);
    }
  }
}

std::uint32_t Client::send(SendMode mode, std::string_view target_username, const std::string& payload, const SendOptions& opt) {
  const std::string dst(target_username);
  std::uint32_t seq = 0;
  switch (mode) {
    case SendMode::Unicast:
      seq = client_.send_unicast(dst, payload, opt.dst_conn_id);
      break;
    case SendMode::Broadcast:
      seq = client_.send_broadcast(dst, payload);
      break;
    case SendMode::RoundRobin:
      seq = client_.send_round_robin(dst, payload, opt.round_robin_interval_ms);
      break;
  }
  if (opt.wait_server_accept) drain_server_ack(seq, "DATA");
  return seq;
}

std::uint32_t Client::send_poly(SendMode mode, std::string_view target_username, int kind, std::string_view type_name,
                               const msgpack::object& data, const SendOptions& opt) {
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

fwd::sdk::Deliver Client::recv_deliver() {
  for (;;) {
    if (!pre_delivers_.empty()) {
      auto d = std::move(pre_delivers_.front());
      pre_delivers_.pop_front();
      return d;
    }
    if (in_order_.empty()) {
      auto ev = client_.recv();
      if (!ev) throw std::runtime_error("recv_deliver: EOF");
      in_order_.push_back(std::move(*ev));
    }
    auto e = std::move(in_order_.front());
    in_order_.pop_front();
    if (const auto* k = std::get_if<fwd::sdk::Kick>(&e)) {
      throw std::runtime_error(std::string("recv: KICK ") + k->reason);
    }
    if (auto* d = std::get_if<fwd::sdk::Deliver>(&e)) {
      return *d;
    }
    if (const auto* r = std::get_if<fwd::sdk::ServerReply>(&e)) {
      (void)r;
      throw std::runtime_error("recv_deliver: 在收到 200 之前收到 201（多线程/调用顺序不合法，或需先 drain 对端应答）");
    }
  }
}

void Client::heartbeat() {
  const std::uint32_t seq = client_.heartbeat();
  expect_heartbeat_ok(seq);
}

std::uint32_t Client::control_list_users(bool wait_server_reply) {
  const std::uint32_t seq = client_.control_list_users();
  if (wait_server_reply) drain_server_ack(seq, "CONTROL");
  return seq;
}

std::uint32_t Client::control_kick_user(std::uint64_t target_user_id, bool wait_server_reply) {
  const std::uint32_t seq = client_.control_kick_user(target_user_id);
  if (wait_server_reply) drain_server_ack(seq, "CONTROL");
  return seq;
}

void Client::log_deliver(const fwd::sdk::Deliver& d, std::ostream& os) {
  os << "[deliver] " << d.src_username << " -> " << d.dst_username << " bytes=" << d.payload.size() << " src_conn=" << d.src_conn_id
     << " dst_conn=" << d.dst_conn_id;
  if (d.typed && d.typed->valid()) os << " typed=" << d.typed->type;
  os << "\n";
}

sdk::RelayClient& Client::raw() { return client_; }

static bool drain_login_ok(fwd::sdk::RelayClient& c) {
  for (;;) {
    auto ev = c.recv();
    if (!ev) return false;
    if (std::get_if<fwd::sdk::Kick>(&*ev)) return false;
    const auto* r = std::get_if<fwd::sdk::ServerReply>(&*ev);
    if (r) return r->ok;
  }
}

bool try_login(std::string_view host, std::uint16_t port, std::string_view user, std::string_view password, std::string_view peer_role) {
  fwd::sdk::RelayClient c;
  c.connect(std::string(host), port);
  c.login(std::string(user), std::string(password), std::string(peer_role), false);
  return drain_login_ok(c);
}

bool try_register(std::string_view host, std::uint16_t port, std::string_view user, std::string_view password, std::string_view peer_role) {
  fwd::sdk::RelayClient c;
  c.connect(std::string(host), port);
  c.login(std::string(user), std::string(password), std::string(peer_role), true);
  return drain_login_ok(c);
}

}  // namespace fwd::asio_forwarder_client
