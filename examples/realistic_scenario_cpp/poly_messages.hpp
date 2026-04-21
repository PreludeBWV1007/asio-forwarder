#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <msgpack.hpp>

#include "messages.hpp"

namespace demo {

// 为“多态传输”枚举一组常见消息类型。
// 约定：wire payload 为 msgpack map：{kind:<int>, type:<str>, data:<obj>}
enum class PayloadKind : std::uint32_t {
  kText = 1,
  kTask = 2,
  kTaskResult = 3,
  kNotice = 4,
  kStockTick = 5,
};

inline const char* kind_name(PayloadKind k) {
  switch (k) {
    case PayloadKind::kText:
      return "Text";
    case PayloadKind::kTask:
      return "Task";
    case PayloadKind::kTaskResult:
      return "TaskResult";
    case PayloadKind::kNotice:
      return "Notice";
    case PayloadKind::kStockTick:
      return "StockTick";
    default:
      return "Unknown";
  }
}

struct PolyEnvelope {
  PayloadKind kind{PayloadKind::kText};
  std::string type;  // 人类可读类型名；用于调试/前端展示
  msgpack::object data;  // 指向 holder 的 zone
  msgpack::object_handle holder;
};

inline std::string pack_poly_payload(PayloadKind kind, const std::string& type, const msgpack::object& data_obj) {
  msgpack::sbuffer sb;
  msgpack::packer<msgpack::sbuffer> pk(&sb);
  pk.pack_map(3);
  pk.pack("kind");
  pk.pack(static_cast<std::uint32_t>(kind));
  pk.pack("type");
  pk.pack(type);
  pk.pack("data");
  pk.pack(data_obj);
  return std::string(sb.data(), sb.size());
}

template <class T>
inline std::string pack_poly_payload(PayloadKind kind, const std::string& type, const T& v) {
  msgpack::sbuffer sb;
  msgpack::pack(sb, v);
  auto oh = msgpack::unpack(sb.data(), sb.size());
  return pack_poly_payload(kind, type, oh.get());
}

inline PolyEnvelope unpack_poly_envelope(const std::string& payload_bytes) {
  PolyEnvelope env;
  env.holder = msgpack::unpack(payload_bytes.data(), payload_bytes.size());
  const msgpack::object root = env.holder.get();
  if (root.type != msgpack::type::MAP) throw std::runtime_error("poly payload: root must be msgpack map");

  std::optional<std::uint32_t> kind_u32;
  std::optional<std::string> type;
  const msgpack::object* data_obj = nullptr;

  const auto& mp = root.via.map;
  for (std::uint32_t i = 0; i < mp.size; ++i) {
    if (mp.ptr[i].key.type != msgpack::type::STR) continue;
    std::string k;
    mp.ptr[i].key.convert(k);
    if (k == "kind") {
      std::uint32_t v = 0;
      mp.ptr[i].val.convert(v);
      kind_u32 = v;
    } else if (k == "type") {
      std::string v;
      mp.ptr[i].val.convert(v);
      type = std::move(v);
    } else if (k == "data") {
      data_obj = &mp.ptr[i].val;
    }
  }

  if (!kind_u32) throw std::runtime_error("poly payload: missing kind");
  if (!type) throw std::runtime_error("poly payload: missing type");
  if (!data_obj) throw std::runtime_error("poly payload: missing data");

  env.kind = static_cast<PayloadKind>(*kind_u32);
  env.type = *type;
  env.data = *data_obj;
  return env;
}

struct PayloadBase {
  virtual ~PayloadBase() = default;
  virtual PayloadKind kind() const = 0;
  virtual std::string type_name() const = 0;
  virtual std::string pack() const = 0;  // returns payload bytes
};

struct TextPayload final : PayloadBase {
  std::string text;
  PayloadKind kind() const override { return PayloadKind::kText; }
  std::string type_name() const override { return kind_name(kind()); }
  std::string pack() const override { return pack_poly_payload(kind(), type_name(), text); }
};

struct TaskPayload final : PayloadBase {
  Task task;
  PayloadKind kind() const override { return PayloadKind::kTask; }
  std::string type_name() const override { return kind_name(kind()); }
  std::string pack() const override { return pack_poly_payload(kind(), type_name(), task); }
};

struct TaskResultPayload final : PayloadBase {
  TaskResult result;
  PayloadKind kind() const override { return PayloadKind::kTaskResult; }
  std::string type_name() const override { return kind_name(kind()); }
  std::string pack() const override { return pack_poly_payload(kind(), type_name(), result); }
};

struct NoticePayload final : PayloadBase {
  Notice notice;
  PayloadKind kind() const override { return PayloadKind::kNotice; }
  std::string type_name() const override { return kind_name(kind()); }
  std::string pack() const override { return pack_poly_payload(kind(), type_name(), notice); }
};

struct StockTickPayload final : PayloadBase {
  StockTick tick;
  PayloadKind kind() const override { return PayloadKind::kStockTick; }
  std::string type_name() const override { return kind_name(kind()); }
  std::string pack() const override { return pack_poly_payload(kind(), type_name(), tick); }
};

inline std::unique_ptr<PayloadBase> decode_poly_payload(const std::string& payload_bytes) {
  const auto env = unpack_poly_envelope(payload_bytes);
  switch (env.kind) {
    case PayloadKind::kText: {
      auto p = std::make_unique<TextPayload>();
      env.data.convert(p->text);
      return p;
    }
    case PayloadKind::kTask: {
      auto p = std::make_unique<TaskPayload>();
      env.data.convert(p->task);
      return p;
    }
    case PayloadKind::kTaskResult: {
      auto p = std::make_unique<TaskResultPayload>();
      env.data.convert(p->result);
      return p;
    }
    case PayloadKind::kNotice: {
      auto p = std::make_unique<NoticePayload>();
      env.data.convert(p->notice);
      return p;
    }
    case PayloadKind::kStockTick: {
      auto p = std::make_unique<StockTickPayload>();
      env.data.convert(p->tick);
      return p;
    }
    default:
      throw std::runtime_error("poly payload: unknown kind=" + std::to_string(static_cast<std::uint32_t>(env.kind)));
  }
}

}  // namespace demo

