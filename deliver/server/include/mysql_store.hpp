#pragma once

#include "fwd/config.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct MYSQL;

namespace fwd::db {

// MySQL 用户校验 + 全局 IP 白名单。线程安全（单连接+互斥锁）。
class MysqlStore {
 public:
  MysqlStore() = default;
  MysqlStore(const MysqlStore&) = delete;
  MysqlStore& operator=(const MysqlStore&) = delete;
  ~MysqlStore();

  void connect(const Config::Mysql& c);
  void close();

  // 先校验用：对端地址字符串，与 ip_allowlist.ip 全字匹配
  bool ip_whitelisted(const std::string& client_ip);
  // 成功返回 nullopt 且填 out；失败返回错误信息（人可读）
  struct UserInfo {
    std::uint64_t id{0};
    bool is_admin{false};
  };
  // 白名单校验通过后调用：无则 INSERT，有则校验口令；want_admin 须与库中 is_admin 一致
  std::optional<std::string> authenticate_or_register(const std::string& username, const std::string& password,
                                                      bool want_admin, UserInfo& out);
  // 为路由缓存：任用户名字 → id
  std::optional<std::uint64_t> user_id_by_username(const std::string& username);

 private:
  std::mutex mu_{};
  MYSQL* conn_{nullptr};
  Config::Mysql cfg_{};
};

}  // namespace fwd::db
