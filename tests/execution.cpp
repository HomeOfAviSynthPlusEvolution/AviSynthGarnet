#include "execution.hpp"
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/string.h>
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                             \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

namespace {
size_t allocations = 0, live_allocations = 0;
mrb_value source(mrb_state* m, void* text) {
  return mrb_load_string(m, static_cast<const char*>(text));
}
void evaluate(garnet::Execution& vm, std::unique_lock<std::recursive_timed_mutex>& lock, const char* text,
              bool success = true) {
  garnet::Invocation invocation(vm, lock, nullptr);
  mrb_bool failed = false;
  mrb_protect_error(vm.ruby, source, const_cast<char*>(text), &failed);
  CHECK((!failed && !vm.ruby->exc) == success);
}
void nested(garnet::Execution& vm, std::unique_lock<std::recursive_timed_mutex>& lock, int depth) {
  garnet::Invocation invocation(vm, lock, nullptr);
  if (depth) {
    garnet::OutsideVM outside(invocation);
    lock.lock();
    nested(vm, lock, depth - 1);
    lock.unlock();
  }
}
} // namespace

// mruby 4's supported allocator override: only this test executable uses it.
extern "C" void* mrb_basic_alloc_func(void* p, size_t bytes) {
  if (!bytes) {
    if (p)
      --live_allocations;
    std::free(p);
    return nullptr;
  }
  auto* result = std::realloc(p, bytes);
  if (result) {
    ++allocations;
    if (!p)
      ++live_allocations;
  }
  return result;
}

int main() {
  auto* m = mrb_open();
  CHECK(MRB_OPEN_SUCCESS(m));
  garnet::Execution vm;
  vm.ruby = m;
  std::recursive_timed_mutex gate;
  std::unique_lock<std::recursive_timed_mutex> lock(gate);
  {
    garnet::Invocation warmup(vm, lock, nullptr);
  }
  CHECK(vm.pooled == 1);
  const auto count = allocations;
  for (int i = 0; i < 10000; ++i) {
    garnet::Invocation invocation(vm, lock, nullptr);
    CHECK(!m->exc && !m->jmp && m->gc.arena_idx == 0);
    CHECK(!mrb_vm_ci_env(m->c->ci));
  }
  CHECK(allocations == count && vm.pooled == 1);
  {
    garnet::Invocation invocation(vm, lock, nullptr);
    {
      garnet::OutsideVM warmup(invocation);
    }
    const auto count = allocations;
    for (int i = 0; i < 100; ++i) {
      garnet::OutsideVM handoff(invocation);
    }
    CHECK(allocations == count);
  }

  evaluate(vm, lock, "value = 'survives reuse'; $saved = -> { value }; nil");
  for (int i = 0; i < 20; ++i) {
    evaluate(vm, lock, "raise 'recoverable'", false);
    evaluate(vm, lock, "GC.start; raise 'closure changed' unless $saved.call == 'survives reuse'; nil");
  }
  {
    garnet::Invocation outer(vm, lock, nullptr);
    const auto temporary = mrb_str_new_cstr(m, "arena-only root");
    {
      garnet::OutsideVM outside(outer);
      lock.lock();
      evaluate(vm, lock, "GC.start; nil");
      lock.unlock();
    }
    CHECK(std::strcmp(RSTRING_PTR(temporary), "arena-only root") == 0);
  }
  nested(vm, lock, 12);
  CHECK(vm.calls.empty() && !vm.current && vm.pooled == vm.pool.size());
  vm.clear();
  CHECK(vm.pooled == 0);

  // A rare oversized script must not permanently inflate the idle pool.
  {
    garnet::Invocation large(vm, lock, nullptr);
    auto* c = m->c;
    c->stbase = static_cast<mrb_value*>(mrb_realloc(m, c->stbase, 8192 * sizeof(mrb_value)));
    c->stend = c->stbase + 8192;
    c->ci->stack = c->stbase;
    std::fill(c->stbase, c->stend, mrb_nil_value());
  }
  CHECK(vm.pooled == 0);
  evaluate(vm, lock, "$saved = nil; GC.start; nil");
  vm.clear();
  mrb_close(m);
  CHECK(live_allocations == 0);
  std::puts("PASS bounded context pool, zero warm entry allocations, escaped closures, exceptions, roots and close");
}
