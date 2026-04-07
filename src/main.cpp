// 本文件结构（建议从这里开始读）：
// - 配置加载：load_config_or_throw() 读取 JSON 配置并做边界校验
// - 协议：proto::Header 的 to_string()（打日志用），以及后续 unpack/pack 的使用点
// - DownstreamSession：单个下游连接的发送队列与背压控制（high_water/hard_limit）
// - Forwarder：
//   - start()：启动三类监听（upstream/downstream/admin）+ metrics 定时器
//   - accept_upstream()/accept_downstream()：接受连接（上游为单连接槽位）
//   - upstream_readloop()：按 “Header(40)+Body(body_len)” 读帧，校验后广播给所有下游
//   - broadcast()：将同一份 frame（shared_ptr<string>）分发给所有下游，避免 N 份复制
//   - admin 接口：/api/stats、/api/events（供 Web sidecar 大屏/SSE 使用）
// - main()：读取配置路径，启动 io_context 线程池

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
#include <utility>
#include <vector>

#include "fwd/config.hpp"
#include "fwd/log.hpp"
#include "fwd/protocol.hpp"

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::ip::tcp;
using boost::asio::use_awaitable;
namespace beast = boost::beast;
namespace http = beast::http;

namespace fwd {

// ===== protocol impl =====
std::string proto::Header::to_string() const {
  return "magic=" + std::to_string(magic) + " ver=" + std::to_string(version) +
         " hlen=" + std::to_string(header_len) + " blen=" + std::to_string(body_len) +
         " type=" + std::to_string(msg_type) + " flags=" + std::to_string(flags) +
         " seq=" + std::to_string(seq) + " src_user=" + std::to_string(src_user_id) +
         " dst_user=" + std::to_string(dst_user_id);
}

// ===== config impl =====
static std::uint16_t as_u16(int v, const char* name) {
  if (v < 1 || v > 65535) throw std::runtime_error(std::string("invalid ") + name);
  return static_cast<std::uint16_t>(v);
}

Config load_config_or_throw(const std::string& path) {
  boost::property_tree::ptree pt;
  boost::property_tree::read_json(path, pt);

  Config cfg;
  cfg.upstream_listen.host = pt.get<std::string>("upstream.listen.host", cfg.upstream_listen.host);
  cfg.upstream_listen.port = as_u16(pt.get<int>("upstream.listen.port", cfg.upstream_listen.port), "upstream.listen.port");
  cfg.upstream_listen.backlog = pt.get<int>("upstream.listen.backlog", cfg.upstream_listen.backlog);

  cfg.downstream_listen.host = pt.get<std::string>("downstream.listen.host", cfg.downstream_listen.host);
  cfg.downstream_listen.port =
      as_u16(pt.get<int>("downstream.listen.port", cfg.downstream_listen.port), "downstream.listen.port");
  cfg.downstream_listen.backlog = pt.get<int>("downstream.listen.backlog", cfg.downstream_listen.backlog);

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

  if (cfg.io_threads < 1 || cfg.io_threads > 64) throw std::runtime_error("invalid threads.io");
  if (cfg.biz_threads < 1 || cfg.biz_threads > 64) throw std::runtime_error("invalid threads.biz");
  if (cfg.admin.events_max < 0 || cfg.admin.events_max > 10000) throw std::runtime_error("invalid admin.events_max");
  // 单帧 body 上限：配置可调；允许到 1GiB（仍远小于 u32 理论值），超大包需配合内存与运维策略。
  constexpr std::uint32_t kMaxBodyLenCap = 1024u * 1024u * 1024u;
  if (cfg.limits.max_body_len == 0 || cfg.limits.max_body_len > kMaxBodyLenCap) throw std::runtime_error("invalid limits.max_body_len");
  if (cfg.flow.high_water_bytes == 0 || cfg.flow.hard_limit_bytes < cfg.flow.high_water_bytes) throw std::runtime_error("invalid flow thresholds");
  if (!(cfg.flow.on_high_water == "drop" || cfg.flow.on_high_water == "disconnect")) throw std::runtime_error("invalid flow.send_queue.on_high_water");
  if (cfg.timeouts.read_ms < 100 || cfg.timeouts.idle_ms < cfg.timeouts.read_ms) throw std::runtime_error("invalid timeouts");
  if (cfg.metrics.interval_ms < 200 || cfg.metrics.interval_ms > 60 * 60 * 1000) throw std::runtime_error("invalid metrics.interval_ms");

  return cfg;
}

// ===== forwarder =====
// Metrics 仅用于粗粒度可观测性（大屏 + admin 接口）。
// 这里刻意采用“无锁 + 近似”的实现（relaxed 原子），原因是：
// - 指标主要用于观察趋势，不要求强一致的精确值
// - 不希望在热路径上引入锁竞争
struct Metrics {
  std::atomic<std::uint64_t> upstream_connects{0};
  std::atomic<std::uint64_t> downstream_connects{0};
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

class DownstreamSession : public std::enable_shared_from_this<DownstreamSession> {
 public:
  DownstreamSession(tcp::socket sock, const Config& cfg, Metrics& m)
      : socket_(std::move(sock)),
        strand_(boost::asio::make_strand(socket_.get_executor())),
        cfg_(cfg),
        metrics_(m) {}

