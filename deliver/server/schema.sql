-- asio-forwarder：全局 IP 白名单 + 用户表（明文密码，生产请改为摘要）
CREATE TABLE IF NOT EXISTS ip_allowlist (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  ip VARCHAR(128) NOT NULL,
  UNIQUE KEY uq_ip (ip)
);

CREATE TABLE IF NOT EXISTS users (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  username VARCHAR(255) NOT NULL,
  password VARCHAR(4096) NOT NULL,
  is_admin TINYINT(1) NOT NULL DEFAULT 0,
  UNIQUE KEY uq_username (username)
);
