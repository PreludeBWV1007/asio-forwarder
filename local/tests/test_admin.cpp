// test_admin — 独立可执行：模拟真实集成方，仅测管理员能力。
// 依赖：中继已启动 + MySQL 已导入 local/tests/seed_e2e.sql（sc_admin/padm、e2e_alice/secret1、127.0.0.1 白名单）。
// 用户语义：在白名单 IP 上，login 可对「尚无库行」的用户名自动 INSERT（见层次 6b）。
// 用法: ./build/test_admin <host> <业务TCP端口>
// 例:   ./build/test_admin 127.0.0.1 19000

#include "fwd/forwarder_client.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <msgpack.hpp>
#include <string>
#include <string_view>

namespace afc = fwd::asio_forwarder_client;

// ---------- 小工具：从 list_users 正文中按登录名取 user_id（在线用户列表） ----------
static std::uint64_t user_id_from_list_users_body(const msgpack::object& root, std::string_view username) {
  if (root.type != msgpack::type::MAP) return 0;
  const auto& mp = root.via.map;
  for (std::uint32_t i = 0; i < mp.size; ++i) {
    if (mp.ptr[i].key.type != msgpack::type::STR) continue;
    std::string key;
    mp.ptr[i].key.convert(key);
    if (key != "users" || mp.ptr[i].val.type != msgpack::type::ARRAY) continue;
    const auto& arr = mp.ptr[i].val.via.array;
    for (std::uint32_t j = 0; j < arr.size; ++j) {
      if (arr.ptr[j].type != msgpack::type::MAP) continue;
      const auto& row = arr.ptr[j].via.map;
      std::uint64_t uid = 0;
      std::string uname;
      for (std::uint32_t t = 0; t < row.size; ++t) {
        if (row.ptr[t].key.type != msgpack::type::STR) continue;
        std::string kk;
        row.ptr[t].key.convert(kk);
        if (kk == "user_id") {
          if (row.ptr[t].val.type == msgpack::type::POSITIVE_INTEGER) uid = row.ptr[t].val.via.u64;
        } else if (kk == "username" && row.ptr[t].val.type == msgpack::type::STR) {
          row.ptr[t].val.convert(uname);
        }
      }
      if (uname == username) return uid;
    }
  }
  return 0;
}

/** allowlist_list / user_table_list 回复里 rows 中按 ip 找 id */
static std::uint64_t allowlist_id_for_ip(const msgpack::object& root, std::string_view want_ip) {
  if (root.type != msgpack::type::MAP) return 0;
  const auto& mp = root.via.map;
  for (std::uint32_t i = 0; i < mp.size; ++i) {
    if (mp.ptr[i].key.type != msgpack::type::STR) continue;
    std::string key;
    mp.ptr[i].key.convert(key);
    if (key != "rows" || mp.ptr[i].val.type != msgpack::type::ARRAY) continue;
    const auto& arr = mp.ptr[i].val.via.array;
    for (std::uint32_t j = 0; j < arr.size; ++j) {
      if (arr.ptr[j].type != msgpack::type::MAP) continue;
      const auto& row = arr.ptr[j].via.map;
      std::uint64_t id = 0;
      std::string ip;
      for (std::uint32_t t = 0; t < row.size; ++t) {
        if (row.ptr[t].key.type != msgpack::type::STR) continue;
        std::string kk;
        row.ptr[t].key.convert(kk);
        if (kk == "id" && row.ptr[t].val.type == msgpack::type::POSITIVE_INTEGER) id = row.ptr[t].val.via.u64;
        else if (kk == "ip" && row.ptr[t].val.type == msgpack::type::STR) row.ptr[t].val.convert(ip);
      }
      if (ip == want_ip) return id;
    }
  }
  return 0;
}

static std::uint64_t user_table_id_for_username(const msgpack::object& root, std::string_view want_un) {
  if (root.type != msgpack::type::MAP) return 0;
  const auto& mp = root.via.map;
  for (std::uint32_t i = 0; i < mp.size; ++i) {
    if (mp.ptr[i].key.type != msgpack::type::STR) continue;
    std::string key;
    mp.ptr[i].key.convert(key);
    if (key != "rows" || mp.ptr[i].val.type != msgpack::type::ARRAY) continue;
    const auto& arr = mp.ptr[i].val.via.array;
    for (std::uint32_t j = 0; j < arr.size; ++j) {
      if (arr.ptr[j].type != msgpack::type::MAP) continue;
      const auto& row = arr.ptr[j].via.map;
      std::uint64_t id = 0;
      std::string uname;
      for (std::uint32_t t = 0; t < row.size; ++t) {
        if (row.ptr[t].key.type != msgpack::type::STR) continue;
        std::string kk;
        row.ptr[t].key.convert(kk);
        if (kk == "id" && row.ptr[t].val.type == msgpack::type::POSITIVE_INTEGER) id = row.ptr[t].val.via.u64;
        else if (kk == "username" && row.ptr[t].val.type == msgpack::type::STR) row.ptr[t].val.convert(uname);
      }
      if (uname == want_un) return id;
    }
  }
  return 0;
}