  void start() {
    fwd::log::write(fwd::log::Level::kInfo, "downstream connected: " + endpoint_str());
  }

  void stop(const std::string& why) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, why] {
      if (self->stopped_) return;
      self->stopped_ = true;
      boost::system::error_code ec;
      self->socket_.cancel(ec);
      self->socket_.close(ec);
      fwd::log::write(fwd::log::Level::kWarn, "downstream closed: " + why + " ep=" + self->endpoint_str());
    });
  }

  void send_frame(std::shared_ptr<std::string> bytes) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, bytes] {
      if (self->stopped_) return;

      // 背压 / 内存保护：
      // - 每个下游有自己的发送队列，因为下游速度可能各不相同（有人快、有人慢）。
      // - 用 pending_bytes_ 统计排队字节数，避免慢下游导致内存无上限增长。
      // - 策略：
      //   - 高水位（high_water）：开始对“这个下游”丢弃新消息或断开连接（不影响其他下游）
      //   - 硬上限（hard_limit）：一定断开，保护进程内存上限
      const auto new_total = self->pending_bytes_ + bytes->size();
      if (new_total > self->cfg_.flow.high_water_bytes && new_total <= self->cfg_.flow.hard_limit_bytes) {
        if (self->cfg_.flow.on_high_water == "disconnect") {
          self->metrics_.drops.fetch_add(1, std::memory_order_relaxed);
          self->stopped_ = true;
          boost::system::error_code ec;
          self->socket_.cancel(ec);
          self->socket_.close(ec);
          fwd::log::write(fwd::log::Level::kWarn, "downstream high-water disconnect ep=" + self->endpoint_str());
          return;
        }
        // 默认策略：drop（不给这个下游排队，保护内存；不影响其他下游）
        self->metrics_.drops.fetch_add(1, std::memory_order_relaxed);
        if (!self->high_water_logged_) {
          self->high_water_logged_ = true;
          fwd::log::write(fwd::log::Level::kWarn, "downstream high-water drop ep=" + self->endpoint_str());
        }
        return;
      }
      if (new_total > self->cfg_.flow.hard_limit_bytes) {
        self->metrics_.drops.fetch_add(1, std::memory_order_relaxed);
        self->stopped_ = true;
        boost::system::error_code ec;
        self->socket_.cancel(ec);
        self->socket_.close(ec);
        fwd::log::write(fwd::log::Level::kError, "downstream hard-limit disconnect ep=" + self->endpoint_str());
        return;
      }

      self->queue_.push_back(std::move(bytes));
      self->pending_bytes_ = new_total;
      if (!self->writing_) self->do_write();
    });
  }

  std::size_t pending_bytes() const { return pending_bytes_; }
  bool is_open() const { return socket_.is_open(); }

 private:
  std::string endpoint_str() const {
    boost::system::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (ec) return "unknown";
    return ep.address().to_string() + ":" + std::to_string(ep.port());
  }

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
                self->stopped_ = true;
                boost::system::error_code ec2;
                self->socket_.cancel(ec2);
                self->socket_.close(ec2);
                fwd::log::write(fwd::log::Level::kWarn, "downstream write error: " + ec.message() + " ep=" + self->endpoint_str());
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

  tcp::socket socket_;
  boost::asio::strand<tcp::socket::executor_type> strand_;
  const Config& cfg_;
  Metrics& metrics_;

  bool stopped_{false};
  bool writing_{false};
  std::deque<std::shared_ptr<std::string>> queue_;
  std::size_t pending_bytes_{0};
  bool high_water_logged_{false};
};

class Forwarder : public std::enable_shared_from_this<Forwarder> {
 public:
  Forwarder(boost::asio::io_context& io, Config cfg)
      : io_(io),
        cfg_(std::move(cfg)),
        upstream_acceptor_(io_),
        downstream_acceptor_(io_),
        admin_acceptor_(io_),
        metrics_timer_(io_) {}

