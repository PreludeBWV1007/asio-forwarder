-- E2E 测试数据（与 e2e_forwarder.py 用户名一致）
INSERT IGNORE INTO ip_allowlist (ip) VALUES ('127.0.0.1'), ('::1');

INSERT INTO users (username, password, is_admin) VALUES
  ('e2e_alice', 'secret1', 0),
  ('e2e_bob', 'secret2', 0),
  ('su_alice', 'p1', 0),
  ('su_bob', 'p2', 0),
  ('sb_alice', 'p1', 0),
  ('sb_bob', 'p2', 0),
  ('rr2_alice', 'p1', 0),
  ('rr2_bob', 'p2', 0),
  ('adm_kick2', 'pwadm', 1),
  ('usr_kick2', 'pwu', 0),
  ('cpp_desk', 'demo-pw', 0),
  ('cpp_md', 'demo-pw', 0),
  ('slimuser', 'demo-pw', 0),
  ('sc_alice', 'pa', 0),
  ('sc_bob', 'pb', 0),
  ('sc_carol', 'pc', 0),
  ('sc_admin', 'padm', 1),
  ('perf_src', 'perf-pw', 0),
  ('perf_dst', 'perf-pw', 0)
ON DUPLICATE KEY UPDATE password = VALUES(password), is_admin = VALUES(is_admin);