static void cout_section(const char* title) { std::cout << "\n========== " << title << " ==========\n"; }

/** 若存在则先删，便于重复跑 test（仅用于 CRUD 测试 IP） */
static void allowlist_cleanup_if_any(afc::Client& adm, const std::string& ip) {
  msgpack::sbuffer lb;
  msgpack::packer<msgpack::sbuffer> pk0(&lb);
  pk0.pack_map(1);
  pk0.pack("action");
  pk0.pack("allowlist_list");
  const msgpack::object_handle oh = adm.control_request(lb);
  const std::uint64_t id = allowlist_id_for_ip(oh.get(), ip);
  if (id == 0) return;
  msgpack::sbuffer db;
  msgpack::packer<msgpack::sbuffer> pk(&db);
  pk.pack_map(2);
  pk.pack("action");
  pk.pack("allowlist_delete");
  pk.pack("id");
  pk.pack(id);
  (void)adm.control_request(db);
}

static msgpack::object_handle control_user_table_list(afc::Client& adm) {
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(1);
  pk.pack("action");
  pk.pack("user_table_list");
  return adm.control_request(buf);
}

static void control_user_table_add(afc::Client& adm, std::string_view username, std::string_view password, bool is_admin) {
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(4);
  pk.pack("action");
  pk.pack("user_table_add");
  pk.pack("username");
  pk.pack(username);
  pk.pack("password");
  pk.pack(password);
  pk.pack("is_admin");
  pk.pack(is_admin);
  (void)adm.control_request(buf);
}

static void control_user_table_update_password(afc::Client& adm, std::uint64_t id, std::string_view password) {
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(3);
  pk.pack("action");
  pk.pack("user_table_update");
  pk.pack("id");
  pk.pack(id);
  pk.pack("password");
  pk.pack(password);
  (void)adm.control_request(buf);
}

static void control_user_table_delete_by_id(afc::Client& adm, std::uint64_t id) {
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(&buf);
  pk.pack_map(2);
  pk.pack("action");
  pk.pack("user_table_delete");
  pk.pack("id");
  pk.pack(id);
  (void)adm.control_request(buf);
}

