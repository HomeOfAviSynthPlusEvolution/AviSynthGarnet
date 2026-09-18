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
  std::thread::id entry_thread = std::this_thread::get_id();
  bool check_release_thread = false;
  std::atomic<bool> allow_failed_entry{false};
  std::atomic<int> live{0}, released{0}, reentries{0};
  std::atomic<int> functions_created{0}, functions_finalized{0};
  std::atomic<bool> reenter{true};
  bool fail_function = false;
  std::mutex events;
  std::condition_variable event;
  void wait_for_reentries(int count) {
    std::unique_lock<std::mutex> lock(events);
    CHECK(event.wait_for(lock, std::chrono::seconds(10), [&] { return reentries == count; }));
    CHECK(live == 0);
  }
  void wait_for_functions(int count) {
    std::unique_lock<std::mutex> lock(events);
    CHECK(event.wait_for(lock, std::chrono::seconds(10), [&] { return functions_finalized == count; }));
  }
  void wait_for_empty() {
    std::unique_lock<std::mutex> lock(events);
    CHECK(event.wait_for(lock, std::chrono::seconds(10), [&] { return live == 0; }));
  }
};
struct Handle {
  Host& host;
  uint32_t type;
  std::atomic<int> references{1};
  garnet_callback callback = nullptr;
  void* data = nullptr;
  garnet_finalizer finalize = nullptr;
};
void GARNET_CALL release_handle(void* pointer) {
  auto* handle = static_cast<Handle*>(pointer);
  // Check EVERY release, not only the final reference. This catches exception
  // cleanup deterministically even if GC has not discarded the Ruby wrapper.
  CHECK(!handle->host.check_release_thread || std::this_thread::get_id() != handle->host.entry_thread);
  if (--handle->references != 0)
    return;
  auto& host = handle->host;
  --host.live;
  ++host.released;
  if (handle->finalize) {
    handle->finalize(handle->data);
    ++host.functions_finalized;
  }
  delete handle;
  // Publish completion under the condition-variable mutex, including releases
  // that do not perform the destructor-reentry exercise below.
  {
    std::lock_guard<std::mutex> lock(host.events);
  }
  host.event.notify_all();
  if (host.reenter) {
    // Model a filter destructor that joins a frame worker. The worker must be
    // able to enter Ruby and collect again, not time out waiting for the VM.
    std::thread worker([&] {
      auto r = host.callback(host.data, &host, nullptr, 0);
      CHECK((r.status == GARNET_OK && r.value.type == GARNET_INT && r.value.as.integer == 42) ||
            (host.allow_failed_entry && r.status == GARNET_ERROR));
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
  CHECK(false);
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
garnet_result GARNET_CALL invoke_function(void*, void* context, void* pointer, const garnet_value* args,
                                          const garnet_string*, size_t count) {
  auto* handle = static_cast<Handle*>(pointer);
  CHECK(handle->callback);
  return handle->callback(handle->data, context, args, count);
}
garnet_result GARNET_CALL make_function(void* identity, void*, garnet_string, garnet_callback callback, void* data,
                                        garnet_finalizer release_data) {
  auto& host = *static_cast<Host*>(identity);
  ++host.functions_created;
  if (host.fail_function) {
    release_data(data);
    ++host.functions_finalized;
    garnet_result r{};
    r.status = GARNET_ERROR;
    r.error = str("deliberate make failure");
    return r;
  }
  ++host.live;
  auto* handle = new Handle{host, GARNET_FUNCTION};
  handle->callback = callback;
  handle->data = data;
  handle->finalize = release_data;
  return owned(handle);
}
garnet_result evaluate(Host& host, const char* source, bool success = true) {
  auto r = garnet_evaluate(host.session, &host, str(source), str("release.avs.rb"));
  if (success && r.status != GARNET_OK)
    std::fprintf(stderr, "%.*s\n", static_cast<int>(r.error.size), r.error.data);
  CHECK((r.status == GARNET_OK) == success);
  return r;
}
void eval(Host& host, const char* source, bool success = true) {
  release(evaluate(host, source, success));
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
void function_lifetimes() {
  {
    Host host;
    host.reenter = false;
    create(host);
    // f is also in its block's local environment. Pure Ruby tracing must be
    // able to collect this self-reference when no native owner remains.
    eval(host, "def owned_function; clip = AVS.Source; f = AVS.function(returns: :clip) { clip }; f; end; "
               "$f = owned_function; nil");
    CHECK(host.live == 1 && host.functions_created == 0);
    auto first = evaluate(host, "$f");
    auto second = evaluate(host, "$f");
    CHECK(first.value.type == GARNET_FUNCTION && second.value.type == GARNET_FUNCTION);
    CHECK(host.live == 3 && host.functions_created == 2);
    eval(host, "$f = nil; GC.start; nil");
    release(first);
    host.wait_for_functions(1);
    // Both native instances use the SAME Ruby block. Retiring the first root
    // must not unroot the second one (mruby unregister removes all matches).
    eval(host, "GC.start; nil");
    CHECK(host.live == 2);
    auto copy = retain(&host, second.value.as.handle);
    release(second);
    auto output = invoke_function(&host, &host, copy.value.as.handle, nullptr, nullptr, 0);
    CHECK(output.status == GARNET_OK && output.value.type == GARNET_CLIP);
    release(output);
    release(copy);
    host.wait_for_functions(2);
    eval(host, "GC.start; nil");
    host.wait_for_empty();
    CHECK(host.released == 3);
    garnet_destroy(host.session);
  }
  {
    Host host;
    host.reenter = false;
    create(host);
    eval(host, "def temporary_function(n); clip = AVS.Source; "
               "f = AVS.function(args: {value: :int}, returns: :int) { |value| "
               "raise unless clip.is_a?(AVS::Clip); value + 1 }; "
               "raise unless f.call(n) == n + 1; nil; end; "
               "6000.times { |n| temporary_function(n); GC.start if n % 32 == 0 }; nil");
    host.wait_for_functions(6000);
    eval(host, "GC.start; nil");
    host.wait_for_empty();
    CHECK(host.functions_created == 6000 && host.released == 12000);
    garnet_destroy(host.session);
  }
  {
    Host host;
    host.reenter = false;
    host.fail_function = true;
    create(host);
    eval(host, "def failed_function; clip = AVS.Source; f = AVS.function { clip }; "
               "begin; f.call; raise 'missing failure'; rescue => e; "
               "raise unless e.message.include?('deliberate make failure'); end; nil; end; "
               "failed_function; nil");
    eval(host, "GC.start; nil");
    host.wait_for_empty();
    CHECK(host.functions_created == 1 && host.functions_finalized == 1);
    garnet_destroy(host.session);
  }
  {
    Host host;
    host.reenter = false;
    create(host);
    auto output = evaluate(host, "clip = AVS.Source; AVS.function { clip }");
    CHECK(output.value.type == GARNET_FUNCTION);
    auto first = retain(&host, output.value.as.handle);
    auto second = retain(&host, output.value.as.handle);
    release(output);
    // Model AVS globals releasing captured values AFTER AtExit destroys Ruby.
    // No callback is permitted now, but the lease finalizer must remain safe.
    garnet_destroy(host.session);
    CHECK(host.live == 1 && host.functions_finalized == 0);
    std::thread a([&] { release(first); });
    std::thread b([&] { release(second); });
    a.join();
    b.join();
    CHECK(host.live == 0 && host.functions_finalized == 1 && host.released == 2);
  }
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
  // Cleanup may now run at a host boundary, before the outer evaluation ends.
  eval(host, "$clip = nil; AVS.Nested; nil");
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
  {
    Host mismatch;
    mismatch.check_release_thread = true;
    create(mismatch);
    eval(mismatch, "$fn = AVS.function(returns: :int) { AVS.Source }; "
                   "begin; $fn.call; raise 'missing mismatch'; rescue => e; "
                   "raise unless e.message.include?('return type mismatch'); end; GC.start; 42");
    mismatch.wait_for_reentries(2); // No native handle is cached on $fn.
    mismatch.reenter = false;
    garnet_destroy(mismatch.session);
    CHECK(mismatch.live == 0 && mismatch.released == 2);
  }
  for (const auto* source : {"[AVS.Source, Object.new]", "AVS.call(:Unused, [AVS.Source, Object.new])"}) {
    Host partial;
    partial.check_release_thread = true;
    partial.allow_failed_entry = true;
    create(partial);
    eval(partial, source, false);
    partial.reenter = false;
    garnet_destroy(partial.session);
    CHECK(partial.live == 0 && partial.released == 1);
  }
  function_lifetimes();
  std::puts("PASS deferred release, reentry, 6000 temporary functions, shared roots, failure and late finalization");
  return 0;
}
