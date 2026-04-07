// 对等客户端中继：单 TCP 接入端口 + Msgpack Body 状态机（LOGIN / HEARTBEAT / TRANSFER / CONTROL）
// + admin HTTP。无“上游/下游”之分。

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
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fwd/config.hpp"
#include "fwd/log.hpp"
#include "fwd/protocol.hpp"
#include "fwd/relay_constants.hpp"

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
  bool is_control() const { return is_control_; }
  bool is_open() const { return socket_.is_open(); }
  std::size_t pending_bytes() const { return pending_bytes_; }

  void touch_heartbeat() { last_hb_ = std::chrono::steady_clock::now(); }

  std::chrono::steady_clock::time_point last_hb_ts() const { return last_hb_; }

  void mark_login(std::uint64_t uid, bool control) {
    logged_in_ = true;
    user_id_ = uid;
    is_control_ = control;
    touch_heartbeat();
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
      if (self->stopped_) return;
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
  bool is_control_{false};
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

  awaitable<void> handle_client_frame(std::shared_ptr<TcpSession> from, const proto::Header& wire_h, std::string body);

 private:
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
        s->stop("heartbeat timeout");
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
    to->send_frame(pack_wire(relay::kMsgServerReply, 0, to->user_id(), seq, body));
  }

  void apply_login(const std::shared_ptr<TcpSession>& self, std::uint64_t uid, bool control) {
    std::vector<std::shared_ptr<TcpSession>> to_kick;
    {
      std::lock_guard lk(mu_);
      auto& dq = user_deque_[uid];
      if (control) {
        for (auto it = dq.begin(); it != dq.end();) {
          if ((*it)->is_control()) {
            to_kick.push_back(*it);
            it = dq.erase(it);
          } else {
            ++it;
          }
        }
      }
      dq.push_back(self);
      while (dq.size() > static_cast<std::size_t>(cfg_.session.max_connections_per_user)) {
        to_kick.push_back(dq.front());
        dq.pop_front();
      }
    }
    for (const auto& v : to_kick) {
      if (v && v != self) v->stop("replaced / max connections per user");
    }
    self->mark_login(uid, control);
  }

  std::shared_ptr<TcpSession> pick_preferred(std::uint64_t uid) {
    std::lock_guard lk(mu_);
    auto it = user_deque_.find(uid);
    if (it == user_deque_.end()) return nullptr;
    for (const auto& s : it->second) {
      if (s && s->is_open() && !s->is_control()) return s;
    }
    for (const auto& s : it->second) {
      if (s && s->is_open()) return s;
    }
    return nullptr;
  }

  std::set<std::uint64_t> other_user_ids(std::uint64_t exclude_uid) {
    std::set<std::uint64_t> out;
    std::lock_guard lk(mu_);
    for (const auto& kv : user_deque_) {
      if (kv.first == exclude_uid) continue;
      bool any = false;
      for (const auto& s : kv.second) {
        if (s && s->is_open()) {
          any = true;
          break;
        }
      }
      if (any) out.insert(kv.first);
    }
    return out;
  }

  void deliver_unicast(std::uint64_t src_uid, std::uint64_t dst_uid, const std::string& payload, std::uint32_t seq) {
    auto s = pick_preferred(dst_uid);
    if (!s) return;
    s->send_frame(pack_wire(relay::kMsgDeliver, src_uid, dst_uid, seq, payload));
  }

  void deliver_broadcast(std::uint64_t src_uid, const std::string& payload, std::uint32_t seq) {
    auto users = other_user_ids(src_uid);
    int sent = 0;
    const int cap = cfg_.session.broadcast_max_recipients;
    for (std::uint64_t uid : users) {
      if (sent >= cap) {
        push_event(fwd::log::Level::kWarn, "broadcast truncated at max_recipients");
        break;
      }
      auto s = pick_preferred(uid);
      if (s) {
        s->send_frame(pack_wire(relay::kMsgDeliver, src_uid, uid, seq, payload));
        ++sent;
      }
    }
  }

  void schedule_round_robin(std::uint64_t src_uid, const std::string& payload, std::uint32_t seq, int interval_ms) {
    auto users = other_user_ids(src_uid);
    std::vector<std::uint64_t> order(users.begin(), users.end());
    if (interval_ms <= 0) interval_ms = cfg_.session.round_robin_default_interval_ms;
    auto self = shared_from_this();
    co_spawn(
        io_,
        [self, order, payload, seq, src_uid, interval_ms]() -> awaitable<void> {
          for (std::size_t i = 0; i < order.size(); ++i) {
            const std::uint64_t uid = order[i];
            auto s = self->pick_preferred(uid);
            if (s) s->send_frame(self->pack_wire(relay::kMsgDeliver, src_uid, uid, seq, payload));
            if (i + 1 < order.size() && interval_ms > 0) {
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

  Metrics metrics_{};
  const std::uint64_t started_ms_{now_ms_wall()};
  std::mutex events_mu_;
  std::deque<AdminEvent> events_;
};

awaitable<void> TcpSession::read_loop(std::shared_ptr<RelayServer> hub) {
  boost::system::error_code ec;
  auto last_activity = std::chrono::steady_clock::now();
  while (true) {
    if (hub->cfg_.timeouts.idle_ms > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_activity > std::chrono::milliseconds(hub->cfg_.timeouts.idle_ms)) {
        stop("idle timeout");
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
  const auto op = map_get_str(mp, "op");
  if (!op) {
    from->stop("missing op");
    co_return;
  }

  if (!from->logged_in()) {
    if (*op != "LOGIN") {
      from->stop("LOGIN required");
      co_return;
    }
    const auto uid = map_get_u64(mp, "user_id");
    if (!uid) {
      from->stop("LOGIN missing user_id");
      co_return;
    }
    std::string role = map_get_str(mp, "role").value_or("data");
    if (role != "control" && role != "data") role = "data";
    apply_login(from, *uid, role == "control");
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_map(5);
    pk.pack("ok");
    pk.pack(true);
    pk.pack("op");
    pk.pack("LOGIN");
    pk.pack("conn_id");
    pk.pack(from->conn_id());
    pk.pack("user_id");
    pk.pack(*uid);
    pk.pack("role");
    pk.pack(role);
    send_reply(from, buf, wire_h.seq);
    co_return;
  }

  from->touch_heartbeat();

  if (*op == "HEARTBEAT") {
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

  if (*op == "TRANSFER") {
    const auto mode = map_get_str(mp, "mode").value_or("unicast");
    const auto* pay_obj = map_get_obj(mp, "payload");
    auto payload = extract_payload_bin(pay_obj);
    if (!payload) {
      msgpack::sbuffer eb;
      msgpack::packer<msgpack::sbuffer> pk(&eb);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(false);
      pk.pack("error");
      pk.pack("TRANSFER missing payload (bin or str)");
      send_reply(from, eb, wire_h.seq);
      co_return;
    }
    std::uint64_t dst = map_get_u64(mp, "dst_user_id").value_or(wire_h.dst_user_id);
    int interval_ms = static_cast<int>(map_get_u64(mp, "interval_ms").value_or(0));
    if (mode == "unicast") {
      if (dst == 0) {
        msgpack::sbuffer eb;
        msgpack::packer<msgpack::sbuffer> pk(&eb);
        pk.pack_map(2);
        pk.pack("ok");
        pk.pack(false);
        pk.pack("error");
        pk.pack("unicast needs dst_user_id");
        send_reply(from, eb, wire_h.seq);
        co_return;
      }
      deliver_unicast(from->user_id(), dst, *payload, wire_h.seq);
    } else if (mode == "broadcast") {
      deliver_broadcast(from->user_id(), *payload, wire_h.seq);
    } else if (mode == "round_robin" || mode == "roundrobin") {
      schedule_round_robin(from->user_id(), *payload, wire_h.seq, interval_ms);
    } else {
      msgpack::sbuffer eb;
      msgpack::packer<msgpack::sbuffer> pk(&eb);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(false);
      pk.pack("error");
      pk.pack("unknown mode");
      send_reply(from, eb, wire_h.seq);
      co_return;
    }
    msgpack::sbuffer ack;
    msgpack::packer<msgpack::sbuffer> pk(&ack);
    pk.pack_map(2);
    pk.pack("ok");
    pk.pack(true);
    pk.pack("op");
    pk.pack("TRANSFER");
    send_reply(from, ack, wire_h.seq);
    co_return;
  }

  if (*op == "CONTROL") {
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
        pk.pack_map(2);
        pk.pack("user_id");
        pk.pack(kv.first);
        pk.pack("connections");
        pk.pack(static_cast<unsigned>(kv.second.size()));
      }
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    if (action == "kick_user") {
      const auto target = map_get_u64(mp, "target_user_id");
      if (!target) {
        msgpack::sbuffer eb;
        msgpack::packer<msgpack::sbuffer> pk(&eb);
        pk.pack_map(2);
        pk.pack("ok");
        pk.pack(false);
        pk.pack("error");
        pk.pack("kick_user needs target_user_id");
        send_reply(from, eb, wire_h.seq);
        co_return;
      }
      std::vector<std::shared_ptr<TcpSession>> victims;
      {
        std::lock_guard lk(mu_);
        auto it = user_deque_.find(*target);
        if (it != user_deque_.end()) {
          victims.assign(it->second.begin(), it->second.end());
          user_deque_.erase(it);
        }
      }
      for (const auto& v : victims) v->stop("kicked by CONTROL");
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("ok");
      pk.pack(true);
      pk.pack("kicked_count");
      pk.pack(static_cast<unsigned>(victims.size()));
      send_reply(from, buf, wire_h.seq);
      co_return;
    }
    msgpack::sbuffer eb;
    msgpack::packer<msgpack::sbuffer> pk(&eb);
    pk.pack_map(2);
    pk.pack("ok");
    pk.pack(false);
    pk.pack("error");
    pk.pack("unknown action");
    send_reply(from, eb, wire_h.seq);
    co_return;
  }

  msgpack::sbuffer eb;
  msgpack::packer<msgpack::sbuffer> pk(&eb);
  pk.pack_map(2);
  pk.pack("ok");
  pk.pack(false);
  pk.pack("error");
  pk.pack("unknown op");
  send_reply(from, eb, wire_h.seq);
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
