// Single translation unit for libasio_forwarder_sdk.a（原 relay_client + asio_forwarder_client 合并）

// C++ RelayClient SDK implementation (sync, blocking).

#include "fwd/relay_client.hpp"

#include <boost/asio.hpp>
#include <msgpack.hpp>

#include <array>
#include <cstring>
#include <optional>
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
    h.reserved0 = 0;
    h.body_len = static_cast<std::uint32_t>(buf.size());
    auto hb = h.pack_le();
    write_all(hb.data(), hb.size());
    if (buf.size()) write_all(buf.data(), buf.size());
  }
};

namespace detail {
std::string pack_typed_payload(const std::string& type, const msgpack::object& obj) {
  msgpack::sbuffer sb;
  msgpack::packer<msgpack::sbuffer> pk(&sb);
  pk.pack_map(2);
  pk.pack("type");
  pk.pack(type);
  pk.pack("data");
  pk.pack(obj);
  return std::string(sb.data(), sb.size());
}
}  // namespace detail

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
                                 const std::string& recv_mode) {
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
  pk.pack("recv_mode");
  pk.pack(recv_mode);
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

std::uint32_t RelayClient::send_data(const std::string& dst_username, const std::string& payload) {
  const std::uint32_t s = impl_->next_seq();
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(2);
  pk.pack("dst_username");
  pk.pack(dst_username);
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

std::uint32_t RelayClient::control_send_raw(const msgpack::sbuffer& body) {
  const std::uint32_t s = impl_->next_seq();
  impl_->send_msgpack(relay::kClientControl, s, body);
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

      // Best-effort typed payload decode: payload bytes are msgpack map {"type":str,"data":<obj>}
      try {
        auto poh = msgpack::unpack(d.payload.data(), d.payload.size());
        msgpack::object po = poh.get();
        if (po.type == msgpack::type::MAP) {
          std::optional<std::string> t;
          const msgpack::object* data_obj = nullptr;
          const auto& pm = po.via.map;
          for (std::uint32_t i = 0; i < pm.size; ++i) {
            if (pm.ptr[i].key.type != msgpack::type::STR) continue;
            std::string kk;
            pm.ptr[i].key.convert(kk);
            if (kk == "type" && pm.ptr[i].val.type == msgpack::type::STR) {
              std::string tv;
              pm.ptr[i].val.convert(tv);
              t = std::move(tv);
            } else if (kk == "data") {
              data_obj = &pm.ptr[i].val;
            }
          }
          if (t && data_obj) {
            TypedPayload tp;
            tp.type = *t;
            tp.holder = std::make_shared<TypedPayload::Holder>(std::move(poh), *data_obj);
            d.typed = std::move(tp);
          }
        }
      } catch (...) {
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
      r.body_raw = body;
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
          } else if (k == "code") {
            if (m.ptr[i].val.type == msgpack::type::POSITIVE_INTEGER) r.code = static_cast<int>(m.ptr[i].val.via.u64);
            else if (m.ptr[i].val.type == msgpack::type::NEGATIVE_INTEGER) r.code = static_cast<int>(m.ptr[i].val.via.i64);
          } else if (k == "message") {
            if (m.ptr[i].val.type == msgpack::type::STR) m.ptr[i].val.convert(r.message);
          } else if (k == "error") {
            if (m.ptr[i].val.type == msgpack::type::STR) m.ptr[i].val.convert(r.message);
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

// asio_forwarder_client：在 fwd::sdk::RelayClient 之上的薄封装；RelayClient 为库内实现。

#include "fwd/asio_forwarder_client.hpp"

#include <cctype>
#include <cstdlib>
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

static std::string json_escape_local(const std::string& in) {
  std::string o;
  for (char c : in) {
    if (c == '\\' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

static std::string make_temp_config_json(int client_port, int admin_port) {
  const char* pw = std::getenv("FWD_MYSQL_PASSWORD");
  const std::string mpw = pw ? std::string(pw) : std::string("e2etest");
  std::ostringstream o;
  o << R"({"client":{"listen":{"host":"127.0.0.1","port":)"
    << client_port << R"(,"backlog":1024}},"admin":{"listen":{"host":"127.0.0.1","port":)"
    << admin_port << R"(,"backlog":128},"events_max":200},"threads":{"io":2,"biz":2},)"
       R"("timeouts":{"read_ms":120000,"idle_ms":120000},"limits":{"max_body_len":67108864},)"
       R"("metrics":{"interval_ms":3600000},"flow":{"send_queue":{"high_water_bytes":67108864,)"
       R"("hard_limit_bytes":268435456,"on_high_water":"drop"}},"session":{)"
       R"("max_connections_per_user":8,"heartbeat_timeout_ms":30000,"broadcast_max_recipients":10000},)"
       R"("mysql":{"host":"127.0.0.1","port":3306,"user":"root","password":")"
    << json_escape_local(mpw) << R"(","database":"forwarder_e2e"}})";
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

void Client::sign_on(std::string_view username, std::string_view password, RecvMode recv_mode, std::string_view peer_role) {
  username_ = std::string(username);
  const std::string rms(recv_mode_str(recv_mode));
  client_.login(username_, std::string(password), std::string(peer_role), rms);
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
      if (!r->ok) {
        throw std::runtime_error("sign_on: " + (r->message.empty() ? "error" : r->message));
      }
      if (r->op == "LOGIN") return;
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
          throw std::runtime_error(std::string(op) + " 未受理: " + (r->message.empty() ? "?" : r->message));
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

fwd::sdk::ServerReply Client::drain_control_full(std::uint32_t seq) {
  for (;;) {
    if (in_order_.empty()) {
      auto ev = client_.recv();
      if (!ev) throw std::runtime_error("CONTROL: EOF");
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
      if (r->seq != seq) {
        throw std::runtime_error("CONTROL: 未预期的 201 seq=" + std::to_string(r->seq));
      }
      if (!r->ok) {
        throw std::runtime_error(std::string("CONTROL 未受理: ") + (r->message.empty() ? "?" : r->message));
      }
      if (r->op != "CONTROL") {
        throw std::runtime_error("CONTROL: op 不符: " + r->op);
      }
      return *r;
    }
  }
}

msgpack::object_handle Client::control_request(const msgpack::sbuffer& control_body) {
  const std::uint32_t s = client_.control_send_raw(control_body);
  const auto rep = drain_control_full(s);
  return msgpack::unpack(rep.body_raw.data(), rep.body_raw.size());
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

std::uint32_t Client::send(std::string_view target_username, const std::string& payload, const SendOptions& opt) {
  const std::string dst(target_username);
  const std::uint32_t seq = client_.send_data(dst, payload);
  if (opt.wait_server_accept) drain_server_ack(seq, "DATA");
  return seq;
}

std::uint32_t Client::send_poly(std::string_view target_username, int kind, std::string_view type_name, const msgpack::object& data,
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

bool try_login(std::string_view host, std::uint16_t port, std::string_view user, std::string_view password, RecvMode recv_mode,
               std::string_view peer_role) {
  fwd::sdk::RelayClient c;
  c.connect(std::string(host), port);
  c.login(std::string(user), std::string(password), std::string(peer_role), std::string(recv_mode_str(recv_mode)));
  return drain_login_ok(c);
}

}  // namespace fwd::asio_forwarder_client
