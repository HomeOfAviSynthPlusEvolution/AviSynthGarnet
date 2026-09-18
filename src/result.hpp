#pragma once
#include <garnet/host.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace garnet {
inline garnet_string span(const std::string& s) {
  return {s.data(), s.size()};
}
inline std::string text(garnet_string s) {
  if (!s.data && s.size)
    throw std::invalid_argument("Null string span");
  return s.size ? std::string(s.data, s.size) : std::string();
}
/* Internal C++ helpers, compiled independently on each producer side. */
struct Storage {
  garnet_value value{};
  std::string string;
  std::vector<std::unique_ptr<Storage>> children;
  std::vector<garnet_value> elements;
  garnet_host host{};
  garnet_result pin{};
  ~Storage() {
    if (pin.release)
      pin.release(pin.owner);
    if (value.type == GARNET_CLIP && value.as.handle && host.release_clip)
      host.release_clip(host.identity, value.as.handle);
  }
  void set_string(std::string s) {
    string = std::move(s);
    value.type = GARNET_STRING;
    value.as.string = span(string);
  }
  void finish_array() {
    for (const auto& child : children)
      elements.push_back(child->value);
    value.type = GARNET_ARRAY;
    value.as.array = {elements.data(), elements.size()};
  }
};
inline void GARNET_CALL release_storage(void* p) {
  delete static_cast<Storage*>(p);
}
inline garnet_result result(std::unique_ptr<Storage> p) {
  auto value = p->value;
  return {GARNET_OK, value, {}, p.release(), release_storage};
}
inline garnet_result error(const char* message, uint32_t status = GARNET_ERROR) noexcept {
  try {
    auto p = std::make_unique<Storage>();
    p->string = message;
    auto s = span(p->string);
    return {status, {}, s, p.release(), release_storage};
  } catch (...) {
    static const char fallback[] = "Garnet: cannot allocate error";
    return {status, {}, {fallback, sizeof(fallback) - 1}, nullptr, nullptr};
  }
}
struct ResultGuard {
  garnet_result value;
  explicit ResultGuard(garnet_result r) : value(r) {}
  ResultGuard(const ResultGuard&) = delete;
  ~ResultGuard() {
    if (value.release)
      value.release(value.owner);
  }
  void check() const {
    if (value.status != GARNET_OK)
      throw std::runtime_error(text(value.error));
  }
};
} // namespace garnet
