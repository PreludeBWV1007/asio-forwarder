#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
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

namespace fwd {

// ===== protocol impl =====
std::string proto::Header::to_string() const {
  return "magic=" + std::to_string(magic) + " ver=" + std::to_string(version) +
         " hlen=" + std::to_string(header_len) + " blen=" + std::to_string(body_len) +
         " type=" + std::to_string(msg_type) + " flags=" + std::to_string(flags) +
         " seq=" + std::to_string(seq);
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
  if (cfg.limits.max_body_len == 0 || cfg.limits.max_body_len > 64u * 1024u * 1024u) throw std::runtime_error("invalid limits.max_body_len");
  if (cfg.flow.high_water_bytes == 0 || cfg.flow.hard_limit_bytes < cfg.flow.high_water_bytes) throw std::runtime_error("invalid flow thresholds");
  if (!(cfg.flow.on_high_water == "drop" || cfg.flow.on_high_water == "disconnect")) throw std::runtime_error("invalid flow.send_queue.on_high_water");
  if (cfg.timeouts.read_ms < 100 || cfg.timeouts.idle_ms < cfg.timeouts.read_ms) throw std::runtime_error("invalid timeouts");
  if (cfg.metrics.interval_ms < 200 || cfg.metrics.interval_ms > 60 * 60 * 1000) throw std::runtime_error("invalid metrics.interval_ms");

  return cfg;
}

// ===== forwarder =====
struct Metrics {
  std::atomic<std::uint64_t> upstream_connects{0};
  std::atomic<std::uint64_t> downstream_connects{0};
  std::atomic<std::uint64_t> frames_in{0};
  std::atomic<std::uint64_t> bytes_in{0};
  std::atomic<std::uint64_t> frames_out{0};
  std::atomic<std::uint64_t> bytes_out{0};
  std::atomic<std::uint64_t> drops{0};
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
        // default: drop (不给这个下游排队，保护内存；不影响其他下游)
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
        metrics_timer_(io_) {}

  void start() {
    open_listen(upstream_acceptor_, cfg_.upstream_listen, "upstream");
    open_listen(downstream_acceptor_, cfg_.downstream_listen, "downstream");

    accept_upstream();
    accept_downstream();
    tick_metrics();

    fwd::log::write(fwd::log::Level::kInfo,
                    "started: upstream=" + cfg_.upstream_listen.host + ":" + std::to_string(cfg_.upstream_listen.port) +
                        " downstream=" + cfg_.downstream_listen.host + ":" + std::to_string(cfg_.downstream_listen.port));
  }

 private:
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
          if (tec) co_return;  // canceled
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
        self->accept_upstream();
        return;
      }
      self->metrics_.upstream_connects.fetch_add(1, std::memory_order_relaxed);

      if (self->upstream_) {
        fwd::log::write(fwd::log::Level::kWarn, "new upstream connected, closing previous upstream");
        boost::system::error_code ec2;
        self->upstream_->cancel(ec2);
        self->upstream_->close(ec2);
        self->upstream_.reset();
      }
      self->upstream_ = std::make_shared<tcp::socket>(std::move(sock));
      fwd::log::write(fwd::log::Level::kInfo, "upstream connected");

      self->start_upstream_readloop();
      self->accept_upstream();
    });
  }

  void accept_downstream() {
    downstream_acceptor_.async_accept([self = shared_from_this()](boost::system::error_code ec, tcp::socket sock) {
      if (ec) {
        fwd::log::write(fwd::log::Level::kError, "accept downstream error: " + ec.message());
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
      self->accept_downstream();
    });
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
        ec = boost::asio::error::invalid_argument;
        break;
      }
      if (h.body_len > cfg_.limits.max_body_len) {
        fwd::log::write(fwd::log::Level::kError, "body too large, closing upstream: " + h.to_string());
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

      auto out = std::make_shared<std::string>();
      out->reserve(proto::Header::kHeaderLen + body.size());
      const auto packed = h.pack_le();
      out->append(reinterpret_cast<const char*>(packed.data()), packed.size());
      out->append(body);

      broadcast(std::move(out));
    }

    fwd::log::write(fwd::log::Level::kWarn, std::string("upstream read loop exit: ") + ec.message());
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
    std::size_t down_count = 0;
    std::size_t down_pending = 0;
    {
      std::lock_guard lk(down_mu_);
      std::size_t write_idx = 0;
      for (std::size_t i = 0; i < down_.size(); ++i) {
        auto& s = down_[i];
        if (s && s->is_open()) {
          ++down_count;
          down_pending += s->pending_bytes();
          down_[write_idx++] = s;
        }
      }
      down_.resize(write_idx);
    }

    fwd::log::write(fwd::log::Level::kInfo,
                    "metrics: up_conn=" + std::to_string(metrics_.upstream_connects.load()) +
                        " down_conn=" + std::to_string(metrics_.downstream_connects.load()) +
                        " down_alive=" + std::to_string(down_count) +
                        " frames_in=" + std::to_string(metrics_.frames_in.load()) +
                        " bytes_in=" + std::to_string(metrics_.bytes_in.load()) +
                        " frames_out=" + std::to_string(metrics_.frames_out.load()) +
                        " bytes_out=" + std::to_string(metrics_.bytes_out.load()) +
                        " drops=" + std::to_string(metrics_.drops.load()) +
                        " down_pending_bytes=" + std::to_string(down_pending));
  }

  boost::asio::io_context& io_;
  Config cfg_;

  tcp::acceptor upstream_acceptor_;
  tcp::acceptor downstream_acceptor_;

  std::shared_ptr<tcp::socket> upstream_;

  // 保存下游弱引用：断开时自动清理
  std::mutex down_mu_;
  std::vector<std::shared_ptr<DownstreamSession>> down_;

  Metrics metrics_{};
  boost::asio::steady_timer metrics_timer_;
};

}  // namespace fwd

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

