// 对等客户端中继：单 TCP 接入端口 + v2 帧头 msg_type（login/heartbeat/control/data）+ Msgpack Body
// + admin HTTP。用户↔多连接映射、账户注册/登录、按用户连接转发。

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <msgpack.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fwd/config.hpp"
#include "fwd/log.hpp"
#include "fwd/protocol.hpp"
#include "fwd/relay_constants.hpp"
#include "fwd/sha256.hpp"

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::ip::tcp;
using boost::asio::use_awaitable;
namespace beast = boost::beast;
namespace http = beast::http;

namespace fwd {

std::string proto::Header::to_string() const {
  return "magic=" + std::to_string(magic) + " ver=" + std::to_string(version) +
         " hlen=" + std::to_string(header_len) + " blen=" + std::to_string(body_len) +
         " type=" + std::to_string(msg_type) + " flags=" + std::to_string(flags) +
         " seq=" + std::to_string(seq) + " src_user=" + std::to_string(src_user_id) +
         " dst_user=" + std::to_string(dst_user_id);
}

static std::uint16_t as_u16(int v, const char* name) {
  if (v < 1 || v > 65535) throw std::runtime_error(std::string("invalid ") + name);
  return static_cast<std::uint16_t>(v);
}

Config load_config_or_throw(const std::string& path) {
  boost::property_tree::ptree pt;
  boost::property_tree::read_json(path, pt);

  Config cfg;
  // 新键 client.listen；兼容旧键 upstream.listen（取其一）
  if (pt.get_child_optional("client.listen")) {
    cfg.client_listen.host = pt.get<std::string>("client.listen.host", cfg.client_listen.host);
    cfg.client_listen.port =
        as_u16(pt.get<int>("client.listen.port", cfg.client_listen.port), "client.listen.port");
    cfg.client_listen.backlog = pt.get<int>("client.listen.backlog", cfg.client_listen.backlog);
  } else {
    cfg.client_listen.host = pt.get<std::string>("upstream.listen.host", cfg.client_listen.host);
    cfg.client_listen.port =
        as_u16(pt.get<int>("upstream.listen.port", cfg.client_listen.port), "upstream.listen.port");
    cfg.client_listen.backlog = pt.get<int>("upstream.listen.backlog", cfg.client_listen.backlog);
  }

  cfg.admin.listen.host = pt.get<std::string>("admin.listen.host", cfg.admin.listen.host);
  cfg.admin.listen.port = as_u16(pt.get<int>("admin.listen.port", cfg.admin.listen.port), "admin.listen.port");
  cfg.admin.listen.backlog = pt.get<int>("admin.listen.backlog", cfg.admin.listen.backlog);
  cfg.admin.events_max = pt.get<int>("admin.events_max", cfg.admin.events_max);

  cfg.io_threads = pt.get<int>("threads.io", cfg.io_threads);
  cfg.biz_threads = pt.get<int>("threads.biz", cfg.biz_threads);

  cfg.timeouts.read_ms = pt.get<int>("timeouts.read_ms", cfg.timeouts.read_ms);
  cfg.timeouts.idle_ms = pt.get<int>("timeouts.idle_ms", cfg.timeouts.idle_ms);

  cfg.limits.max_body_len = pt.get<std::uint32_t>("limits.max_body_len", cfg.limits.max_body_len);

  cfg.flow.high_water_bytes = pt.get<std::uint32_t>("flow.send_queue.high_water_bytes", cfg.flow.high_water_bytes);
  cfg.flow.hard_limit_bytes = pt.get<std::uint32_t>("flow.send_queue.hard_limit_bytes", cfg.flow.hard_limit_bytes);
  cfg.flow.on_high_water = pt.get<std::string>("flow.send_queue.on_high_water", cfg.flow.on_high_water);

  cfg.metrics.interval_ms = pt.get<int>("metrics.interval_ms", cfg.metrics.interval_ms);

  cfg.session.max_connections_per_user = pt.get<int>("session.max_connections_per_user", cfg.session.max_connections_per_user);
  cfg.session.heartbeat_timeout_ms = pt.get<int>("session.heartbeat_timeout_ms", cfg.session.heartbeat_timeout_ms);
  cfg.session.round_robin_default_interval_ms =
      pt.get<int>("session.round_robin_default_interval_ms", cfg.session.round_robin_default_interval_ms);
  cfg.session.broadcast_max_recipients =
      pt.get<int>("session.broadcast_max_recipients", cfg.session.broadcast_max_recipients);

  if (cfg.io_threads < 1 || cfg.io_threads > 64) throw std::runtime_error("invalid threads.io");
  if (cfg.biz_threads < 1 || cfg.biz_threads > 64) throw std::runtime_error("invalid threads.biz");
  if (cfg.admin.events_max < 0 || cfg.admin.events_max > 10000) throw std::runtime_error("invalid admin.events_max");
  constexpr std::uint32_t kMaxBodyLenCap = 1024u * 1024u * 1024u;
  if (cfg.limits.max_body_len == 0 || cfg.limits.max_body_len > kMaxBodyLenCap) throw std::runtime_error("invalid limits.max_body_len");
  if (cfg.flow.high_water_bytes == 0 || cfg.flow.hard_limit_bytes < cfg.flow.high_water_bytes) {
    throw std::runtime_error("invalid flow thresholds");
  }
  if (!(cfg.flow.on_high_water == "drop" || cfg.flow.on_high_water == "disconnect")) {
    throw std::runtime_error("invalid flow.send_queue.on_high_water");
  }
  if (cfg.timeouts.read_ms < 100 || cfg.timeouts.idle_ms < cfg.timeouts.read_ms) throw std::runtime_error("invalid timeouts");
  if (cfg.metrics.interval_ms < 200 || cfg.metrics.interval_ms > 60 * 60 * 1000) throw std::runtime_error("invalid metrics.interval_ms");
  if (cfg.session.max_connections_per_user < 1 || cfg.session.max_connections_per_user > 256) {
    throw std::runtime_error("invalid session.max_connections_per_user");
  }
  if (cfg.session.heartbeat_timeout_ms < 1000 || cfg.session.heartbeat_timeout_ms > 24 * 60 * 60 * 1000) {
    throw std::runtime_error("invalid session.heartbeat_timeout_ms");
  }
  if (cfg.session.round_robin_default_interval_ms < 0 || cfg.session.round_robin_default_interval_ms > 60 * 60 * 1000) {
    throw std::runtime_error("invalid session.round_robin_default_interval_ms");
  }
  if (cfg.session.broadcast_max_recipients < 1 || cfg.session.broadcast_max_recipients > 1'000'000) {
    throw std::runtime_error("invalid session.broadcast_max_recipients");
  }

  return cfg;
}

struct Metrics {
  std::atomic<std::uint64_t> client_connects{0};
  std::atomic<std::uint64_t> frames_in{0};
  std::atomic<std::uint64_t> bytes_in{0};
  std::atomic<std::uint64_t> frames_out{0};
  std::atomic<std::uint64_t> bytes_out{0};
  std::atomic<std::uint64_t> drops{0};
};

struct AdminEvent {
  std::uint64_t ts_ms{0};
  const char* level{"INFO"};
  std::string msg;
};

class RelayServer;

class TcpSession : public std::enable_shared_from_this<TcpSession> {
  friend class RelayServer;
 public:
  TcpSession(std::uint64_t conn_id, tcp::socket sock, const Config& cfg, Metrics& m, std::weak_ptr<RelayServer> hub)
      : conn_id_(conn_id),
        socket_(std::move(sock)),
        strand_(boost::asio::make_strand(socket_.get_executor())),
        cfg_(cfg),
        metrics_(m),
        hub_(std::move(hub)),
        created_at_(std::chrono::steady_clock::now()) {}

