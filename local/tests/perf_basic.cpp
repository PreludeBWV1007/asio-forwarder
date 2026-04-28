// 基本性能：两客户端 login 后 A→B 投递闭环 N 次，打印总耗时与平均 RTT（含本端等待 201 与对端 200）。
// 用法：forwarder_perf HOST PORT [N]   —— 须已配置 MySQL 且 seed 含 perf_src / perf_dst。

#include "fwd/asio_forwarder_client.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: forwarder_perf HOST PORT [roundtrips]\n";
    return 1;
  }
  const std::string host = argv[1];
  const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  const int N = (argc >= 4) ? std::atoi(argv[3]) : 1000;
  if (N < 1) return 1;

  namespace afc = fwd::asio_forwarder_client;
  afc::Client a;
  afc::Client b;
  a.open({host, port});
  b.open({host, port});
  a.sign_on("perf_src", "perf-pw", afc::RecvMode::Broadcast, "user");
  b.sign_on("perf_dst", "perf-pw", afc::RecvMode::Broadcast, "user");

  std::string pl(64, 'z');
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) {
    pl[0] = static_cast<char>('A' + (i % 26));
    (void)a.send("perf_dst", pl, {/*wait_server_accept=*/true});
    (void)b.recv_deliver();
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

  std::cout << "forwarder_perf roundtrips=" << N << " payload_bytes=" << pl.size() << " total_ms=" << ms
            << " rtt_avg_ms=" << (ms / static_cast<double>(N)) << '\n';
  return 0;
}
