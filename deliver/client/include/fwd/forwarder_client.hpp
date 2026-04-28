#pragma once

// 对外集成推荐**只包含本头文件**：同步高层 API（连接、登录、收发字节、管理命令）
// 与可选的本机测试用 LocalForwarder。实现位于静态库 asio_forwarder_sdk（单源文件编译）。
#include "fwd/asio_forwarder_client.hpp"