  void start() {
    open_listen(upstream_acceptor_, cfg_.upstream_listen, "upstream");
    open_listen(downstream_acceptor_, cfg_.downstream_listen, "downstream");
    open_listen(admin_acceptor_, cfg_.admin.listen, "admin");

    accept_upstream();
    accept_downstream();
    accept_admin();
    tick_metrics();

    fwd::log::write(fwd::log::Level::kInfo,
                    "started: upstream=" + cfg_.upstream_listen.host + ":" + std::to_string(cfg_.upstream_listen.port) +
                        " downstream=" + cfg_.downstream_listen.host + ":" + std::to_string(cfg_.downstream_listen.port) +
                        " admin=" + cfg_.admin.listen.host + ":" + std::to_string(cfg_.admin.listen.port));
  }

 private:
  static std::uint64_t now_ms() {
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
    ev.ts_ms = now_ms();
    ev.level = level_name(lv);
    ev.msg = std::move(msg);
    std::lock_guard lk(events_mu_);
    events_.push_back(std::move(ev));
    while (static_cast<int>(events_.size()) > cfg_.admin.events_max) events_.pop_front();
  }

  struct StatsSnapshot {
    std::uint64_t ts_ms{0};
    std::uint64_t started_ms{0};
    bool upstream_connected{false};
    std::uint64_t upstream_connects{0};
    std::uint64_t downstream_connects{0};
    std::size_t downstream_alive{0};
    std::size_t downstream_pending_bytes{0};
    std::uint64_t frames_in{0};
    std::uint64_t bytes_in{0};
    std::uint64_t frames_out{0};
    std::uint64_t bytes_out{0};
    std::uint64_t drops{0};
  };