/** 若存在则先删，便于重复跑 test（用户名） */
static void user_table_cleanup_if_any(afc::Client& adm, std::string_view username) {
  const std::uint64_t id = user_table_id_for_username(control_user_table_list(adm).get(), username);
  if (id == 0) return;
  control_user_table_delete_by_id(adm, id);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: test_admin <host> <业务TCP端口>\n";
    return 1;
  }
  const std::string host = argv[1];
  const std::uint16_t biz = static_cast<std::uint16_t>(std::atoi(argv[2]));

  try {
    // ========== 层次 1：管理员会话（业务长连 + peer_role=admin）==========
    cout_section("1 管理员 connect + sign_on");
    afc::Client adm;
    adm.open({host, biz});
    adm.sign_on("sc_admin", "padm", afc::RecvMode::Broadcast, "admin");
    std::cout << "[OK] 管理员登录名: " << adm.local_username() << '\n';

    std::cout << "[INFO] 空闲 60s 验证长连（依赖内置周期 HEARTBEAT：20s）…\n";
    std::this_thread::sleep_for(std::chrono::minutes(1));
    std::cout << "[OK] 空闲结束，继续层次 2\n";

    // ========== 层次 2：control_list_users —— 取 201 全文（含 users）==========
    cout_section("2 control_list_users（应返回 optional，正文含 users）");
    auto list_oh = adm.control_list_users(true);
    if (!list_oh) {
      std::cout << "[FAIL] 未拿到应答\n";
      return 2;
    }
    std::cout << "[DATA] list_users 正文:\n" << list_oh->get() << '\n';

    // ========== 层次 3：CONTROL 查库 —— 白名单 / 用户表（与 control_request 一致）==========
    cout_section("3 control_request: allowlist_list");
    {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(1);
      pk.pack("action");
      pk.pack("allowlist_list");
      std::cout << "[DATA] allowlist_list:\n" << adm.control_request(buf).get() << '\n';
    }

    cout_section("4 control_request: user_table_list（节选：库内账号行数）");
    {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(1);
      pk.pack("action");
      pk.pack("user_table_list");
      const auto reply = adm.control_request(buf).get();
      std::cout << "[DATA] user_table_list:\n" << reply << '\n';
      if (reply.type == msgpack::type::MAP) {
        for (std::uint32_t i = 0; i < reply.via.map.size; ++i) {
          if (reply.via.map.ptr[i].key.type != msgpack::type::STR) continue;
          std::string k;
          reply.via.map.ptr[i].key.convert(k);
          if (k == "rows" && reply.via.map.ptr[i].val.type == msgpack::type::ARRAY) {
            std::cout << "[INFO] users 表行数(rows): " << reply.via.map.ptr[i].val.via.array.size << '\n';
            break;
          }
        }
      }
    }

    // ========== 层次 5：ip_allowlist 全 CRUD（测试网段 IP，重复跑前先清理）==========
    cout_section("5 allowlist CRUD：add → list → update → list → delete → list");
    const std::string crud_ip_a = "192.0.2.77";
    const std::string crud_ip_b = "192.0.2.78";
    allowlist_cleanup_if_any(adm, crud_ip_a);
    allowlist_cleanup_if_any(adm, crud_ip_b);
    {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(2);
      pk.pack("action");
      pk.pack("allowlist_add");
      pk.pack("ip");
      pk.pack(crud_ip_a);
      (void)adm.control_request(buf);
      std::cout << "[OK] allowlist_add " << crud_ip_a << '\n';
    }
    {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(1);
      pk.pack("action");
      pk.pack("allowlist_list");
      const auto r = adm.control_request(buf).get();
      const std::uint64_t aid = allowlist_id_for_ip(r, crud_ip_a);
      std::cout << "[DATA] allowlist 应含 " << crud_ip_a << " id=" << aid << "\n" << r << '\n';
      if (aid == 0) {
        std::cout << "[FAIL] allowlist_add 后 list 找不到 ip\n";
        return 6;
      }
      msgpack::sbuffer ub;
      msgpack::packer<msgpack::sbuffer> pk2(&ub);
      pk2.pack_map(3);
      pk2.pack("action");
      pk2.pack("allowlist_update");
      pk2.pack("id");
      pk2.pack(aid);
      pk2.pack("ip");
      pk2.pack(crud_ip_b);
      (void)adm.control_request(ub);
      std::cout << "[OK] allowlist_update id=" << aid << " → " << crud_ip_b << '\n';
    }
    {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(1);
      pk.pack("action");
      pk.pack("allowlist_list");
      const auto r = adm.control_request(buf).get();
      const std::uint64_t bid = allowlist_id_for_ip(r, crud_ip_b);
      if (allowlist_id_for_ip(r, crud_ip_a) != 0 || bid == 0) {
        std::cout << "[FAIL] allowlist_update 后 rows 不符合预期\n"
                  << r << '\n';
        return 7;
      }
      std::cout << "[OK] allowlist 已变为 " << crud_ip_b << " id=" << bid << '\n';
      msgpack::sbuffer db;
      msgpack::packer<msgpack::sbuffer> pk2(&db);
      pk2.pack_map(2);
      pk2.pack("action");
      pk2.pack("allowlist_delete");
      pk2.pack("id");
      pk2.pack(bid);
      (void)adm.control_request(db);
      std::cout << "[OK] allowlist_delete id=" << bid << '\n';
    }
    {
      msgpack::sbuffer buf;
      msgpack::packer<msgpack::sbuffer> pk(&buf);
      pk.pack_map(1);
      pk.pack("action");
      pk.pack("allowlist_list");
      const auto r = adm.control_request(buf).get();
      if (allowlist_id_for_ip(r, crud_ip_b) != 0) {
        std::cout << "[FAIL] delete 后仍能看到 " << crud_ip_b << '\n';
        return 8;
      }
      std::cout << "[OK] allowlist delete 后已无测试 IP\n";
    }

    // ========== 层次 6：users 表（管理员 CRUD + 与「登录自动落库」的交互）==========
    cout_section("6 users 表：6a 管理员 CRUD + try_login；6b 删行后登录再注册");
    const std::string crud_un = "tu_crud_user";
    const std::string crud_pw1 = "crud-pw-one";
    const std::string crud_pw2 = "crud-pw-two";
    user_table_cleanup_if_any(adm, crud_un);

    cout_section("6a user_table add / list / update 密码，try_login 校验新旧口令");
    control_user_table_add(adm, crud_un, crud_pw1, false);
    std::cout << "[OK] user_table_add " << crud_un << '\n';
    std::uint64_t crud_uid = user_table_id_for_username(control_user_table_list(adm).get(), crud_un);
    if (crud_uid == 0) {
      std::cout << "[FAIL] user_table_add 后 list 无用户\n";
      return 9;
    }
    std::cout << "[OK] user_table id=" << crud_uid << '\n';
    control_user_table_update_password(adm, crud_uid, crud_pw2);
    std::cout << "[OK] user_table_update 改口令\n";
    if (!afc::try_login(host, biz, crud_un, crud_pw2, afc::RecvMode::Broadcast, "user")) {
      std::cout << "[FAIL] try_login：库内新口令应成功（短连探测）\n";
      return 10;
    }
    if (afc::try_login(host, biz, crud_un, crud_pw1, afc::RecvMode::Broadcast, "user")) {
      std::cout << "[FAIL] try_login：旧口令应被拒绝\n";
      return 11;
    }
    std::cout << "[OK] try_login 新口令成功、旧口令失败\n";

    cout_section("6b user_table_delete 后：同口令 try_login 应自动 INSERT（新 id ≠ 删前）");
    control_user_table_delete_by_id(adm, crud_uid);
    std::cout << "[OK] user_table_delete id=" << crud_uid << "（表中已无此行）\n";
    // 曾与「禁止登录路径注册」的假设施混淆：此处验证的是服务端默认语义——库里无此行时 login 会注册。
    if (!afc::try_login(host, biz, crud_un, crud_pw2, afc::RecvMode::Broadcast, "user")) {
      std::cout << "[FAIL] 删行后 try_login 应再次成功（登录路径自动注册，白名单内需可用）\n";
      return 12;
    }
    std::cout << "[OK] 删行后 try_login 成功（自动注册落库）\n";
    const std::uint64_t rereg_uid = user_table_id_for_username(control_user_table_list(adm).get(), crud_un);
    if (rereg_uid == 0) {
      std::cout << "[FAIL] 自动注册后 user_table_list 应含该用户名\n";
      return 13;
    }
    if (rereg_uid == crud_uid) {
      std::cout << "[FAIL] 自动注册应产生新 id，不应仍等于删前的 id=" << crud_uid << '\n';
      return 14;
    }
    std::cout << "[OK] 自动注册后 id=" << rereg_uid << "（删前 id=" << crud_uid << "）\n";
    user_table_cleanup_if_any(adm, crud_un);
    std::cout << "[INFO] 已清理 " << crud_un << "，避免干扰层次 7+\n";

    // ========== 层次 7：普通用户上线 → list 中有 e2e_alice → kick_user ==========
    cout_section("7 普通用户 e2e_alice 登录（供踢人演示）");
    afc::Client alice;
    alice.open({host, biz});
    alice.sign_on("e2e_alice", "secret1", afc::RecvMode::Broadcast, "user");
    std::cout << "[OK] e2e_alice 已在线\n";

    cout_section("8 再次 control_list_users，解析 e2e_alice 的 user_id");
    list_oh = adm.control_list_users(true);
    if (!list_oh) {
      std::cout << "[FAIL] 第二次 list 无应答\n";
      return 3;
    }
    const std::uint64_t alice_uid = user_id_from_list_users_body(list_oh->get(), "e2e_alice");
    std::cout << "[INFO] e2e_alice user_id=" << alice_uid << '\n';
    if (alice_uid == 0) {
      std::cout << "[FAIL] 列表中未找到 e2e_alice\n";
      return 4;
    }

    cout_section("9 control_kick_user（按 user_id 踢掉该用户全部连接）");
    (void)adm.control_kick_user(alice_uid, true);
    std::cout << "[OK] 已发送 kick_user uid=" << alice_uid << " 并等待 201\n";

    cout_section("10 验证：被踢连接不应再能 heartbeat");
    try {
      alice.heartbeat();
      std::cout << "[FAIL] alice 仍可用，预期应失败\n";
      return 5;
    } catch (const std::exception& e) {
      std::cout << "[OK] alice 侧异常（预期）: " << e.what() << '\n';
    }

    std::cout << "\n========== 全部步骤完成 ==========\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[FAIL] " << e.what() << '\n';
    return 10;
  }
}
