#include "template_cache.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>

#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                             \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)
struct Value {
  static inline int live = 0;
  Value() { ++live; }
  ~Value() { --live; }
};
int main() {
  {
    garnet::TemplateCache<std::shared_ptr<Value>> cache(3);
    auto retained = cache.insert("a", std::make_shared<Value>());
    cache.insert("b", std::make_shared<Value>());
    cache.insert("c", std::make_shared<Value>());
    CHECK(cache.find("a").value() == retained);
    cache.insert("d", std::make_shared<Value>());
    CHECK(!cache.find("b") && cache.size() == 3 && Value::live == 3);
    CHECK(cache.insert("a", std::make_shared<Value>()) == retained);
    // A by-value parameter may be destroyed at the end of the full expression.
    CHECK(Value::live == 3);
    for (int i = 0; i < 10000; ++i)
      cache.insert(std::to_string(i), std::make_shared<Value>());
    CHECK(cache.size() == 3 && Value::live == 4); // Eviction does not invalidate retained values.
    retained.reset();
    CHECK(Value::live == 3);
  }
  CHECK(Value::live == 0);
  {
    garnet::TemplateCache<int> cache(0); // Minimum useful capacity is one.
    cache.insert("a", 1);
    CHECK(cache.insert("b", 2) == 2 && cache.size() == 1 && !cache.find("a"));
  }
  {
    garnet::TemplateCache<int> cache(2);
    cache.keep_parsed();
    for (int i = 0; i < 10; ++i)
      cache.insert(std::to_string(i), i);
    CHECK(cache.size() == 10 && cache.find("0").value() == 0);
  }
  CHECK(garnet::template_key("c[WIDTH]i[Height]i") == "c[width]i[height]i");
  CHECK(garnet::template_key("I[Width]F") == "I[width]F");
  std::puts("PASS template capacity, recency, duplicate insert, retained lifetime and keyword canonicalization");
}
