#include <garnet/engine.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

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
void check(garnet_result r, int expected) {
  if (r.status != GARNET_OK)
    std::fprintf(stderr, "%.*s\n", static_cast<int>(r.error.size), r.error.data);
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_INT && r.value.as.integer == expected);
  release(r);
}
struct Host {
  garnet_session* session = nullptr;
  garnet_callback callback = nullptr;
  void* data = nullptr;
  std::mutex events;
  std::condition_variable event;
  bool a_parked = false, b_parked = false, release_b = false;
  bool importing = false, finish_import = false;
  int entrants = 0, imported = 0;
};
struct Context {
  std::thread::id thread = std::this_thread::get_id();
  int value = 0;
};
garnet_result call(Host& host, int mode) {
  Context context;
  garnet_value arg{};
  arg.type = GARNET_INT;
  arg.as.integer = mode;
  return host.callback(host.data, &context, &arg, 1);
}
garnet_result GARNET_CALL invoke(void* identity, void* context, garnet_string name, const garnet_value*,
                                 const garnet_string*, size_t) {
  auto& host = *static_cast<Host*>(identity);
  CHECK(static_cast<Context*>(context)->thread == std::this_thread::get_id());
  const std::string_view filter(name.data, name.size);
  if (filter == "CrossThread") {
    // A waits synchronously for B, which needs the SAME Ruby VM. Previously
    // B timed out after five seconds because A held the VM lock.
    std::thread worker([&] {
      auto r = call(host, 0);
      CHECK(r.status == GARNET_OK && r.value.type == GARNET_INT);
      release(r);
    });
    worker.join();
  } else if (filter == "SameThread") {
    auto r = call(host, 0);
    CHECK(r.status == GARNET_OK);
    release(r);
  } else if (filter == "BeforeImport" || filter == "InitializeImport" || filter == "AfterImport") {
    std::unique_lock<std::mutex> lock(host.events);
    if (filter == "BeforeImport")
      ++host.entrants;
    else if (filter == "AfterImport")
      ++host.imported;
    else
      host.importing = true;
    host.event.notify_all();
    if (filter == "InitializeImport")
      CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.finish_import; }));
  } else if (filter == "NestedFailure") {
    return call(host, 6);
  } else if (filter == "Failure") {
    garnet_result r{};
    r.status = GARNET_ERROR;
    r.error = str("native failure");
    return r;
  } else if (filter == "ParkA" || filter == "ParkB") {
    std::unique_lock<std::mutex> lock(host.events);
    if (filter == "ParkA") {
      host.a_parked = true;
      host.event.notify_all();
      CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.b_parked; }));
    } else {
      host.b_parked = true;
      host.event.notify_all();
      CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.release_b; }));
    }
  } else {
    CHECK(filter == "Pause");
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
garnet_result GARNET_CALL get(void*, void* context, garnet_string) {
  auto& c = *static_cast<Context*>(context);
  CHECK(c.thread == std::this_thread::get_id());
  garnet_result r{};
  r.value.type = GARNET_INT;
  r.value.as.integer = c.value;
  return r;
}
garnet_result GARNET_CALL set(void*, void* context, garnet_string, const garnet_value* value, int) {
  auto& c = *static_cast<Context*>(context);
  CHECK(c.thread == std::this_thread::get_id() && value->type == GARNET_INT);
  c.value = static_cast<int>(value->as.integer);
  return {};
}
garnet_result GARNET_CALL invoke_function(void*, void*, void*, const garnet_value*, const garnet_string*, size_t) {
  CHECK(false);
  return {};
}
garnet_result GARNET_CALL make_function(void*, void*, garnet_string, garnet_callback, void*, garnet_finalizer) {
  CHECK(false);
  return {};
}
} // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  const auto filename = (std::filesystem::u8path(argv[1]) / "concurrency.avs.rb").u8string();
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
  auto r = garnet_create(&api, &host.session);
  CHECK(r.status == GARNET_OK);
  release(r);
  Context context;
  r = garnet_evaluate(host.session, &context, str(R"RUBY(
counter = 0
shared = []
AVS.export('Count', 'i') do |mode|
  AVS[:local] = mode
  if mode == 0
    shared << 'nested'
    GC.start
  elsif mode == 1
    AVS.Pause
  elsif mode == 2
    AVS.CrossThread
  elsif mode == 3
    original = shared
    AVS.SameThread
    raise 'identity lost' unless original.equal?(shared) && shared[-1] == 'nested'
    a = [3,1,2].sort { |x,y| AVS.CrossThread; x <=> y }
    raise 'sort' unless a == [1,2,3]
    a = Array.new(3) { |i| AVS.CrossThread; "item#{i}" }
    raise 'Array.new' unless a == ['item0','item1','item2']
    h = Hash.new { |hash,key| AVS.CrossThread; hash[key] = 'default' }
    raise 'Hash default' unless h[:key] == 'default'
    raise 'index' unless a.index { |v| AVS.CrossThread; v == 'item1' } == 1
    begin
      begin
        raise 'pending Ruby error'
      ensure
        AVS.CrossThread
      end
    rescue => e
      raise 'lost pending exception' unless e.message == 'pending Ruby error'
    end
    begin
      AVS.Failure
      raise 'missing native error'
    rescue => e
      raise 'native error' unless e.message == 'native failure'
    ensure
      AVS.CrossThread
    end
    GC.start
    raise 'call context changed' unless AVS[:local] == mode
    next 42
  elsif mode == 8 || mode == 9
    AVS.BeforeImport
    loaded = require_relative(mode == 8 ? 'concurrent-library.rb' : 'concurrent-failure.rb')
    raise 'incomplete library' unless $library_count == 1 && $library_ready
    AVS.AfterImport
    next loaded ? 1 : 0
  elsif mode == 6
    raise 'ordinary callback failure'
  elsif mode == 7
    begin
      AVS.NestedFailure
      raise 'missing nested exception'
    rescue => e
      raise 'wrong nested error' unless e.message.include?('ordinary callback failure')
    end
    GC.start
    next 42
  elsif mode == 4 || mode == 5
    # A and B both park, then A exits and frees its arena BEFORE B resumes.
    a = Array.new(3) { |i| "#{mode}-#{i}" }
    mode == 4 ? AVS.ParkA : AVS.ParkB
    GC.start
    raise 'lost parked roots' unless a == ["#{mode}-0", "#{mode}-1", "#{mode}-2"]
    raise 'call context changed' unless AVS[:local] == mode
    next mode
  end
  raise 'call context changed' unless AVS[:local] == mode
  counter += 1
end
nil
)RUBY"),
                      str(filename.c_str()));
  if (r.status != GARNET_OK)
    std::fprintf(stderr, "%.*s\n", static_cast<int>(r.error.size), r.error.data);
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
  check(call(host, 2), 102);
  check(call(host, 0), 103);
  check(call(host, 3), 42);
  check(call(host, 7), 42);
  r = call(host, 6);
  CHECK(r.status == GARNET_ERROR);
  release(r);
  check(call(host, 7), 42);
  std::thread a([&] { check(call(host, 4), 4); });
  {
    std::unique_lock<std::mutex> lock(host.events);
    CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.a_parked; }));
  }
  std::thread b([&] { check(call(host, 5), 5); });
  a.join();
  // Evaluation must be rejected even when the VM is temporarily idle.
  r = garnet_evaluate(host.session, &context, str("42"), str("busy.rb"));
  CHECK(r.status == GARNET_BUSY);
  release(r);
  r = call(host, 6); // Failure in A must not poison B's parked invocation.
  CHECK(r.status == GARNET_ERROR);
  release(r);
  r = call(host, 0); // GC while B owns a suspended invocation's arena.
  CHECK(r.status == GARNET_OK);
  release(r);
  {
    std::lock_guard<std::mutex> lock(host.events);
    host.release_b = true;
  }
  host.event.notify_all();
  b.join();
  std::thread initializer([&] { check(call(host, 8), 1); });
  {
    std::unique_lock<std::mutex> lock(host.events);
    CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.importing; }));
  }
  std::thread waiter([&] { check(call(host, 8), 0); });
  {
    std::unique_lock<std::mutex> lock(host.events);
    CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.entrants == 2; }));
    CHECK(!host.event.wait_for(lock, std::chrono::milliseconds(50), [&] { return host.imported != 0; }));
    host.finish_import = true;
  }
  host.event.notify_all();
  initializer.join();
  waiter.join();
  check(call(host, 8), 0);
  {
    std::lock_guard<std::mutex> lock(host.events);
    host.importing = host.finish_import = false;
    host.entrants = host.imported = 0;
  }
  const auto fail_import = [&] {
    auto failure = call(host, 9);
    CHECK(failure.status == GARNET_ERROR);
    release(failure);
  };
  std::thread failing_initializer(fail_import);
  {
    std::unique_lock<std::mutex> lock(host.events);
    CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.importing; }));
  }
  std::thread failing_waiter(fail_import);
  {
    std::unique_lock<std::mutex> lock(host.events);
    CHECK(host.event.wait_for(lock, std::chrono::seconds(5), [&] { return host.entrants == 2; }));
    host.event.wait_for(lock, std::chrono::milliseconds(50));
    host.finish_import = true;
  }
  host.event.notify_all();
  failing_initializer.join();
  failing_waiter.join();
  CHECK(host.imported == 0);
  garnet_destroy(host.session);
  std::puts("PASS host handoff, original thread/context, GC, C blocks, exceptions and non-LIFO completion");
  return 0;
}
