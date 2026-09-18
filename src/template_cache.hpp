#pragma once
#include <cstddef>
#include <limits>
#include <list>
#include <optional>
#include <string>
#include <utility>

namespace garnet {
// Caller provides synchronization. Values must be capture-free factories, not
// closures owning live Ruby callbacks. Most workloads hit the first few items.
template <class Value>
class TemplateCache {
  std::list<std::pair<std::string, Value>> entries;
  size_t capacity;

public:
  explicit TemplateCache(size_t limit = 128) : capacity(limit ? limit : 1) {}
  std::optional<Value> find(const std::string& key) {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it->first == key) {
        entries.splice(entries.begin(), entries, it);
        return entries.front().second;
      }
    }
    return std::nullopt;
  }
  Value insert(const std::string& key, Value value) {
    if (auto existing = find(key))
      return *existing;
    entries.emplace_front(key, std::move(value));
    if (entries.size() > capacity)
      entries.pop_back();
    return entries.front().second;
  }
  size_t size() const noexcept { return entries.size(); }
  void keep_parsed() noexcept { capacity = (std::numeric_limits<size_t>::max)(); }
};
inline std::string template_key(std::string signature) {
  // Parameter types remain strict; only AVS keyword spelling is insensitive.
  bool keyword = false;
  for (auto& c : signature) {
    if (c == '[')
      keyword = true;
    else if (c == ']')
      keyword = false;
    else if (keyword && c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  }
  return signature;
}
} // namespace garnet
