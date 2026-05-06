// 对等客户端中继：单 TCP 接入端口 + v3 帧头 msg_type（login/heartbeat/control/data）+ Msgpack Body
// + admin HTTP + MySQL 鉴权。用户↔多连接、按接收策略转发 DATA。

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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <mysql/mysql.h>

#include <algorithm>
#include <cstring>

#include "fwd/config.hpp"
#include "fwd/log.hpp"
#include "fwd/protocol.hpp"
#include "fwd/relay_constants.hpp"
#include "mysql_store.hpp"

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
         " type=" + std::to_string(msg_type) + " seq=" + std::to_string(seq) +
         " r0=" + std::to_string(reserved0);
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
  cfg.session.broadcast_max_recipients =
      pt.get<int>("session.broadcast_max_recipients", cfg.session.broadcast_max_recipients);

  if (!pt.get_child_optional("mysql")) throw std::runtime_error("config must contain mysql { host, port, user, password, database }");
  cfg.mysql.host = pt.get<std::string>("mysql.host", cfg.mysql.host);
  cfg.mysql.port = as_u16(pt.get<int>("mysql.port", cfg.mysql.port), "mysql.port");
  cfg.mysql.user = pt.get<std::string>("mysql.user", cfg.mysql.user);
  cfg.mysql.password = pt.get<std::string>("mysql.password", cfg.mysql.password);
  cfg.mysql.database = pt.get<std::string>("mysql.database", cfg.mysql.database);
  if (cfg.mysql.user.empty() || cfg.mysql.database.empty()) throw std::runtime_error("mysql.user and mysql.database required");

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

  std::string client_ip() const {
    boost::system::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (ec) return "";
    return ep.address().to_string();
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
    db_.connect(cfg_.mysql);
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
      const std::uint64_t uid = s->user_id();
      auto du = user_deque_.find(uid);
      if (du != user_deque_.end()) {
        auto& dq = du->second;
        for (auto it = dq.begin(); it != dq.end(); ++it) {
          if (*it == s) {
            dq.erase(it);
            break;
          }
        }
        if (dq.empty()) {
          user_deque_.erase(du);
          user_recv_mode_.erase(uid);
          rr_next_index_.erase(uid);
        }
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
    auto wire = pack_wire(relay::kMsgKick, 0, body);
    s->kick_notify_and_close(std::move(wire), std::move(log_why));
  }

  awaitable<void> handle_client_frame(std::shared_ptr<TcpSession> from, const proto::Header& wire_h, std::string body);

 private:
  enum class RecvMode { kBroadcast, kRoundRobin };

  static std::optional<RecvMode> parse_recv_mode_str(const std::string& s) {
    if (s == "broadcast") return RecvMode::kBroadcast;
    if (s == "round_robin" || s == "roundrobin") return RecvMode::kRoundRobin;
    return std::nullopt;
  }

  static const char* recv_mode_cstr(RecvMode m) {
    return m == RecvMode::kBroadcast ? "broadcast" : "round_robin";
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
      bool is_ad = false;
      for (const auto& s : kv.second) {
        if (s && s->logged_in()) {
          is_ad = s->is_admin_account();
          break;
        }
      }
      j += ",\"is_admin\":" + std::string(is_ad ? "true" : "false");
      std::string rms = "broadcast";
      if (const auto itm = user_recv_mode_.find(uid); itm != user_recv_mode_.end()) {
        rms = recv_mode_cstr(itm->second);
      }
      j += ",\"recv_mode\":\"" + json_escape(rms) + "\"";
      j += ",\"connections\":[";
      bool fc = true;
      for (const auto& s : kv.second) {
        if (!s || !s->is_open()) continue;
        if (!fc) j += ",";
        fc = false;
        j += "{\"conn_id\":" + std::to_string(s->conn_id());
        j += ",\"endpoint\":\"" + json_escape(s->endpoint_str()) + "\""
             ",\"pending_bytes\":" + std::to_string(s->pending_bytes()) + "}";
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

  static void add_admin_cors(http::response<http::string_body>& res) {
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
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
    const std::string target = std::string(req.target());
    if (req.method() == http::verb::options) {
      res.result(http::status::no_content);
      add_admin_cors(res);
      res.prepare_payload();
      co_await http::async_write(stream, res, boost::asio::redirect_error(use_awaitable, ec));
      stream.socket().shutdown(tcp::socket::shutdown_send, ec);
      co_return;
    }
    if (req.method() != http::verb::get) {
      res.set(http::field::content_type, "application/json; charset=utf-8");
      res.result(http::status::method_not_allowed);
      res.body() = "{\"error\":\"method_not_allowed\"}";
      add_admin_cors(res);
    } else {
      res.set(http::field::content_type, "application/json; charset=utf-8");
      add_admin_cors(res);
      if (target == "/api/health" || target == "/health") {
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

  std::shared_ptr<std::string> pack_wire(std::uint32_t msg_type, std::uint32_t seq, const std::string& body) {
    proto::Header h{};
    h.msg_type = msg_type;
    h.seq = seq;
    h.reserved0 = 0;
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

  bool map_has_key(const msgpack::object_map& m, const char* key) {
    for (std::uint32_t i = 0; i < m.size; ++i) {
      if (m.ptr[i].key.type != msgpack::type::STR) continue;
      std::string k;
      try {
        m.ptr[i].key.convert(k);
      } catch (...) {
        continue;
      }
      if (k == key) return true;
    }
    return false;
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
    to->send_frame(pack_wire(relay::kMsgServerReply, seq, body));
  }

  void send_err_reply(const std::shared_ptr<TcpSession>& from, std::uint32_t seq, int code, const std::string& message) {
    msgpack::sbuffer eb;
    msgpack::packer<msgpack::sbuffer> pk(&eb);
    pk.pack_map(3);
    pk.pack("ok");
    pk.pack(false);
    pk.pack("code");
    pk.pack(code);
    pk.pack("message");
    pk.pack(message);
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

  struct LoginOk {
    RecvMode effective_mode{};
    std::optional<std::string> recv_mode_notice;
  };

  // 成功：返回 nullopt 并填 out_ok；失败：返回 {code,message}
  std::optional<std::pair<int, std::string>> try_authenticate(const std::shared_ptr<TcpSession>& self, const msgpack::object_map& mp,
                                                            LoginOk& out_ok) {
    const auto un = map_get_str(mp, "username");
    const auto pw = map_get_str(mp, "password");
    const auto pr = map_get_str(mp, "peer_role");
    const auto rm = map_get_str(mp, "recv_mode");
    if (!un || un->empty()) return std::make_pair(relay::errc::kInvalidLoginBody, std::string("缺少 username"));
    if (!pw) return std::make_pair(relay::errc::kInvalidLoginBody, std::string("缺少 password"));
    if (!pr) return std::make_pair(relay::errc::kInvalidLoginBody, std::string("缺少 peer_role（user 或 admin）"));
    if (!rm) return std::make_pair(relay::errc::kInvalidLoginBody, std::string("缺少 recv_mode（broadcast 或 round_robin）"));
    std::string peer = *pr;
    if (peer != "admin" && peer != "user") return std::make_pair(relay::errc::kInvalidLoginBody, std::string("peer_role 必须是 user 或 admin"));
    const bool want_admin = (peer == "admin");
    const auto want_mode = parse_recv_mode_str(*rm);
    if (!want_mode) return std::make_pair(relay::errc::kInvalidLoginBody, std::string("recv_mode 须为 broadcast 或 round_robin"));

    const std::string cip = self->client_ip();
    if (cip.empty() || !db_.ip_whitelisted(cip)) {
      return std::make_pair(relay::errc::kIpNotAllowed, std::string("IP 不在白名单"));
    }
    db::MysqlStore::UserInfo uinfo{};
    if (const auto e = db_.authenticate_or_register(*un, *pw, want_admin, uinfo)) {
      return std::make_pair(relay::errc::kAuthFailed, *e);
    }

    const std::uint64_t uid = uinfo.id;
    std::vector<std::pair<std::shared_ptr<TcpSession>, std::string>> to_notify;
    {
      std::lock_guard lk(mu_);
      uid_to_username_[uid] = *un;
      name_to_uid_[*un] = uid;

      const bool no_other_sessions =
          (user_deque_.find(uid) == user_deque_.end() || user_deque_[uid].empty());
      if (no_other_sessions) {
        user_recv_mode_[uid] = *want_mode;
        out_ok.effective_mode = *want_mode;
        out_ok.recv_mode_notice = std::nullopt;
      } else {
        const RecvMode established = user_recv_mode_[uid];
        out_ok.effective_mode = established;
        if (established != *want_mode) {
          out_ok.recv_mode_notice = std::string("接收方式按首次 login 为准；若需更改，请关闭该用户全部连接后重新登录");
        } else {
          out_ok.recv_mode_notice = std::nullopt;
        }
      }

      auto& dq = user_deque_[uid];
      dq.push_back(self);
      while (dq.size() > static_cast<std::size_t>(cfg_.session.max_connections_per_user)) {
        auto victim = dq.front();
        dq.pop_front();
        to_notify.push_back({victim, "因单用户连接数达到上限，服务端按策略释放了最旧的一条连接"});
      }
      self->mark_login(uid, uinfo.is_admin, *un);
    }
    for (const auto& pr2 : to_notify) {
      if (pr2.first && pr2.first != self) notify_and_close(pr2.first, pr2.second, "max_connections_per_user");
    }
    return std::nullopt;
  }

  // 在锁外先查库并写入 name_to_uid_（在 deliver 中调用）
  std::optional<std::uint64_t> ensure_uid_for_username(const std::string& username) {
    {
      std::lock_guard lk(mu_);
      auto it = name_to_uid_.find(username);
      if (it != name_to_uid_.end()) return it->second;
    }
    const auto id = db_.user_id_by_username(username);
    if (!id) return std::nullopt;
    std::lock_guard lk(mu_);
    name_to_uid_[username] = *id;
    return *id;
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

  void deliver_to_user_by_recv_mode(const std::string& src_username, std::uint64_t src_conn_id, const std::string& dst_username,
                                    const std::string& payload, std::uint32_t seq) {
    const auto dst_uid_opt = ensure_uid_for_username(dst_username);
    if (!dst_uid_opt) return;
    const std::uint64_t dst_uid = *dst_uid_opt;
    std::lock_guard lk(mu_);
    auto it = user_deque_.find(dst_uid);
    if (it == user_deque_.end() || it->second.empty()) return;
    RecvMode mode = RecvMode::kBroadcast;
    if (const auto m = user_recv_mode_.find(dst_uid); m != user_recv_mode_.end()) mode = m->second;

    std::vector<std::shared_ptr<TcpSession>> open;
    open.reserve(it->second.size());
    for (const auto& s : it->second) {
      if (s && s->is_open()) open.push_back(s);
    }
    if (open.empty()) return;

    if (mode == RecvMode::kBroadcast) {
      int sent = 0;
      const int cap = cfg_.session.broadcast_max_recipients;
      for (const auto& s : open) {
        if (sent >= cap) {
          push_event(fwd::log::Level::kWarn, "broadcast truncated at max_recipients");
          break;
        }
        const auto body = pack_deliver_envelope(payload, src_conn_id, s->conn_id(), src_username, dst_username);
        s->send_frame(pack_wire(relay::kMsgDeliver, seq, body));
        ++sent;
      }
    } else {
      std::size_t& cur = rr_next_index_[dst_uid];
      const std::size_t pick = (cur++) % open.size();
      auto s = open[pick];
      const auto body = pack_deliver_envelope(payload, src_conn_id, s->conn_id(), src_username, dst_username);
      s->send_frame(pack_wire(relay::kMsgDeliver, seq, body));
    }
  }

  void admin_kick_uid_sessions(std::uint64_t uid, const std::string& user_reason, const std::string& log_tag) {
    std::vector<std::shared_ptr<TcpSession>> victims;
    {
      std::lock_guard lk(mu_);
      auto it = user_deque_.find(uid);
      if (it != user_deque_.end()) victims.assign(it->second.begin(), it->second.end());
    }
    for (const auto& v : victims) {
      if (v) notify_and_close(v, user_reason, log_tag);
    }
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
  std::unordered_map<std::uint64_t, std::string> uid_to_username_;
  std::unordered_map<std::string, std::uint64_t> name_to_uid_;
  std::unordered_map<std::uint64_t, RecvMode> user_recv_mode_;
  std::unordered_map<std::uint64_t, std::size_t> rr_next_index_;

  fwd::db::MysqlStore db_{};

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

// GCC：Boost.Asio awaitable 协程帧的分配方式会触发 -Wmismatched-new-delete 误报（协程结束时析构帧）。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
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
    LoginOk lok{};
    if (const auto fe = try_authenticate(from, mp, lok)) {
      send_err_reply(from, wire_h.seq, fe->first, fe->second);
      co_return;
    }
    const std::string peer_str = from->is_admin_account() ? "admin" : "user";
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    if (lok.recv_mode_notice) {
      pk.pack_map(8);
    } else {
      pk.pack_map(7);
    }
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
    pk.pack("recv_mode");
    pk.pack(recv_mode_cstr(lok.effective_mode));
    if (lok.recv_mode_notice) {
      pk.pack("recv_mode_notice");
      pk.pack(*lok.recv_mode_notice);
    }
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
    const auto* pay_obj = map_get_obj(mp, "payload");
    auto payload = extract_payload_bin(pay_obj);
    if (!payload) {
      send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("DATA 缺少 payload（bin 或 str）"));
      co_return;
    }
    const auto dst_username = map_get_str(mp, "dst_username");
    if (!dst_username || dst_username->empty()) {
      send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("DATA 需要 dst_username"));
      co_return;
    }
    const std::uint64_t src_c = from->conn_id();
    const std::string& src_un = from->username();
    deliver_to_user_by_recv_mode(src_un, src_c, *dst_username, *payload, wire_h.seq);
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
      send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("需要管理员账号权限"));
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
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("kick_user 需要 target_user_id"));
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
    if (action == "allowlist_list") {
      std::vector<fwd::db::MysqlStore::AdminIpRow> rows;
      if (const auto err = db_.admin_list_allowlist(rows)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(3);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      pk.pack("rows");
      pk.pack_array(rows.size());
      for (const auto& r : rows) {
        pk.pack_map(2);
        pk.pack("id");
        pk.pack(r.id);
        pk.pack("ip");
        pk.pack(r.ip);
      }
      send_reply(from, buf, wire_h.seq);
      push_event(fwd::log::Level::kInfo, "admin allowlist_list by " + from->username());
      co_return;
    }
    if (action == "allowlist_add") {
      const auto ip = map_get_str(mp, "ip");
      if (!ip || ip->empty()) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("allowlist_add 需要 ip"));
        co_return;
      }
      if (const auto err = db_.admin_insert_allowlist(*ip)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      send_reply(from, buf, wire_h.seq);
      push_event(fwd::log::Level::kInfo, "admin allowlist_add ip=" + *ip + " by " + from->username());
      co_return;
    }
    if (action == "allowlist_update") {
      const auto rid = map_get_u64(mp, "id");
      const auto ip = map_get_str(mp, "ip");
      if (!rid || !ip) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("allowlist_update 需要 id 与 ip"));
        co_return;
      }
      if (const auto err = db_.admin_update_allowlist(*rid, *ip)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    if (action == "allowlist_delete") {
      const auto rid = map_get_u64(mp, "id");
      if (!rid) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("allowlist_delete 需要 id"));
        co_return;
      }
      if (const auto err = db_.admin_delete_allowlist(*rid)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    if (action == "user_table_list") {
      std::vector<fwd::db::MysqlStore::AdminUserRow> rows;
      if (const auto err = db_.admin_list_users(rows)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(3);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      pk.pack("rows");
      pk.pack_array(rows.size());
      for (const auto& r : rows) {
        pk.pack_map(3);
        pk.pack("id");
        pk.pack(r.id);
        pk.pack("username");
        pk.pack(r.username);
        pk.pack("is_admin");
        pk.pack(r.is_admin);
      }
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    if (action == "user_table_add") {
      const auto un = map_get_str(mp, "username");
      const auto pw = map_get_str(mp, "password");
      if (!un || !pw || un->empty() || pw->empty()) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("user_table_add 需要 username/password"));
        co_return;
      }
      const bool adm = map_get_bool(mp, "is_admin", false);
      if (const auto err = db_.admin_insert_user(*un, *pw, adm)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      send_reply(from, buf, wire_h.seq);
      push_event(fwd::log::Level::kInfo, "admin user_table_add " + *un + " by " + from->username());
      co_return;
    }
    if (action == "user_table_update") {
      const auto uid = map_get_u64(mp, "id");
      if (!uid) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("user_table_update 需要 id"));
        co_return;
      }
      std::string old_un;
      if (const auto err = db_.admin_get_username_by_id(*uid, old_un)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      const std::string* nu_ptr = nullptr;
      std::string nu_store;
      if (map_has_key(mp, "username")) {
        const auto v = map_get_str(mp, "username");
        if (!v) {
          send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("username 非法"));
          co_return;
        }
        nu_store = *v;
        nu_ptr = &nu_store;
      }
      const std::string* pw_ptr = nullptr;
      std::string pw_store;
      if (map_has_key(mp, "password")) {
        const auto v = map_get_str(mp, "password");
        if (!v) {
          send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("password 非法"));
          co_return;
        }
        pw_store = *v;
        pw_ptr = &pw_store;
      }
      const bool* ia_ptr = nullptr;
      bool ia_store = false;
      if (map_has_key(mp, "is_admin")) {
        ia_store = map_get_bool(mp, "is_admin", false);
        ia_ptr = &ia_store;
      }
      if (const auto err = db_.admin_update_user(*uid, nu_ptr, pw_ptr, ia_ptr)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      if (nu_ptr) {
        std::lock_guard lk(mu_);
        name_to_uid_.erase(old_un);
        name_to_uid_[*nu_ptr] = *uid;
        uid_to_username_[*uid] = *nu_ptr;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    if (action == "user_table_delete") {
      const auto uid = map_get_u64(mp, "id");
      if (!uid) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("user_table_delete 需要 id"));
        co_return;
      }
      std::string uname;
      if (const auto err = db_.admin_get_username_by_id(*uid, uname)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      admin_kick_uid_sessions(*uid, "管理员从库中删除该账号", "user_table_delete");
      {
        std::lock_guard lk(mu_);
        name_to_uid_.erase(uname);
        uid_to_username_.erase(*uid);
      }
      if (const auto err = db_.admin_delete_user(*uid)) {
        send_err_reply(from, wire_h.seq, relay::errc::kProtocol, *err);
        co_return;
      }
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("op");
      pk.pack("CONTROL");
      send_reply(from, buf, wire_h.seq);
      push_event(fwd::log::Level::kInfo, "admin user_table_delete id=" + std::to_string(*uid) + " by " + from->username());
      co_return;
    }
    send_err_reply(from, wire_h.seq, relay::errc::kProtocol, std::string("unknown action"));
    co_return;
  }

  send_err_reply(from, wire_h.seq, relay::errc::kProtocol,
                 std::string("未知 msg_type（已登录帧须为 heartbeat=2 / control=3 / data=4）"));
  co_return;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}  // namespace fwd

namespace fwd::db {

MysqlStore::~MysqlStore() { close(); }

void MysqlStore::close() {
  std::lock_guard lk(mu_);
  if (conn_) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
}

void MysqlStore::connect(const Config::Mysql& c) {
  std::lock_guard lk(mu_);
  if (conn_) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  cfg_ = c;
  MYSQL* raw = mysql_init(nullptr);
  if (!raw) throw std::runtime_error("mysql_init failed");
  (void)mysql_options(raw, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!mysql_real_connect(raw, c.host.c_str(), c.user.c_str(), c.password.c_str(), c.database.c_str(),
                          static_cast<unsigned int>(c.port), nullptr, 0)) {
    const std::string err = mysql_error(raw);
    mysql_close(raw);
    throw std::runtime_error("mysql_real_connect: " + err);
  }
  conn_ = raw;
}

bool MysqlStore::ip_whitelisted(const std::string& client_ip) {
  std::lock_guard lk(mu_);
  if (!conn_) return false;
  static const char q[] = "SELECT 1 FROM ip_allowlist WHERE ip = ? LIMIT 1";
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return false;
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    mysql_stmt_close(st);
    return false;
  }
  char buf[256]{};
  const unsigned long len = static_cast<unsigned long>(std::min(client_ip.size(), sizeof(buf) - 1));
  std::memcpy(buf, client_ip.data(), len);
  MYSQL_BIND bind{};
  unsigned long plen = len;
  bind.buffer_type = MYSQL_TYPE_STRING;
  bind.buffer = buf;
  bind.buffer_length = sizeof(buf);
  bind.length = &plen;
  if (mysql_stmt_bind_param(st, &bind) != 0) {
    mysql_stmt_close(st);
    return false;
  }
  if (mysql_stmt_execute(st) != 0) {
    mysql_stmt_close(st);
    return false;
  }
  (void)mysql_stmt_store_result(st);
  const my_ulonglong n = mysql_stmt_num_rows(st);
  mysql_stmt_close(st);
  return n > 0;
}

std::optional<std::string> MysqlStore::authenticate_or_register(const std::string& username, const std::string& password,
                                                                bool want_admin, UserInfo& out) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");

  static const char sel[] = "SELECT id, password, is_admin FROM users WHERE username = ? LIMIT 1";
  static const char ins[] = "INSERT INTO users (username, password, is_admin) VALUES (?, ?, ?)";

  for (int attempt = 0; attempt < 4; ++attempt) {
    MYSQL_STMT* st = mysql_stmt_init(conn_);
    if (!st) return std::string("mysql_stmt_init");
    if (mysql_stmt_prepare(st, sel, static_cast<unsigned long>(sizeof(sel) - 1)) != 0) {
      const std::string e = mysql_stmt_error(st);
      mysql_stmt_close(st);
      return e;
    }
    char uname[512]{};
    const unsigned long ulen = static_cast<unsigned long>(std::min(username.size(), sizeof(uname) - 1));
    std::memcpy(uname, username.data(), ulen);
    MYSQL_BIND pbind{};
    unsigned long ulen_b = ulen;
    pbind.buffer_type = MYSQL_TYPE_STRING;
    pbind.buffer = uname;
    pbind.buffer_length = sizeof(uname);
    pbind.length = &ulen_b;
    if (mysql_stmt_bind_param(st, &pbind) != 0) {
      mysql_stmt_close(st);
      return std::string("bind param");
    }
    my_ulonglong idv = 0;
    char pwdb[4096]{};
    unsigned long pwdlen = 0;
    std::int8_t isadmin = 0;
    bool n0 = false, n1 = false, n2 = false;
    MYSQL_BIND rb[3]{};
    rb[0].buffer_type = MYSQL_TYPE_LONGLONG;
    rb[0].buffer = &idv;
    rb[0].is_unsigned = true;
    rb[0].is_null = &n0;
    rb[1].buffer_type = MYSQL_TYPE_STRING;
    rb[1].buffer = pwdb;
    rb[1].buffer_length = sizeof(pwdb);
    rb[1].length = &pwdlen;
    rb[1].is_null = &n1;
    rb[2].buffer_type = MYSQL_TYPE_TINY;
    rb[2].buffer = &isadmin;
    rb[2].is_null = &n2;
    if (mysql_stmt_bind_result(st, rb) != 0) {
      mysql_stmt_close(st);
      return std::string("bind result");
    }
    if (mysql_stmt_execute(st) != 0) {
      const std::string e = mysql_stmt_error(st);
      mysql_stmt_close(st);
      return e;
    }
    const int f = mysql_stmt_fetch(st);
    if (f == 0) {
      mysql_stmt_close(st);
      if (n0 || n1) return std::string("数据异常");
      const std::string got_pw(pwdb, pwdlen);
      if (got_pw != password) return std::string("用户不存在或密码错误");
      const bool db_ad = (!n2 && isadmin != 0);
      if (db_ad != want_admin) return std::string("登录所选权限与账号不一致");
      out.id = static_cast<std::uint64_t>(idv);
      out.is_admin = db_ad;
      return std::nullopt;
    }
    if (f == MYSQL_NO_DATA) {
      mysql_stmt_close(st);
      MYSQL_STMT* inst = mysql_stmt_init(conn_);
      if (!inst) return std::string("mysql_stmt_init");
      if (mysql_stmt_prepare(inst, ins, static_cast<unsigned long>(sizeof(ins) - 1)) != 0) {
        const std::string e = mysql_stmt_error(inst);
        mysql_stmt_close(inst);
        return e;
      }
      char uname2[512]{};
      std::memcpy(uname2, uname, ulen);
      uname2[ulen] = '\0';
      char pwbuf[4096]{};
      const unsigned long pwcopy =
          static_cast<unsigned long>(std::min(password.size(), sizeof(pwbuf) - 1));
      std::memcpy(pwbuf, password.data(), pwcopy);
      std::int8_t ad = want_admin ? 1 : 0;
      MYSQL_BIND ib[3]{};
      unsigned long un2 = ulen;
      unsigned long pwl = pwcopy;
      ib[0].buffer_type = MYSQL_TYPE_STRING;
      ib[0].buffer = uname2;
      ib[0].buffer_length = sizeof(uname2);
      ib[0].length = &un2;
      ib[1].buffer_type = MYSQL_TYPE_STRING;
      ib[1].buffer = pwbuf;
      ib[1].buffer_length = sizeof(pwbuf);
      ib[1].length = &pwl;
      ib[2].buffer_type = MYSQL_TYPE_TINY;
      ib[2].buffer = &ad;
      if (mysql_stmt_bind_param(inst, ib) != 0) {
        mysql_stmt_close(inst);
        return std::string("bind insert param");
      }
      if (mysql_stmt_execute(inst) != 0) {
        const unsigned int en = mysql_errno(conn_);
        mysql_stmt_close(inst);
        if (en == 1062) {
          continue;
        }
        return mysql_error(conn_);
      }
      mysql_stmt_close(inst);
      out.id = static_cast<std::uint64_t>(mysql_insert_id(conn_));
      out.is_admin = want_admin;
      return std::nullopt;
    }
    mysql_stmt_close(st);
    return std::string("读用户失败");
  }
  return std::string("注册冲突，请稍后重试");
}

std::optional<std::uint64_t> MysqlStore::user_id_by_username(const std::string& username) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::nullopt;
  static const char q[] = "SELECT id FROM users WHERE username = ? LIMIT 1";
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::nullopt;
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    mysql_stmt_close(st);
    return std::nullopt;
  }
  char uname[512]{};
  const unsigned long ulen = static_cast<unsigned long>(std::min(username.size(), sizeof(uname) - 1));
  std::memcpy(uname, username.data(), ulen);
  MYSQL_BIND pbind{};
  unsigned long ulen_b = ulen;
  pbind.buffer_type = MYSQL_TYPE_STRING;
  pbind.buffer = uname;
  pbind.buffer_length = sizeof(uname);
  pbind.length = &ulen_b;
  if (mysql_stmt_bind_param(st, &pbind) != 0) {
    mysql_stmt_close(st);
    return std::nullopt;
  }
  my_ulonglong idv = 0;
  bool n0 = false;
  MYSQL_BIND rb{};
  rb.buffer_type = MYSQL_TYPE_LONGLONG;
  rb.buffer = &idv;
  rb.is_unsigned = true;
  rb.is_null = &n0;
  if (mysql_stmt_bind_result(st, &rb) != 0) {
    mysql_stmt_close(st);
    return std::nullopt;
  }
  if (mysql_stmt_execute(st) != 0) {
    mysql_stmt_close(st);
    return std::nullopt;
  }
  if (mysql_stmt_fetch(st) != 0) {
    mysql_stmt_close(st);
    return std::nullopt;
  }
  mysql_stmt_close(st);
  if (n0) return std::nullopt;
  return static_cast<std::uint64_t>(idv);
}

std::optional<std::string> MysqlStore::admin_list_allowlist(std::vector<AdminIpRow>& out) {
  std::lock_guard lk(mu_);
  out.clear();
  if (!conn_) return std::string("数据库未连接");
  const char q[] = "SELECT id, ip FROM ip_allowlist ORDER BY id ASC";
  if (mysql_real_query(conn_, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) return std::string(mysql_error(conn_));
  MYSQL_RES* res = mysql_store_result(conn_);
  if (!res) return std::string(mysql_error(conn_));
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    if (!row[0] || !row[1]) continue;
    AdminIpRow r;
    try {
      r.id = std::stoull(row[0]);
    } catch (...) {
      continue;
    }
    r.ip = row[1];
    out.push_back(std::move(r));
  }
  mysql_free_result(res);
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_insert_allowlist(const std::string& ip) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::string("mysql_stmt_init");
  static const char q[] = "INSERT INTO ip_allowlist (ip) VALUES (?)";
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  char buf[256]{};
  const unsigned long plen = static_cast<unsigned long>(std::min(ip.size(), sizeof(buf) - 1));
  std::memcpy(buf, ip.data(), plen);
  MYSQL_BIND bind{};
  unsigned long len = plen;
  bind.buffer_type = MYSQL_TYPE_STRING;
  bind.buffer = buf;
  bind.buffer_length = sizeof(buf);
  bind.length = &len;
  if (mysql_stmt_bind_param(st, &bind) != 0) {
    mysql_stmt_close(st);
    return std::string("bind");
  }
  if (mysql_stmt_execute(st) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    if (e.find("Duplicate") != std::string::npos || e.find("1062") != std::string::npos) return std::string("IP 已存在");
    return e;
  }
  mysql_stmt_close(st);
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_update_allowlist(std::uint64_t id, const std::string& ip) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::string("mysql_stmt_init");
  static const char q[] = "UPDATE ip_allowlist SET ip = ? WHERE id = ?";
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  char buf[256]{};
  const unsigned long plen = static_cast<unsigned long>(std::min(ip.size(), sizeof(buf) - 1));
  std::memcpy(buf, ip.data(), plen);
  MYSQL_BIND b[2]{};
  unsigned long l0 = plen;
  b[0].buffer_type = MYSQL_TYPE_STRING;
  b[0].buffer = buf;
  b[0].buffer_length = sizeof(buf);
  b[0].length = &l0;
  b[1].buffer_type = MYSQL_TYPE_LONGLONG;
  b[1].buffer = &id;
  b[1].is_unsigned = true;
  if (mysql_stmt_bind_param(st, b) != 0) {
    mysql_stmt_close(st);
    return std::string("bind");
  }
  if (mysql_stmt_execute(st) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    if (e.find("Duplicate") != std::string::npos || e.find("1062") != std::string::npos) return std::string("IP 与已有记录冲突");
    return e;
  }
  const auto aff = mysql_stmt_affected_rows(st);
  mysql_stmt_close(st);
  if (aff == 0) return std::string("无此 id");
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_delete_allowlist(std::uint64_t id) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::string("mysql_stmt_init");
  static const char q[] = "DELETE FROM ip_allowlist WHERE id = ?";
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  MYSQL_BIND b{};
  b.buffer_type = MYSQL_TYPE_LONGLONG;
  b.buffer = &id;
  b.is_unsigned = true;
  if (mysql_stmt_bind_param(st, &b) != 0) {
    mysql_stmt_close(st);
    return std::string("bind");
  }
  if (mysql_stmt_execute(st) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  const auto aff = mysql_stmt_affected_rows(st);
  mysql_stmt_close(st);
  if (aff == 0) return std::string("无此 id");
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_list_users(std::vector<AdminUserRow>& out) {
  std::lock_guard lk(mu_);
  out.clear();
  if (!conn_) return std::string("数据库未连接");
  const char q[] = "SELECT id, username, is_admin FROM users ORDER BY id ASC";
  if (mysql_real_query(conn_, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) return std::string(mysql_error(conn_));
  MYSQL_RES* res = mysql_store_result(conn_);
  if (!res) return std::string(mysql_error(conn_));
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    if (!row[0] || !row[1] || !row[2]) continue;
    AdminUserRow r;
    try {
      r.id = std::stoull(row[0]);
    } catch (...) {
      continue;
    }
    r.username = row[1];
    r.is_admin = (std::strcmp(row[2], "1") == 0);
    out.push_back(std::move(r));
  }
  mysql_free_result(res);
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_insert_user(const std::string& username, const std::string& password, bool is_admin) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::string("mysql_stmt_init");
  static const char q[] = "INSERT INTO users (username, password, is_admin) VALUES (?, ?, ?)";
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  char un[512]{};
  char pw[4096]{};
  const unsigned long ulen = static_cast<unsigned long>(std::min(username.size(), sizeof(un) - 1));
  const unsigned long plen = static_cast<unsigned long>(std::min(password.size(), sizeof(pw) - 1));
  std::memcpy(un, username.data(), ulen);
  std::memcpy(pw, password.data(), plen);
  MYSQL_BIND b[3]{};
  unsigned long l0 = ulen, l1 = plen;
  b[0].buffer_type = MYSQL_TYPE_STRING;
  b[0].buffer = un;
  b[0].buffer_length = sizeof(un);
  b[0].length = &l0;
  b[1].buffer_type = MYSQL_TYPE_STRING;
  b[1].buffer = pw;
  b[1].buffer_length = sizeof(pw);
  b[1].length = &l1;
  int8_t adm = static_cast<int8_t>(is_admin ? 1 : 0);
  b[2].buffer_type = MYSQL_TYPE_TINY;
  b[2].buffer = &adm;
  if (mysql_stmt_bind_param(st, b) != 0) {
    mysql_stmt_close(st);
    return std::string("bind");
  }
  if (mysql_stmt_execute(st) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    if (e.find("Duplicate") != std::string::npos || e.find("1062") != std::string::npos) return std::string("用户名已存在");
    return e;
  }
  mysql_stmt_close(st);
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_update_user(std::uint64_t id, const std::string* new_username, const std::string* new_password,
                                                         const bool* new_is_admin) {
  if (!new_username && !new_password && !new_is_admin) return std::string("无字段可更新");
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");
  auto run_one = [&](const char* sql, auto&& bind_fn) -> std::optional<std::string> {
    MYSQL_STMT* st = mysql_stmt_init(conn_);
    if (!st) return std::string("mysql_stmt_init");
    if (mysql_stmt_prepare(st, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
      std::string e = mysql_stmt_error(st);
      mysql_stmt_close(st);
      return e;
    }
    if (bind_fn(st) != 0) {
      mysql_stmt_close(st);
      return std::string("bind");
    }
    if (mysql_stmt_execute(st) != 0) {
      std::string e = mysql_stmt_error(st);
      mysql_stmt_close(st);
      if (e.find("Duplicate") != std::string::npos || e.find("1062") != std::string::npos) return std::string("用户名冲突");
      return e;
    }
    if (mysql_stmt_affected_rows(st) == 0) {
      mysql_stmt_close(st);
      return std::string("无此 id");
    }
    mysql_stmt_close(st);
    return std::nullopt;
  };
  if (new_username) {
    static const char q[] = "UPDATE users SET username = ? WHERE id = ?";
    char un[512]{};
    const unsigned long ulen = static_cast<unsigned long>(std::min(new_username->size(), sizeof(un) - 1));
    std::memcpy(un, new_username->data(), ulen);
    unsigned long l0 = ulen;
    std::uint64_t idc = id;
    if (const auto e =
            run_one(q, [&](MYSQL_STMT* st) {
              MYSQL_BIND b[2]{};
              b[0].buffer_type = MYSQL_TYPE_STRING;
              b[0].buffer = un;
              b[0].buffer_length = sizeof(un);
              b[0].length = &l0;
              b[1].buffer_type = MYSQL_TYPE_LONGLONG;
              b[1].buffer = &idc;
              b[1].is_unsigned = true;
              return mysql_stmt_bind_param(st, b);
            })) {
      return e;
    }
  }
  if (new_password) {
    static const char q[] = "UPDATE users SET password = ? WHERE id = ?";
    char pw[4096]{};
    const unsigned long plen = static_cast<unsigned long>(std::min(new_password->size(), sizeof(pw) - 1));
    std::memcpy(pw, new_password->data(), plen);
    unsigned long l1 = plen;
    std::uint64_t idc = id;
    if (const auto e =
            run_one(q, [&](MYSQL_STMT* st) {
              MYSQL_BIND b[2]{};
              b[0].buffer_type = MYSQL_TYPE_STRING;
              b[0].buffer = pw;
              b[0].buffer_length = sizeof(pw);
              b[0].length = &l1;
              b[1].buffer_type = MYSQL_TYPE_LONGLONG;
              b[1].buffer = &idc;
              b[1].is_unsigned = true;
              return mysql_stmt_bind_param(st, b);
            })) {
      return e;
    }
  }
  if (new_is_admin) {
    static const char q[] = "UPDATE users SET is_admin = ? WHERE id = ?";
    int8_t adm = static_cast<int8_t>(*new_is_admin ? 1 : 0);
    std::uint64_t idc = id;
    if (const auto e = run_one(q, [&](MYSQL_STMT* st) {
          MYSQL_BIND b[2]{};
          b[0].buffer_type = MYSQL_TYPE_TINY;
          b[0].buffer = &adm;
          b[1].buffer_type = MYSQL_TYPE_LONGLONG;
          b[1].buffer = &idc;
          b[1].is_unsigned = true;
          return mysql_stmt_bind_param(st, b);
        })) {
      return e;
    }
  }
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_delete_user(std::uint64_t id) {
  std::lock_guard lk(mu_);
  if (!conn_) return std::string("数据库未连接");
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::string("mysql_stmt_init");
  static const char q[] = "DELETE FROM users WHERE id = ?";
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  MYSQL_BIND b{};
  b.buffer_type = MYSQL_TYPE_LONGLONG;
  b.buffer = &id;
  b.is_unsigned = true;
  if (mysql_stmt_bind_param(st, &b) != 0) {
    mysql_stmt_close(st);
    return std::string("bind");
  }
  if (mysql_stmt_execute(st) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  const auto aff = mysql_stmt_affected_rows(st);
  mysql_stmt_close(st);
  if (aff == 0) return std::string("无此 id");
  return std::nullopt;
}

std::optional<std::string> MysqlStore::admin_get_username_by_id(std::uint64_t id, std::string& out_name) {
  std::lock_guard lk(mu_);
  out_name.clear();
  if (!conn_) return std::string("数据库未连接");
  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return std::string("mysql_stmt_init");
  static const char q[] = "SELECT username FROM users WHERE id = ? LIMIT 1";
  if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(sizeof(q) - 1)) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  MYSQL_BIND pb{};
  pb.buffer_type = MYSQL_TYPE_LONGLONG;
  pb.buffer = &id;
  pb.is_unsigned = true;
  if (mysql_stmt_bind_param(st, &pb) != 0) {
    mysql_stmt_close(st);
    return std::string("bind");
  }
  char un[512]{};
  unsigned long ulen = 0;
  MYSQL_BIND rb{};
  rb.buffer_type = MYSQL_TYPE_STRING;
  rb.buffer = un;
  rb.buffer_length = sizeof(un);
  rb.length = &ulen;
  if (mysql_stmt_bind_result(st, &rb) != 0) {
    mysql_stmt_close(st);
    return std::string("bind result");
  }
  if (mysql_stmt_execute(st) != 0) {
    std::string e = mysql_stmt_error(st);
    mysql_stmt_close(st);
    return e;
  }
  if (mysql_stmt_fetch(st) != 0) {
    mysql_stmt_close(st);
    return std::string("无此用户");
  }
  out_name.assign(un, ulen);
  mysql_stmt_close(st);
  return std::nullopt;
}

}  // namespace fwd::db

int main(int argc, char** argv) {
  try {
    std::string cfg_path = "deliver/server/forwarder.json";
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
