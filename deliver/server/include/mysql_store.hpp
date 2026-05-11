#pragma once

#include "fwd/config.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
  // 白名单校验通过后调用：库中无此用户名则 INSERT 注册；有则校验口令；want_admin 须与库中 is_admin 一致
  std::optional<std::string> authenticate_or_register(const std::string& username, const std::string& password,
                                                      bool want_admin, UserInfo& out);
  // 任用户名字 → id
  std::optional<std::uint64_t> user_id_by_username(const std::string& username);

  // 管理员 CONTROL：两张库的 CRUD（错误时返回人可读信息；成功返回 nullopt）
  struct AdminIpRow {
    std::uint64_t id{0};
    std::string ip;
  };
  struct AdminUserRow {
    std::uint64_t id{0};
    std::string username;
    bool is_admin{false};
  };
  std::optional<std::string> admin_list_allowlist(std::vector<AdminIpRow>& out);
  std::optional<std::string> admin_insert_allowlist(const std::string& ip);
  std::optional<std::string> admin_update_allowlist(std::uint64_t id, const std::string& ip);
  std::optional<std::string> admin_delete_allowlist(std::uint64_t id);
  std::optional<std::string> admin_list_users(std::vector<AdminUserRow>& out);
  std::optional<std::string> admin_insert_user(const std::string& username, const std::string& password, bool is_admin);
  // new_username / new_password 空指针表示不改该项；new_is_admin 空指针表示不改
  std::optional<std::string> admin_update_user(std::uint64_t id, const std::string* new_username, const std::string* new_password,
                                               const bool* new_is_admin);
  std::optional<std::string> admin_delete_user(std::uint64_t id);
  std::optional<std::string> admin_get_username_by_id(std::uint64_t id, std::string& out_name);

 private:
  std::mutex mu_{};
  MYSQL* conn_{nullptr};
  Config::Mysql cfg_{};
};

}  // namespace fwd::db