  StatsSnapshot snapshot_stats() {
    StatsSnapshot s;
    s.ts_ms = now_ms();
    s.started_ms = started_ms_;
    s.upstream_connects = metrics_.upstream_connects.load(std::memory_order_relaxed);
    s.downstream_connects = metrics_.downstream_connects.load(std::memory_order_relaxed);
    s.frames_in = metrics_.frames_in.load(std::memory_order_relaxed);
    s.bytes_in = metrics_.bytes_in.load(std::memory_order_relaxed);
    s.frames_out = metrics_.frames_out.load(std::memory_order_relaxed);
    s.bytes_out = metrics_.bytes_out.load(std::memory_order_relaxed);
    s.drops = metrics_.drops.load(std::memory_order_relaxed);
    s.upstream_connected = (upstream_ && upstream_->is_open());

    {
      std::lock_guard lk(down_mu_);
      std::size_t write_idx = 0;
      for (std::size_t i = 0; i < down_.size(); ++i) {
        auto& d = down_[i];
        if (d && d->is_open()) {
          ++s.downstream_alive;
          s.downstream_pending_bytes += d->pending_bytes();
          down_[write_idx++] = d;
        }
      }
      // 机会性清理：
      // 这里不额外引入“回收线程”，而是在 stats/broadcast 已经持锁遍历时顺便压缩容器，
      // 移除已断开的 session，保持实现简单、开销可控。
      down_.resize(write_idx);
    }
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
    j += "\"upstream_connected\":" + std::string(s.upstream_connected ? "true" : "false") + ",";
    j += "\"upstream_connects\":" + std::to_string(s.upstream_connects) + ",";
    j += "\"downstream_connects\":" + std::to_string(s.downstream_connects) + ",";
    j += "\"downstream_alive\":" + std::to_string(s.downstream_alive) + ",";
    j += "\"downstream_pending_bytes\":" + std::to_string(s.downstream_pending_bytes) + ",";
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
    j.reserve(1024);
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

    // 读超时实现：让 timer 与 async_read 竞态。
    // - timer 先触发：cancel socket，read 通常以 operation_aborted 等错误返回
    // - read 先完成：取消 timer
    // 目的：在“半开连接/对端卡死/网络抖动”时，读循环不会长期挂住。
    co_spawn(
        ex,
        [t, done, &sock]() -> awaitable<void> {
          boost::system::error_code tec;
          co_await t->async_wait(boost::asio::redirect_error(use_awaitable, tec));
          if (tec) co_return;  // timer 已取消（通常是 read 先完成）
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

  void accept_upstream() {
    upstream_acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket sock) {
      if (ec) {
        fwd::log::write(fwd::log::Level::kError, "accept upstream error: " + ec.message());
        self->push_event(fwd::log::Level::kError, "accept upstream error: " + ec.message());
        self->accept_upstream();
        return;
      }
      self->metrics_.upstream_connects.fetch_add(1, std::memory_order_relaxed);

      // 单上游设计：
      // 这个转发器假设只有一个上游生产者（例如某个 gateway）。
      // 若新上游连入，则主动关闭旧上游，避免多源并存导致语义混乱，也让广播逻辑保持简单。
      if (self->upstream_) {
        fwd::log::write(fwd::log::Level::kWarn, "new upstream connected, closing previous upstream");
        self->push_event(fwd::log::Level::kWarn, "new upstream connected, closing previous upstream");
        boost::system::error_code ec2;
        self->upstream_->cancel(ec2);
        self->upstream_->close(ec2);
        self->upstream_.reset();
      }
      self->upstream_ = std::make_shared<tcp::socket>(std::move(sock));
      fwd::log::write(fwd::log::Level::kInfo, "upstream connected");
      self->push_event(fwd::log::Level::kInfo, "upstream connected");

      self->start_upstream_readloop();
      self->accept_upstream();
    });
  }

  void accept_downstream() {
    downstream_acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket sock) {
      if (ec) {
        fwd::log::write(fwd::log::Level::kError, "accept downstream error: " + ec.message());
        self->push_event(fwd::log::Level::kError, "accept downstream error: " + ec.message());
        self->accept_downstream();
        return;
      }
      self->metrics_.downstream_connects.fetch_add(1, std::memory_order_relaxed);
      auto s = std::make_shared<DownstreamSession>(std::move(sock), self->cfg_, self->metrics_);
      {
        std::lock_guard lk(self->down_mu_);
        self->down_.push_back(s);
      }
      s->start();
      self->push_event(fwd::log::Level::kInfo, "downstream connected");
      self->accept_downstream();
    });
  }

  void accept_admin() {
    admin_acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket sock) {
      if (ec) {
        fwd::log::write(fwd::log::Level::kError, "accept admin error: " + ec.message());
        self->push_event(fwd::log::Level::kError, "accept admin error: " + ec.message());
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
    res.set(http::field::server, "asio-forwarder-admin");
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

  void start_upstream_readloop() {
    if (!upstream_) return;
    co_spawn(io_, [self = shared_from_this()]() -> awaitable<void> { co_await self->upstream_readloop(); }, detached);
  }

  awaitable<void> upstream_readloop() {
    auto sock = upstream_;
    if (!sock) co_return;

    boost::system::error_code ec;
    auto last_activity = std::chrono::steady_clock::now();
    while (true) {
      if (cfg_.timeouts.idle_ms > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_activity > std::chrono::milliseconds(cfg_.timeouts.idle_ms)) {
          ec = boost::asio::error::timed_out;
          fwd::log::write(fwd::log::Level::kWarn, "upstream idle timeout, closing");
          push_event(fwd::log::Level::kWarn, "upstream idle timeout, closing");
          break;
        }
      }

      std::array<std::uint8_t, proto::Header::kHeaderLen> hb{};
      std::size_t n = co_await read_exact_with_timeout(*sock, boost::asio::buffer(hb),
                                                       std::chrono::milliseconds(cfg_.timeouts.read_ms), ec);
      if (ec) break;
      if (n != hb.size()) {
        ec = boost::asio::error::operation_aborted;
        break;
      }
      last_activity = std::chrono::steady_clock::now();

      const auto h = proto::Header::unpack_le(hb.data());
      if (h.magic != proto::Header::kMagic || h.version != proto::Header::kVersion || h.header_len != proto::Header::kHeaderLen) {
        fwd::log::write(fwd::log::Level::kError, "protocol header invalid, closing upstream: " + h.to_string());
        push_event(fwd::log::Level::kError, "protocol header invalid, closing upstream: " + h.to_string());
        ec = boost::asio::error::invalid_argument;
        break;
      }
      if (h.body_len > cfg_.limits.max_body_len) {
        fwd::log::write(fwd::log::Level::kError, "body too large, closing upstream: " + h.to_string());
        push_event(fwd::log::Level::kError, "body too large, closing upstream: " + h.to_string());
        ec = boost::asio::error::message_size;
        break;
      }

      std::string body;
      body.resize(h.body_len);
      if (h.body_len > 0) {
        std::size_t bn = co_await read_exact_with_timeout(*sock, boost::asio::buffer(body),
                                                          std::chrono::milliseconds(cfg_.timeouts.read_ms), ec);
        if (ec) break;
        if (bn != h.body_len) {
          ec = boost::asio::error::operation_aborted;
          break;
        }
      }
      last_activity = std::chrono::steady_clock::now();

      metrics_.frames_in.fetch_add(1, std::memory_order_relaxed);
      metrics_.bytes_in.fetch_add(proto::Header::kHeaderLen + body.size(), std::memory_order_relaxed);

      // 将 header+body 拼成一段连续内存，并在所有下游之间共享（shared_ptr）。
      // 好处：避免“下游 N 个就复制 N 份 payload”；每个下游只是在队列里保存一份 shared_ptr。
      auto out = std::make_shared<std::string>();
      out->reserve(proto::Header::kHeaderLen + body.size());
      const auto packed = h.pack_le();
      out->append(reinterpret_cast<const char*>(packed.data()), packed.size());
      out->append(body);

      broadcast(std::move(out));
    }

    fwd::log::write(fwd::log::Level::kWarn, std::string("upstream read loop exit: ") + ec.message());
    push_event(fwd::log::Level::kWarn, std::string("upstream read loop exit: ") + ec.message());
    if (upstream_ == sock) {
      boost::system::error_code ec2;
      sock->cancel(ec2);
      sock->close(ec2);
      upstream_.reset();
    }
    co_return;
  }

  void broadcast(std::shared_ptr<std::string> bytes) {
    std::vector<std::shared_ptr<DownstreamSession>> targets;
    {
      std::lock_guard lk(down_mu_);
      targets.reserve(down_.size());
      std::size_t write_idx = 0;
      for (std::size_t i = 0; i < down_.size(); ++i) {
        auto& s = down_[i];
        if (s && s->is_open()) {
          targets.push_back(s);
          down_[write_idx++] = s;
        }
      }
      // 与 stats 相同的机会性清理：压缩容器，减少后续遍历成本。
      down_.resize(write_idx);
    }

    for (auto& s : targets) {
      s->send_frame(bytes);
    }
  }

  void tick_metrics() {
    metrics_timer_.expires_after(std::chrono::milliseconds(cfg_.metrics.interval_ms));
    metrics_timer_.async_wait([self = shared_from_this()](boost::system::error_code ec) {
      if (ec) return;
      self->report_metrics();
      self->tick_metrics();
    });
  }

  void report_metrics() {
    const auto s = snapshot_stats();

    fwd::log::write(fwd::log::Level::kInfo,
                    "metrics: up_conn=" + std::to_string(metrics_.upstream_connects.load()) +
                        " down_conn=" + std::to_string(metrics_.downstream_connects.load()) +
                        " down_alive=" + std::to_string(s.downstream_alive) +
                        " frames_in=" + std::to_string(metrics_.frames_in.load()) +
                        " bytes_in=" + std::to_string(metrics_.bytes_in.load()) +
                        " frames_out=" + std::to_string(metrics_.frames_out.load()) +
                        " bytes_out=" + std::to_string(metrics_.bytes_out.load()) +
                        " drops=" + std::to_string(metrics_.drops.load()) +
                        " down_pending_bytes=" + std::to_string(s.downstream_pending_bytes));
  }

  boost::asio::io_context& io_;
  Config cfg_;

  tcp::acceptor upstream_acceptor_;
  tcp::acceptor downstream_acceptor_;
  tcp::acceptor admin_acceptor_;

  std::shared_ptr<tcp::socket> upstream_;

  // 下游 session 列表采用“简单 vector + 机会性清理”的方式维护。
  // 这里刻意不引入更复杂的容器/弱引用机制，原因是：
  // - session 由此 vector 持有（shared_ptr）
  // - 连接断开后 is_open() 会变为 false，随后在 snapshot_stats()/broadcast() 持同一把锁时被移除
  std::mutex down_mu_;
  std::vector<std::shared_ptr<DownstreamSession>> down_;

  Metrics metrics_{};
  boost::asio::steady_timer metrics_timer_;

  const std::uint64_t started_ms_{now_ms()};
  std::mutex events_mu_;
  std::deque<AdminEvent> events_;
};

}  // 命名空间 fwd

int main(int argc, char** argv) {
  try {
    std::string cfg_path = "configs/dev/forwarder.json";
    if (argc >= 2) cfg_path = argv[1];

    auto cfg = fwd::load_config_or_throw(cfg_path);

    boost::asio::io_context io;
    auto fw = std::make_shared<fwd::Forwarder>(io, cfg);
    fw->start();

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