  std::uint64_t conn_id() const { return conn_id_; }
  bool logged_in() const { return logged_in_; }
  std::uint64_t user_id() const { return user_id_; }
  bool is_admin_account() const { return is_admin_; }
  const std::string& username() const { return username_; }
  bool is_open() const { return socket_.is_open(); }
  std::size_t pending_bytes() const { return pending_bytes_; }

  void touch_heartbeat() { last_hb_ = std::chrono::steady_clock::now(); }

  std::chrono::steady_clock::time_point last_hb_ts() const { return last_hb_; }

  void mark_login(std::uint64_t uid, bool is_admin, std::string username) {
    logged_in_ = true;
    user_id_ = uid;
    is_admin_ = is_admin;
    username_ = std::move(username);
    touch_heartbeat();
  }

  void kick_notify_and_close(std::shared_ptr<std::string> notify_wire, std::string log_why) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, notify_wire = std::move(notify_wire), log_why = std::move(log_why)]() mutable {
      if (self->stopped_ || self->kick_pending_) return;
      self->kick_pending_ = true;
      self->kick_log_ = log_why;
      self->queue_.clear();
      self->pending_bytes_ = 0;
      self->queue_.push_back(std::move(notify_wire));
      self->pending_bytes_ += self->queue_.back()->size();
      if (!self->writing_) self->do_write();
    });
  }

  void stop(const std::string& why) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, why] {
      if (self->stopped_) return;
      self->stopped_ = true;
      boost::system::error_code ec;
      self->socket_.cancel(ec);
      self->socket_.close(ec);
      fwd::log::write(fwd::log::Level::kWarn, "session closed: " + why + " conn=" + std::to_string(self->conn_id_) +
                                                  " ep=" + self->endpoint_str());
    });
  }

  void send_frame(std::shared_ptr<std::string> bytes) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, bytes] {
      if (self->stopped_ || self->kick_pending_) return;
      const auto new_total = self->pending_bytes_ + bytes->size();
      if (new_total > self->cfg_.flow.high_water_bytes && new_total <= self->cfg_.flow.hard_limit_bytes) {
        if (self->cfg_.flow.on_high_water == "disconnect") {
          self->metrics_.drops.fetch_add(1, std::memory_order_relaxed);
          self->stop("high-water disconnect");
          return;
        }
        self->metrics_.drops.fetch_add(1, std::memory_order_relaxed);
        if (!self->high_water_logged_) {
          self->high_water_logged_ = true;
          fwd::log::write(fwd::log::Level::kWarn, "high-water drop conn=" + std::to_string(self->conn_id_));
        }
        return;
      }
      if (new_total > self->cfg_.flow.hard_limit_bytes) {
        self->metrics_.drops.fetch_add(1, std::memory_order_relaxed);
        self->stop("hard-limit disconnect");
        return;
      }
      self->queue_.push_back(std::move(bytes));
      self->pending_bytes_ = new_total;
      if (!self->writing_) self->do_write();
    });
  }

  std::string endpoint_str() const {
    boost::system::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (ec) return "unknown";
    return ep.address().to_string() + ":" + std::to_string(ep.port());
  }

  awaitable<void> read_loop(std::shared_ptr<RelayServer> hub);

 private:
  void do_write() {
    if (queue_.empty() || stopped_) return;
    writing_ = true;
    auto bytes = queue_.front();
    boost::asio::async_write(
        socket_, boost::asio::buffer(*bytes),
        boost::asio::bind_executor(
            strand_, [self = shared_from_this(), bytes](boost::system::error_code ec, std::size_t n) {
              self->writing_ = false;
              if (ec) {
                self->stop("write error: " + ec.message());
                return;
              }
              self->metrics_.frames_out.fetch_add(1, std::memory_order_relaxed);
              self->metrics_.bytes_out.fetch_add(n, std::memory_order_relaxed);
              if (!self->queue_.empty()) self->queue_.pop_front();
              if (self->pending_bytes_ >= bytes->size()) self->pending_bytes_ -= bytes->size();
              else self->pending_bytes_ = 0;
              if (self->kick_pending_ && self->queue_.empty()) {
                self->kick_pending_ = false;
                self->stopped_ = true;
                boost::system::error_code ec2;
                self->socket_.cancel(ec2);
                self->socket_.close(ec2);
                fwd::log::write(fwd::log::Level::kWarn, "session closed after KICK: " + self->kick_log_ +
                                                          " conn=" + std::to_string(self->conn_id_) +
                                                          " ep=" + self->endpoint_str());
                return;
              }
              if (!self->queue_.empty()) self->do_write();
            }));
  }

  std::uint64_t conn_id_{0};
  tcp::socket socket_;
  boost::asio::strand<tcp::socket::executor_type> strand_;
  const Config& cfg_;
  Metrics& metrics_;
  std::weak_ptr<RelayServer> hub_;
  const std::chrono::steady_clock::time_point created_at_;

  bool stopped_{false};
  bool writing_{false};
  std::deque<std::shared_ptr<std::string>> queue_;
  std::size_t pending_bytes_{0};
  bool high_water_logged_{false};

  bool logged_in_{false};
  std::uint64_t user_id_{0};
  bool is_admin_{false};
  std::string username_{};
  bool kick_pending_{false};
  std::string kick_log_{};
  std::chrono::steady_clock::time_point last_hb_{};
};

