#include <garnet/engine.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                                \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {
garnet_string str(const char* text) {
  return {text, std::strlen(text)};
}
void release(garnet_result r) {
  if (r.release)
    r.release(r.owner);
}
struct Host {
  garnet_callback callback = nullptr;
  void* data = nullptr;
};
garnet_result call(Host& host, int mode) {
  garnet_value arg{};
  arg.type = GARNET_INT;
  arg.as.integer = mode;
  return host.callback(host.data, &host, &arg, 1);
}
garnet_result GARNET_CALL invoke(void* identity, void*, garnet_string name, const garnet_value*, const garnet_string*,
                                 size_t) {
  auto& host = *static_cast<Host*>(identity);
  if (std::string_view(name.data, name.size) == "CrossThread") {
    // Outer Ruby holds the gate while the host waits synchronously on a worker.
    // The worker must time out, not hang or enter the running mruby VM.
    std::thread worker([&] {
      const auto start = std::chrono::steady_clock::now();
      auto r = call(host, 0);
      CHECK(r.status == GARNET_BUSY);
      CHECK(std::string_view(r.error.data, r.error.size).find("5 seconds") != std::string_view::npos);
      CHECK(std::chrono::steady_clock::now() - start >= std::chrono::seconds(4));
      release(r);
    });
    worker.join();
  } else {
    CHECK(std::string_view(name.data, name.size) == "Pause");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {};
}
garnet_result GARNET_CALL retain(void*, void*) {
  CHECK(false);
  return {};
}
void GARNET_CALL free_handle(void*, void*) {
  CHECK(false);
}
garnet_result GARNET_CALL register_filter(void* identity, void*, garnet_string, garnet_string, garnet_callback callback,
                                          void* data) {
  auto& host = *static_cast<Host*>(identity);
  host.callback = callback;
  host.data = data;
  return {};
}
garnet_result GARNET_CALL get(void*, void*, garnet_string) {
  CHECK(false);
  return {};
}
garnet_result GARNET_CALL set(void*, void*, garnet_string, const garnet_value*, int) {
  CHECK(false);
  return {};
}
garnet_result GARNET_CALL invoke_function(void*, void*, void*, const garnet_value*, const garnet_string*, size_t) {
  CHECK(false);
  return {};
}
garnet_result GARNET_CALL make_function(void*, void*, garnet_string, garnet_callback, void*) {
  CHECK(false);
  return {};
}
} // namespace

int main() {
  Host host;
  garnet_host api{GARNET_CONTRACT_REVISION,
                  sizeof(garnet_host),
                  &host,
                  invoke,
                  retain,
                  free_handle,
                  register_filter,
                  get,
                  set,
                  retain,
                  free_handle,
                  invoke_function,
                  make_function};
  garnet_session* session = nullptr;
  auto r = garnet_create(&api, &session);
  CHECK(r.status == GARNET_OK);
  release(r);
  r = garnet_evaluate(session, &host,
                      str("counter = 0\n"
                          "AVS.export('Count', 'i') do |mode|\n"
                          "  if mode == 2\n"
                          "    AVS.CrossThread\n"
                          "  elsif mode == 1\n"
                          "    AVS.Pause\n"
                          "  end\n"
                          "  counter += 1\n"
                          "end\n"
                          "nil\n"),
                      str("concurrency.avs.rb"));
  CHECK(r.status == GARNET_OK && host.callback);
  release(r);
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i)
    workers.emplace_back([&] {
      ++ready;
      while (!start.load())
        std::this_thread::yield();
      for (int j = 0; j < 25; ++j) {
        auto result = call(host, 1);
        CHECK(result.status == GARNET_OK && result.value.type == GARNET_INT);
        release(result);
      }
    });
  while (ready.load() != 4)
    std::this_thread::yield();
  start = true;
  for (auto& worker : workers)
    worker.join();
  r = call(host, 2);
  CHECK(r.status == GARNET_OK && r.value.as.integer == 101);
  release(r);
  // A rejected concurrent entry must not poison the owner or later callbacks.
  r = call(host, 0);
  CHECK(r.status == GARNET_OK && r.value.as.integer == 102);
  release(r);
  garnet_destroy(session);
}
