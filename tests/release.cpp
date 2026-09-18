#include <garnet/engine.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                             \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {
garnet_string str(const char* value) {
  return {value, std::strlen(value)};
}
void release(garnet_result r) {
  if (r.release)
    r.release(r.owner);
}
struct Host {
  garnet_session* session = nullptr;
  garnet_callback callback = nullptr;
  void* data = nullptr;
  std::atomic<int> live{0}, released{0}, reentries{0};
  std::atomic<bool> reenter{true};
  int expected_released = 0;
  std::mutex events;
  std::condition_variable event;
  void wait_for_reentries(int count) {
    std::unique_lock<std::mutex> lock(events);
    CHECK(event.wait_for(lock, std::chrono::seconds(10), [&] { return reentries == count; }));
    CHECK(live == 0);
  }
};
struct Handle {
  Host& host;
  uint32_t type;
  std::atomic<int> references{1};
};
void GARNET_CALL release_handle(void* pointer) {
  auto* handle = static_cast<Handle*>(pointer);
  if (--handle->references != 0)
    return;
  auto& host = handle->host;
  --host.live;
  ++host.released;
  delete handle;
  if (host.reenter) {
    // Model a filter destructor that joins a frame worker. The worker must be
    // able to enter Ruby and collect again, not time out waiting for the VM.
    std::thread worker([&] {
      auto r = host.callback(host.data, &host, nullptr, 0);
      CHECK(r.status == GARNET_OK && r.value.type == GARNET_INT && r.value.as.integer == 42);
      release(r);
      {
        std::lock_guard<std::mutex> lock(host.events);
        ++host.reentries;
      }
      host.event.notify_all();
    });
    worker.join();
  }
}
garnet_result owned(Handle* handle) {
  garnet_result r{};
  r.value.type = handle->type;
  r.value.as.handle = handle;
  r.owner = handle;
  r.release = release_handle;
  return r;
}
garnet_result GARNET_CALL retain(void* identity, void* pointer) {
  auto* handle = static_cast<Handle*>(pointer);
  CHECK(&handle->host == identity);
  ++handle->references;
  return owned(handle);
}
void GARNET_CALL free_handle(void*, void* pointer) {
  release_handle(pointer);
}
garnet_result GARNET_CALL invoke(void* identity, void* context, garnet_string name, const garnet_value*,
                                 const garnet_string*, size_t count) {
  auto& host = *static_cast<Host*>(identity);
  CHECK(context == identity && count == 0);
  const std::string_view filter(name.data, name.size);
  if (filter == "Source" || filter == "Lambda") {
    ++host.live;
    return owned(new Handle{host, static_cast<uint32_t>(filter == "Source" ? GARNET_CLIP : GARNET_FUNCTION)});
  }
  if (filter == "Nested")
    return host.callback(host.data, &host, nullptr, 0);
  CHECK(filter == "AssertNotReleased");
  CHECK(host.released == host.expected_released);
  return {};
}
garnet_result GARNET_CALL register_filter(void* identity, void*, garnet_string, garnet_string, garnet_callback callback,
                                          void* data) {
  auto& host = *static_cast<Host*>(identity);
  CHECK(!host.callback);
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
void eval(Host& host, const char* source, bool success = true) {
  auto r = garnet_evaluate(host.session, &host, str(source), str("release.avs.rb"));
  if (success && r.status != GARNET_OK)
    std::fprintf(stderr, "%.*s\n", static_cast<int>(r.error.size), r.error.data);
  CHECK((r.status == GARNET_OK) == success);
  release(r);
}
void create(Host& host) {
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
  auto r = garnet_create(&api, &host.session);
  CHECK(r.status == GARNET_OK);
  release(r);
  eval(host, "AVS.export('Reenter', '') { GC.start; 42 }; nil");
}
} // namespace

int main() {
  Host host;
  create(host);
  eval(host, "$clip = AVS.Source; $fn = AVS.Lambda; nil");
  CHECK(host.live == 2);
  eval(host, "$clip = nil; $fn = nil; GC.start; nil");
  host.wait_for_reentries(2);
  CHECK(host.live == 0 && host.reentries == 2);

  eval(host, "$clip = AVS.Source; nil");
  host.expected_released = host.released;
  eval(host, "$clip = nil; AVS.Nested; AVS.AssertNotReleased; nil");
  host.wait_for_reentries(3);
  CHECK(host.live == 0 && host.reentries == 3);

  eval(host, "$clip = AVS.Source; $fn = AVS.Lambda; nil");
  CHECK(host.live == 2);
  host.reenter = false; // The host contract forbids callbacks during destruction.
  garnet_destroy(host.session);
  CHECK(host.live == 0 && host.released == 5);

  Host failed;
  create(failed);
  eval(failed, "$clip = AVS.Source; $fn = AVS.Lambda; nil");
  failed.reenter = false;
  eval(failed, "$clip = nil; $fn = nil; GC.start; raise 'deliberate failure'", false);
  garnet_destroy(failed.session);
  CHECK(failed.live == 0 && failed.released == 2);
  std::puts("PASS deferred clip/function release, cross-thread destructor reentry, nesting, failure and close");
  return 0;
}