class RelayServer : public std::enable_shared_from_this<RelayServer> {
  friend class TcpSession;

 public:
  RelayServer(boost::asio::io_context& io, Config cfg)
      : io_(io),
        cfg_(std::move(cfg)),
        client_acceptor_(io_),
        admin_acceptor_(io_),
        metrics_timer_(io_),
        heartbeat_timer_(io_) {}

  void start() {
    open_listen(client_acceptor_, cfg_.client_listen, "client");
    open_listen(admin_acceptor_, cfg_.admin.listen, "admin");
    accept_clients();
    accept_admin();
    tick_metrics();
    tick_heartbeat();
    fwd::log::write(fwd::log::Level::kInfo,
                    "started: client=" + cfg_.client_listen.host + ":" + std::to_string(cfg_.client_listen.port) +
                        " admin=" + cfg_.admin.listen.host + ":" + std::to_string(cfg_.admin.listen.port));
    push_event(fwd::log::Level::kInfo, "relay server started");
  }

  void remove_session(const std::shared_ptr<TcpSession>& s) {
    std::lock_guard lk(mu_);
    by_id_.erase(s->conn_id());
    for (auto it = all_.begin(); it != all_.end(); ++it) {
      if (*it == s) {
        all_.erase(it);
        break;
      }
    }
    if (s->logged_in()) {
      auto du = user_deque_.find(s->user_id());
      if (du != user_deque_.end()) {
        auto& dq = du->second;
        for (auto it = dq.begin(); it != dq.end(); ++it) {
          if (*it == s) {
            dq.erase(it);
            break;
          }
        }
        if (dq.empty()) user_deque_.erase(du);
      }
    }
  }

  void notify_and_close(const std::shared_ptr<TcpSession>& s, std::string user_reason, std::string log_why) {
    if (!s) return;
    if (!s->logged_in()) {
      s->stop(log_why);
      return;
    }
    msgpack::sbuffer kb;
    msgpack::packer<msgpack::sbuffer> pk(&kb);
    pk.pack_map(2);
    pk.pack("op");
    pk.pack("KICK");
    pk.pack("reason");
    pk.pack(user_reason);
    std::string body(kb.data(), kb.size());
    auto wire = pack_wire(relay::kMsgKick, 0, 0, 0, body);
    s->kick_notify_and_close(std::move(wire), std::move(log_why));
  }

  awaitable<void> handle_client_frame(std::shared_ptr<TcpSession> from, const proto::Header& wire_h, std::string body);

 private:
  struct AccountRecord {
    std::string username;
    std::string salt_hex;
    std::string pass_hash_hex;
    bool is_admin{false};
    std::uint64_t user_id{0};
  };

  static std::string random_hex_salt(std::size_t nbytes) {
    std::random_device rd;
    static const char* hx = "0123456789abcdef";
    std::string out(nbytes * 2, '0');
    for (std::size_t i = 0; i < nbytes; ++i) {
      unsigned v = static_cast<unsigned>(rd()) & 0xFF;
      out[i * 2] = hx[v >> 4];
      out[i * 2 + 1] = hx[v & 15];
    }
    return out;
  }

  static bool hex_to_bytes(const std::string& hex, std::string& out) {
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      int hi = nib(hex[i]);
      int lo = nib(hex[i + 1]);
      if (hi < 0 || lo < 0) return false;
      out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
  }

  static std::string hash_password(const std::string& salt_hex, const std::string& password_utf8) {
    std::string salt_raw;
    if (!hex_to_bytes(salt_hex, salt_raw)) return {};
    const std::string material = salt_raw + password_utf8;
    return sha256::hash_hex(material.data(), material.size());
  }

  static std::uint64_t now_ms_wall() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
  }

  static const char* level_name(fwd::log::Level lv) {
    switch (lv) {
      case fwd::log::Level::kInfo:
        return "INFO";
      case fwd::log::Level::kWarn:
        return "WARN";
      case fwd::log::Level::kError:
        return "ERROR";
      case fwd::log::Level::kDebug:
        return "DEBUG";
    }
    return "INFO";
  }

  void push_event(fwd::log::Level lv, std::string msg) {
    if (cfg_.admin.events_max <= 0) return;
    AdminEvent ev;
    ev.ts_ms = now_ms_wall();
    ev.level = level_name(lv);
    ev.msg = std::move(msg);
    std::lock_guard lk(events_mu_);
    events_.push_back(std::move(ev));
    while (static_cast<int>(events_.size()) > cfg_.admin.events_max) events_.pop_front();
  }

  struct StatsSnapshot {
    std::uint64_t ts_ms{0};
    std::uint64_t started_ms{0};
    std::size_t sessions_alive{0};
    std::size_t sessions_logged_in{0};
    std::size_t unique_users{0};
    std::size_t pending_bytes{0};
    std::uint64_t frames_in{0};
    std::uint64_t bytes_in{0};
    std::uint64_t frames_out{0};
    std::uint64_t bytes_out{0};
    std::uint64_t drops{0};
  };

  StatsSnapshot snapshot_stats() {
    StatsSnapshot s;
    s.ts_ms = now_ms_wall();
    s.started_ms = started_ms_;
    s.frames_in = metrics_.frames_in.load(std::memory_order_relaxed);
    s.bytes_in = metrics_.bytes_in.load(std::memory_order_relaxed);
    s.frames_out = metrics_.frames_out.load(std::memory_order_relaxed);
    s.bytes_out = metrics_.bytes_out.load(std::memory_order_relaxed);
    s.drops = metrics_.drops.load(std::memory_order_relaxed);
    std::unordered_set<std::uint64_t> users;
    std::lock_guard lk(mu_);
    std::size_t write_idx = 0;
    for (std::size_t i = 0; i < all_.size(); ++i) {
      auto& sess = all_[i];
      if (sess && sess->is_open()) {
        ++s.sessions_alive;
        if (sess->logged_in()) {
          ++s.sessions_logged_in;
          users.insert(sess->user_id());
        }
        s.pending_bytes += sess->pending_bytes();
        all_[write_idx++] = sess;
      }
    }
    all_.resize(write_idx);
    s.unique_users = users.size();
    return s;
  }

  static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
      switch (c) {
        case '\"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) out += '?';
          else out += c;
      }
    }
    return out;
  }

  std::string build_stats_json() {
    const auto s = snapshot_stats();
    std::string j;
    j.reserve(512);
    j += "{";
    j += "\"ts_ms\":" + std::to_string(s.ts_ms) + ",";
    j += "\"started_ms\":" + std::to_string(s.started_ms) + ",";
    j += "\"sessions_alive\":" + std::to_string(s.sessions_alive) + ",";
    j += "\"sessions_logged_in\":" + std::to_string(s.sessions_logged_in) + ",";
    j += "\"unique_users\":" + std::to_string(s.unique_users) + ",";
    j += "\"pending_bytes\":" + std::to_string(s.pending_bytes) + ",";
    j += "\"frames_in\":" + std::to_string(s.frames_in) + ",";
    j += "\"bytes_in\":" + std::to_string(s.bytes_in) + ",";
    j += "\"frames_out\":" + std::to_string(s.frames_out) + ",";
    j += "\"bytes_out\":" + std::to_string(s.bytes_out) + ",";
    j += "\"drops\":" + std::to_string(s.drops);
    j += "}";
    return j;
  }

  std::string build_users_json() {
    std::lock_guard lk(mu_);
    std::string j;
    j.reserve(1024);
    j += "{\"users\":[";
    bool first = true;
    for (const auto& kv : user_deque_) {
      if (!first) j += ",";
      first = false;
      const std::uint64_t uid = kv.first;
      std::string uname;
      auto itn = uid_to_username_.find(uid);
      if (itn != uid_to_username_.end()) uname = itn->second;
      j += "{\"user_id\":" + std::to_string(uid);
      j += ",\"username\":\"" + json_escape(uname) + "\"";
      j += ",\"connections\":[";
      bool fc = true;
      for (const auto& s : kv.second) {
        if (!s || !s->is_open()) continue;
        if (!fc) j += ",";
        fc = false;
        j += "{\"conn_id\":" + std::to_string(s->conn_id());
        j += ",\"endpoint\":\"" + json_escape(s->endpoint_str()) + "\"}";
      }
      j += "]}";
    }
    j += "]}";
    return j;
  }

  std::string build_events_json() {
    std::deque<AdminEvent> copy;
    {
      std::lock_guard lk(events_mu_);
      copy = events_;
    }
    std::string j;
    j += "{\"events\":[";
    bool first = true;
    for (const auto& e : copy) {
      if (!first) j += ",";
      first = false;
      j += "{";
      j += "\"ts_ms\":" + std::to_string(e.ts_ms) + ",";
      j += "\"level\":\"" + std::string(e.level) + "\",";
      j += "\"msg\":\"" + json_escape(e.msg) + "\"";
      j += "}";
    }
    j += "]}";
    return j;
  }

  awaitable<std::size_t> read_exact_with_timeout(tcp::socket& sock, const boost::asio::mutable_buffer& buf,
                                                std::chrono::milliseconds timeout, boost::system::error_code& ec) {
    auto ex = co_await boost::asio::this_coro::executor;
    auto done = std::make_shared<std::atomic_bool>(false);
    auto t = std::make_shared<boost::asio::steady_timer>(ex);
    t->expires_after(timeout);
    co_spawn(
        ex,
        [t, done, &sock]() -> awaitable<void> {
          boost::system::error_code tec;
          co_await t->async_wait(boost::asio::redirect_error(use_awaitable, tec));
          if (tec) co_return;
          if (!done->load(std::memory_order_acquire)) {
            boost::system::error_code ec2;
            sock.cancel(ec2);
          }
          co_return;
        },
        detached);
    std::size_t n = co_await boost::asio::async_read(sock, buf, boost::asio::redirect_error(use_awaitable, ec));
    done->store(true, std::memory_order_release);
    boost::system::error_code ignore;
    t->cancel(ignore);
    co_return n;
  }

  static void open_listen(tcp::acceptor& acc, const Config::Listen& ls, const std::string& tag) {
    boost::system::error_code ec;
    tcp::endpoint ep(boost::asio::ip::make_address(ls.host, ec), ls.port);
    if (ec) throw std::runtime_error(tag + " listen host invalid: " + ec.message());
    acc.open(ep.protocol(), ec);
    if (ec) throw std::runtime_error(tag + " acceptor open: " + ec.message());
    acc.set_option(boost::asio::socket_base::reuse_address(true), ec);
    acc.bind(ep, ec);
    if (ec) throw std::runtime_error(tag + " bind: " + ec.message());
    acc.listen(ls.backlog, ec);
    if (ec) throw std::runtime_error(tag + " listen: " + ec.message());
  }

  void accept_clients() {
    client_acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket sock) {
      if (ec) {
        fwd::log::write(fwd::log::Level::kError, "accept client error: " + ec.message());
        self->push_event(fwd::log::Level::kError, "accept client error: " + ec.message());
        self->accept_clients();
        return;
      }
      self->metrics_.client_connects.fetch_add(1, std::memory_order_relaxed);
      const std::uint64_t id = ++self->next_conn_id_;
      auto s = std::make_shared<TcpSession>(id, std::move(sock), self->cfg_, self->metrics_, self->weak_from_this());
      {
        std::lock_guard lk(self->mu_);
        self->by_id_[id] = s;
        self->all_.push_back(s);
      }
      fwd::log::write(fwd::log::Level::kInfo, "client connected conn=" + std::to_string(id) + " ep=" + s->endpoint_str());
      self->push_event(fwd::log::Level::kInfo, "client connected conn=" + std::to_string(id));
      co_spawn(self->io_, [self, s]() -> awaitable<void> { co_await s->read_loop(self); }, detached);
      self->accept_clients();
    });
  }

  void accept_admin() {
    admin_acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket sock) {
      if (ec) {
        fwd::log::write(fwd::log::Level::kError, "accept admin error: " + ec.message());
        self->accept_admin();
        return;
      }
      co_spawn(self->io_, [self, s = std::move(sock)]() mutable -> awaitable<void> { co_await self->handle_admin(std::move(s)); },
               detached);
      self->accept_admin();
    });
  }

  awaitable<void> handle_admin(tcp::socket sock) {
    beast::flat_buffer buffer;
    beast::tcp_stream stream(std::move(sock));
    stream.expires_after(std::chrono::seconds(10));
    boost::system::error_code ec;
    http::request<http::string_body> req;
    co_await http::async_read(stream, buffer, req, boost::asio::redirect_error(use_awaitable, ec));
    if (ec) co_return;
    http::response<http::string_body> res;
    res.version(req.version());
    res.keep_alive(false);
    res.set(http::field::server, "asio-relay-admin");
    res.set(http::field::content_type, "application/json; charset=utf-8");
    const std::string target = std::string(req.target());
    if (req.method() != http::verb::get) {
      res.result(http::status::method_not_allowed);
      res.body() = "{\"error\":\"method_not_allowed\"}";
    } else if (target == "/api/health" || target == "/health") {
      res.result(http::status::ok);
      res.body() = "{\"ok\":true}";
    } else if (target == "/api/stats") {
      res.result(http::status::ok);
      res.body() = build_stats_json();
    } else if (target == "/api/events") {
      res.result(http::status::ok);
      res.body() = build_events_json();
    } else if (target == "/api/users") {
      res.result(http::status::ok);
      res.body() = build_users_json();
    } else {
      res.result(http::status::not_found);
      res.body() = "{\"error\":\"not_found\"}";
    }
    res.prepare_payload();
    co_await http::async_write(stream, res, boost::asio::redirect_error(use_awaitable, ec));
    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    co_return;
  }

  void tick_metrics() {
    metrics_timer_.expires_after(std::chrono::milliseconds(cfg_.metrics.interval_ms));
    metrics_timer_.async_wait([self = shared_from_this()](boost::system::error_code ec) {
      if (ec) return;
      const auto s = self->snapshot_stats();
      fwd::log::write(fwd::log::Level::kInfo,
                      "metrics: sessions=" + std::to_string(s.sessions_alive) +
                          " users=" + std::to_string(s.unique_users) + " frames_in=" + std::to_string(s.frames_in) +
                          " bytes_in=" + std::to_string(s.bytes_in) + " frames_out=" + std::to_string(s.frames_out) +
                          " bytes_out=" + std::to_string(s.bytes_out) + " drops=" + std::to_string(s.drops) +
                          " pending_bytes=" + std::to_string(s.pending_bytes));
      self->tick_metrics();
    });
  }

  void tick_heartbeat() {
    heartbeat_timer_.expires_after(std::chrono::seconds(1));
    heartbeat_timer_.async_wait([self = shared_from_this()](boost::system::error_code ec) {
      if (ec) return;
      self->check_heartbeats();
      self->tick_heartbeat();
    });
  }

  void check_heartbeats() {
    const auto timeout = std::chrono::milliseconds(cfg_.session.heartbeat_timeout_ms);
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<TcpSession>> snap;
    {
      std::lock_guard lk(mu_);
      snap = all_;
    }
    for (const auto& s : snap) {
      if (!s || !s->logged_in() || !s->is_open()) continue;
      if (now - s->last_hb_ts() > timeout) {
        notify_and_close(s, "心跳超时：在设定时间内未收到有效业务帧（含心跳）", "heartbeat timeout");
        push_event(fwd::log::Level::kWarn, "heartbeat timeout conn=" + std::to_string(s->conn_id()));
      }
    }
  }

  std::shared_ptr<std::string> pack_wire(std::uint32_t msg_type, std::uint64_t src_uid, std::uint64_t dst_uid,
                                        std::uint32_t seq, const std::string& body) {
    proto::Header h{};
    h.msg_type = msg_type;
    h.src_user_id = src_uid;
    h.dst_user_id = dst_uid;
    h.seq = seq;
    h.body_len = static_cast<std::uint32_t>(body.size());
    auto ph = h.pack_le();
    auto out = std::make_shared<std::string>();
    out->reserve(proto::Header::kHeaderLen + body.size());
    out->append(reinterpret_cast<const char*>(ph.data()), ph.size());
    out->append(body);
    return out;
  }

  std::optional<std::string> map_get_str(const msgpack::object_map& m, const char* key) {
    for (std::uint32_t i = 0; i < m.size; ++i) {
      if (m.ptr[i].key.type != msgpack::type::STR) continue;
      std::string k;
      try {
        m.ptr[i].key.convert(k);
      } catch (...) {
        continue;
      }
      if (k == key && m.ptr[i].val.type == msgpack::type::STR) {
        std::string v;
        m.ptr[i].val.convert(v);
        return v;
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> map_get_u64(const msgpack::object_map& m, const char* key) {
    for (std::uint32_t i = 0; i < m.size; ++i) {
      if (m.ptr[i].key.type != msgpack::type::STR) continue;
      std::string k;
      try {
        m.ptr[i].key.convert(k);
      } catch (...) {
        continue;
      }
      if (k != key) continue;
      const auto& v = m.ptr[i].val;
      if (v.type == msgpack::type::POSITIVE_INTEGER) {
        return static_cast<std::uint64_t>(v.via.u64);
      }
      if (v.type == msgpack::type::NEGATIVE_INTEGER) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  bool map_get_bool(const msgpack::object_map& m, const char* key, bool def) {
    for (std::uint32_t i = 0; i < m.size; ++i) {
      if (m.ptr[i].key.type != msgpack::type::STR) continue;
      std::string k;
      try {
        m.ptr[i].key.convert(k);
      } catch (...) {
        continue;
      }
      if (k == key && m.ptr[i].val.type == msgpack::type::BOOLEAN) return m.ptr[i].val.via.boolean;
    }
    return def;
  }

  const msgpack::object* map_get_obj(const msgpack::object_map& m, const char* key) {
    for (std::uint32_t i = 0; i < m.size; ++i) {
      if (m.ptr[i].key.type != msgpack::type::STR) continue;
      std::string k;
      try {
        m.ptr[i].key.convert(k);
      } catch (...) {
        continue;
      }
      if (k == key) return &m.ptr[i].val;
    }
    return nullptr;
  }

  std::optional<std::string> extract_payload_bin(const msgpack::object* o) {
    if (!o) return std::nullopt;
    if (o->type == msgpack::type::BIN) {
      return std::string(reinterpret_cast<const char*>(o->via.bin.ptr), o->via.bin.size);
    }
    if (o->type == msgpack::type::STR) {
      return std::string(o->via.str.ptr, o->via.str.size);
    }
    return std::nullopt;
  }

  void send_reply(const std::shared_ptr<TcpSession>& to, const msgpack::sbuffer& buf, std::uint32_t seq = 0) {
    std::string body(buf.data(), buf.size());
    to->send_frame(pack_wire(relay::kMsgServerReply, 0, 0, seq, body));
  }

  void send_err_reply(const std::shared_ptr<TcpSession>& from, std::uint32_t seq, const std::string& err) {
    msgpack::sbuffer eb;
    msgpack::packer<msgpack::sbuffer> pk(&eb);
    pk.pack_map(2);
    pk.pack("ok");
    pk.pack(false);
    pk.pack("error");
    pk.pack(err);
    send_reply(from, eb, seq);
  }

  std::string pack_deliver_envelope(const std::string& payload, std::uint64_t src_conn, std::uint64_t dst_conn,
                                    const std::string& src_username, const std::string& dst_username) {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_map(5);
    pk.pack("payload");
    pk.pack_bin(static_cast<std::uint32_t>(payload.size()));
    pk.pack_bin_body(payload.data(), static_cast<std::uint32_t>(payload.size()));
    pk.pack("src_conn_id");
    pk.pack(src_conn);
    pk.pack("dst_conn_id");
    pk.pack(dst_conn);
    pk.pack("src_username");
    pk.pack(src_username);
    pk.pack("dst_username");
    pk.pack(dst_username);
    return std::string(buf.data(), buf.size());
  }

  std::optional<std::uint64_t> username_to_uid(const std::string& username) {
    std::lock_guard lk(mu_);
    auto it = accounts_.find(username);
    if (it == accounts_.end()) return std::nullopt;
    return it->second.user_id;
  }

  std::optional<std::string> try_authenticate(const std::shared_ptr<TcpSession>& self, const msgpack::object_map& mp) {
    const auto un = map_get_str(mp, "username");
    const auto pw = map_get_str(mp, "password");
    const auto pr = map_get_str(mp, "peer_role");
    if (!un || un->empty()) return std::string("缺少 username");
    if (!pw) return std::string("缺少 password");
    if (!pr) return std::string("缺少 peer_role（user 或 admin）");
    std::string peer = *pr;
    if (peer != "admin" && peer != "user") return std::string("peer_role 必须是 user 或 admin");
    const bool want_admin = (peer == "admin");
    const bool reg = map_get_bool(mp, "register", false);

    std::vector<std::pair<std::shared_ptr<TcpSession>, std::string>> to_notify;
    {
      std::lock_guard lk(mu_);
      if (reg) {
        if (accounts_.find(*un) != accounts_.end()) return std::string("用户名已存在");
        AccountRecord rec;
        rec.username = *un;
        rec.salt_hex = random_hex_salt(16);
        rec.pass_hash_hex = hash_password(rec.salt_hex, *pw);
        if (rec.pass_hash_hex.empty()) return std::string("口令处理失败");
        rec.is_admin = want_admin;
        rec.user_id = ++next_user_id_;
        accounts_[rec.username] = rec;
        uid_to_username_[rec.user_id] = rec.username;
        auto& dq = user_deque_[rec.user_id];
        dq.push_back(self);
        while (dq.size() > static_cast<std::size_t>(cfg_.session.max_connections_per_user)) {
          auto victim = dq.front();
          dq.pop_front();
          to_notify.push_back({victim, "因单用户连接数达到上限，服务端按策略释放了最旧的一条连接"});
        }
        self->mark_login(rec.user_id, rec.is_admin, rec.username);
      } else {
        auto it = accounts_.find(*un);
        if (it == accounts_.end()) return std::string("用户不存在");
        if (it->second.pass_hash_hex != hash_password(it->second.salt_hex, *pw)) return std::string("密码错误");
        if (it->second.is_admin != want_admin) return std::string("登录所选权限与账号不一致");
        const std::uint64_t uid = it->second.user_id;
        auto& dq = user_deque_[uid];
        dq.push_back(self);
        while (dq.size() > static_cast<std::size_t>(cfg_.session.max_connections_per_user)) {
          auto victim = dq.front();
          dq.pop_front();
          to_notify.push_back({victim, "因单用户连接数达到上限，服务端按策略释放了最旧的一条连接"});
        }
        self->mark_login(uid, it->second.is_admin, it->second.username);
      }
    }
    for (const auto& pr2 : to_notify) {
      if (pr2.first && pr2.first != self) notify_and_close(pr2.first, pr2.second, "max_connections_per_user");
    }
    return std::nullopt;
  }

  std::shared_ptr<TcpSession> find_user_connection(std::uint64_t uid, std::uint64_t conn_id) {
    std::lock_guard lk(mu_);
    auto it = user_deque_.find(uid);
    if (it == user_deque_.end()) return nullptr;
    if (conn_id == 0) {
      for (const auto& s : it->second) {
        if (s && s->is_open()) return s;
      }
      return nullptr;
    }
    for (const auto& s : it->second) {
      if (s && s->is_open() && s->conn_id() == conn_id) return s;
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<TcpSession>> list_user_connections(std::uint64_t uid) {
    std::lock_guard lk(mu_);
    std::vector<std::shared_ptr<TcpSession>> out;
    auto it = user_deque_.find(uid);
    if (it == user_deque_.end()) return out;
    for (const auto& s : it->second) {
      if (s && s->is_open()) out.push_back(s);
    }
    return out;
  }

  void deliver_unicast_by_username(const std::string& src_username, std::uint64_t src_conn_id, const std::string& dst_username,
                                   std::uint64_t dst_conn_id, const std::string& payload, std::uint32_t seq) {
    auto dst_uid = username_to_uid(dst_username);
    if (!dst_uid) return;
    auto s = find_user_connection(*dst_uid, dst_conn_id);
    if (!s) return;
    const auto body = pack_deliver_envelope(payload, src_conn_id, s->conn_id(), src_username, dst_username);
    s->send_frame(pack_wire(relay::kMsgDeliver, 0, 0, seq, body));
  }

  void deliver_broadcast_to_username(const std::string& src_username, std::uint64_t src_conn_id, const std::string& dst_username,
                                     const std::string& payload, std::uint32_t seq) {
    auto dst_uid = username_to_uid(dst_username);
    if (!dst_uid) return;
    auto targets = list_user_connections(*dst_uid);
    int sent = 0;
    const int cap = cfg_.session.broadcast_max_recipients;
    for (const auto& s : targets) {
      if (sent >= cap) {
        push_event(fwd::log::Level::kWarn, "broadcast truncated at max_recipients");
        break;
      }
      const auto body = pack_deliver_envelope(payload, src_conn_id, s->conn_id(), src_username, dst_username);
      s->send_frame(pack_wire(relay::kMsgDeliver, 0, 0, seq, body));
      ++sent;
    }
  }

  void schedule_round_robin_username(const std::string& src_username, std::uint64_t src_conn_id, const std::string& dst_username,
                                     const std::string& payload, std::uint32_t seq, int interval_ms) {
    auto dst_uid = username_to_uid(dst_username);
    if (!dst_uid) return;
    auto targets = list_user_connections(*dst_uid);
    if (interval_ms <= 0) interval_ms = cfg_.session.round_robin_default_interval_ms;
    auto self = shared_from_this();
    co_spawn(
        io_,
        [self, targets, payload, seq, src_username, src_conn_id, dst_username, interval_ms]() -> awaitable<void> {
          for (std::size_t i = 0; i < targets.size(); ++i) {
            auto s = targets[i];
            if (s && s->is_open()) {
              const auto body = self->pack_deliver_envelope(payload, src_conn_id, s->conn_id(), src_username, dst_username);
              s->send_frame(self->pack_wire(relay::kMsgDeliver, 0, 0, seq, body));
            }
            if (i + 1 < targets.size() && interval_ms > 0) {
              auto ex = co_await boost::asio::this_coro::executor;
              boost::asio::steady_timer t(ex);
              t.expires_after(std::chrono::milliseconds(interval_ms));
              co_await t.async_wait(use_awaitable);
            }
          }
          co_return;
        },
        detached);
  }

  boost::asio::io_context& io_;
  Config cfg_;
  tcp::acceptor client_acceptor_;
  tcp::acceptor admin_acceptor_;
  boost::asio::steady_timer metrics_timer_;
  boost::asio::steady_timer heartbeat_timer_;

  std::mutex mu_;
  std::uint64_t next_conn_id_{0};
  std::vector<std::shared_ptr<TcpSession>> all_;
  std::unordered_map<std::uint64_t, std::shared_ptr<TcpSession>> by_id_;
  std::unordered_map<std::uint64_t, std::deque<std::shared_ptr<TcpSession>>> user_deque_;
  std::unordered_map<std::string, AccountRecord> accounts_;
  std::unordered_map<std::uint64_t, std::string> uid_to_username_;
  std::uint64_t next_user_id_{0};

  Metrics metrics_{};
  const std::uint64_t started_ms_{now_ms_wall()};
  std::mutex events_mu_;
  std::deque<AdminEvent> events_;
};

awaitable<void> TcpSession::read_loop(std::shared_ptr<RelayServer> hub) {
  boost::system::error_code ec;
  auto last_activity = std::chrono::steady_clock::now();
  while (true) {
    if (stopped_) break;
    if (hub->cfg_.timeouts.idle_ms > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_activity > std::chrono::milliseconds(hub->cfg_.timeouts.idle_ms)) {
        hub->notify_and_close(shared_from_this(), "空闲超时：长时间无任何帧收发", "idle timeout");
        break;
      }
    }
    std::array<std::uint8_t, proto::Header::kHeaderLen> hb{};
    std::size_t n = co_await hub->read_exact_with_timeout(socket_, boost::asio::buffer(hb),
                                                          std::chrono::milliseconds(hub->cfg_.timeouts.read_ms), ec);
    if (ec) break;
    if (n != hb.size()) break;
    last_activity = std::chrono::steady_clock::now();

    const auto h = proto::Header::unpack_le(hb.data());
    if (h.magic != proto::Header::kMagic || h.version != proto::Header::kVersion || h.header_len != proto::Header::kHeaderLen) {
      stop("invalid header");
      break;
    }
    if (h.body_len > hub->cfg_.limits.max_body_len) {
      stop("body too large");
      break;
    }
    std::string body;
    body.resize(h.body_len);
    if (h.body_len > 0) {
      std::size_t bn = co_await hub->read_exact_with_timeout(socket_, boost::asio::buffer(body),
                                                              std::chrono::milliseconds(hub->cfg_.timeouts.read_ms), ec);
      if (ec) break;
      if (bn != h.body_len) break;
    }
    last_activity = std::chrono::steady_clock::now();
    hub->metrics_.frames_in.fetch_add(1, std::memory_order_relaxed);
    hub->metrics_.bytes_in.fetch_add(proto::Header::kHeaderLen + body.size(), std::memory_order_relaxed);

    co_await hub->handle_client_frame(shared_from_this(), h, std::move(body));
    if (stopped_) break;
  }
  if (auto hs = hub_.lock()) hs->remove_session(shared_from_this());
  boost::system::error_code ec2;
  socket_.cancel(ec2);
  socket_.close(ec2);
  co_return;
}

awaitable<void> RelayServer::handle_client_frame(std::shared_ptr<TcpSession> from, const proto::Header& wire_h,
                                                std::string body) {
  const std::uint32_t mtype = wire_h.msg_type;

  if (!from->logged_in()) {
    if (mtype != relay::kClientLogin) {
      from->stop("需要先登录：首帧 Header.msg_type 必须为 login(1)");
      co_return;
    }
    msgpack::object_handle oh;
    try {
      oh = msgpack::unpack(body.data(), body.size());
    } catch (...) {
      from->stop("invalid msgpack body");
      co_return;
    }
    const msgpack::object& root = oh.get();
    if (root.type != msgpack::type::MAP) {
      from->stop("body must be msgpack map");
      co_return;
    }
    const auto& mp = root.via.map;
    if (const auto err = try_authenticate(from, mp)) {
      send_err_reply(from, wire_h.seq, *err);
      co_return;
    }
    const std::string peer_str = from->is_admin_account() ? "admin" : "user";
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_map(6);
    pk.pack("ok");
    pk.pack(true);
    pk.pack("op");
    pk.pack("LOGIN");
    pk.pack("conn_id");
    pk.pack(from->conn_id());
    pk.pack("user_id");
    pk.pack(from->user_id());
    pk.pack("username");
    pk.pack(from->username());
    pk.pack("peer_role");
    pk.pack(peer_str);
    send_reply(from, buf, wire_h.seq);
    fwd::log::write(fwd::log::Level::kInfo, "login ok conn=" + std::to_string(from->conn_id()) + " ep=" +
                                                 from->endpoint_str() + " user=" + from->username() +
                                                 " uid=" + std::to_string(from->user_id()) + " seq=" +
                                                 std::to_string(wire_h.seq));
    push_event(fwd::log::Level::kInfo, "login ok user=" + from->username());
    co_return;
  }

  msgpack::object_handle oh;
  try {
    oh = msgpack::unpack(body.data(), body.size());
  } catch (...) {
    from->stop("invalid msgpack body");
    co_return;
  }
  const msgpack::object& root = oh.get();
  if (root.type != msgpack::type::MAP) {
    from->stop("body must be msgpack map");
    co_return;
  }
  const auto& mp = root.via.map;

  if (mtype == relay::kClientHeartbeat) {
    from->touch_heartbeat();
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_map(2);
    pk.pack("ok");
    pk.pack(true);
    pk.pack("op");
    pk.pack("HEARTBEAT");
    send_reply(from, buf, wire_h.seq);
    co_return;
  }

  from->touch_heartbeat();

  if (mtype == relay::kClientData) {
    const auto mode = map_get_str(mp, "mode").value_or("unicast");
    const auto* pay_obj = map_get_obj(mp, "payload");
    auto payload = extract_payload_bin(pay_obj);
    if (!payload) {
      send_err_reply(from, wire_h.seq, "DATA 缺少 payload（bin 或 str）");
      co_return;
    }
    const auto dst_username = map_get_str(mp, "dst_username");
    const std::uint64_t dst_conn = map_get_u64(mp, "dst_conn_id").value_or(0);
    const int interval_ms = static_cast<int>(map_get_u64(mp, "interval_ms").value_or(0));
    const std::uint64_t src_c = from->conn_id();
    const std::string& src_un = from->username();

    if (mode == "unicast") {
      if (!dst_username || dst_username->empty()) {
        send_err_reply(from, wire_h.seq, "unicast 需要 dst_username");
        co_return;
      }
      deliver_unicast_by_username(src_un, src_c, *dst_username, dst_conn, *payload, wire_h.seq);
    } else if (mode == "broadcast") {
      if (!dst_username || dst_username->empty()) {
        send_err_reply(from, wire_h.seq, "broadcast 需要 dst_username（发往该用户全部连接）");
        co_return;
      }
      deliver_broadcast_to_username(src_un, src_c, *dst_username, *payload, wire_h.seq);
    } else if (mode == "round_robin" || mode == "roundrobin") {
      if (!dst_username || dst_username->empty()) {
        send_err_reply(from, wire_h.seq, "round_robin 需要 dst_username（该用户全部连接等间隔轮询）");
        co_return;
      }
      schedule_round_robin_username(src_un, src_c, *dst_username, *payload, wire_h.seq, interval_ms);
    } else {
      send_err_reply(from, wire_h.seq, "未知 mode");
      co_return;
    }
    msgpack::sbuffer ack;
    msgpack::packer<msgpack::sbuffer> pk(&ack);
    pk.pack_map(2);
    pk.pack("ok");
    pk.pack(true);
    pk.pack("op");
    pk.pack("DATA");
    send_reply(from, ack, wire_h.seq);
    co_return;
  }

  if (mtype == relay::kClientControl) {
    if (!from->is_admin_account()) {
      send_err_reply(from, wire_h.seq, "需要管理员账号权限");
      co_return;
    }
    const auto action = map_get_str(mp, "action").value_or("");
    if (action == "list_users") {
      std::lock_guard lk(mu_);
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(3);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      pk.pack("users");
      pk.pack_array(user_deque_.size());
      for (const auto& kv : user_deque_) {
        std::string uname;
        auto itn = uid_to_username_.find(kv.first);
        if (itn != uid_to_username_.end()) uname = itn->second;
        std::size_t nopen = 0;
        for (const auto& s : kv.second) {
          if (s && s->is_open()) ++nopen;
        }
        pk.pack_map(3);
        pk.pack("user_id");
        pk.pack(kv.first);
        pk.pack("username");
        pk.pack(uname);
        pk.pack("connections");
        pk.pack_array(static_cast<unsigned>(nopen));
        for (const auto& s : kv.second) {
          if (!s || !s->is_open()) continue;
          pk.pack_map(2);
          pk.pack("conn_id");
          pk.pack(s->conn_id());
          pk.pack("endpoint");
          pk.pack(s->endpoint_str());
        }
      }
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    if (action == "kick_user") {
      const auto target = map_get_u64(mp, "target_user_id");
      if (!target) {
        send_err_reply(from, wire_h.seq, "kick_user 需要 target_user_id");
        co_return;
      }
      std::vector<std::shared_ptr<TcpSession>> victims;
      {
        std::lock_guard lk(mu_);
        auto it = user_deque_.find(*target);
        if (it != user_deque_.end()) {
          victims.assign(it->second.begin(), it->second.end());
        }
      }
      const std::string kick_msg =
          "被管理员踢出：操作者 " + from->username() + "（user_id=" + std::to_string(from->user_id()) +
          "）执行了 kick_user，目标 user_id=" + std::to_string(*target);
      for (const auto& v : victims) {
        if (v) notify_and_close(v, kick_msg, "kick_user");
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(3);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      pk.pack("kicked_count");
      pk.pack(static_cast<unsigned>(victims.size()));
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    send_err_reply(from, wire_h.seq, "unknown action");
    co_return;
  }

  send_err_reply(from, wire_h.seq, "未知 msg_type（已登录帧须为 heartbeat=2 / control=3 / data=4）");
  co_return;
}

}  // namespace fwd

int main(int argc, char** argv) {
  try {
    std::string cfg_path = "configs/dev/forwarder.json";
    if (argc >= 2) cfg_path = argv[1];

    auto cfg = fwd::load_config_or_throw(cfg_path);

    boost::asio::io_context io;
    auto srv = std::make_shared<fwd::RelayServer>(io, cfg);
    srv->start();

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.io_threads));
    for (int i = 0; i < cfg.io_threads; ++i) {
      threads.emplace_back([&io] { io.run(); });
    }
    for (auto& t : threads) t.join();
    return 0;
  } catch (const std::exception& e) {
    fwd::log::write(fwd::log::Level::kError, std::string("fatal: ") + e.what());
    return 1;
  }
}
