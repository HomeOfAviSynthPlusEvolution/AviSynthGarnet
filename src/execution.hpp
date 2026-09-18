#pragma once
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/proc.h>
#include <mruby/version.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

// This is an embedding contract with the pinned mruby, not a Fiber scheduler.
// Native stacks stay on their calling threads; only VM ownership is exchanged.
static_assert(MRUBY_RELEASE_MAJOR == 4 && MRUBY_RELEASE_MINOR == 0, "Re-audit execution contexts when upgrading mruby");
#if defined(MRB_GC_FIXED_ARENA) || defined(MRB_USE_TASK_SCHEDULER)
#error Garnet execution contexts require dynamic GC arenas and no task scheduler
#endif

namespace garnet {
struct ExecutionState {
  mrb_context* context = nullptr;
  mrb_jmpbuf* jump = nullptr;
  RObject* exception = nullptr;
  RBasic** arena = nullptr;
  int size = 0, capacity = 0;
  static ExecutionState save(mrb_state* m) noexcept {
    assert(m->c == m->root_c);
    return {m->c, m->jmp, m->exc, m->gc.arena, m->gc.arena_idx, m->gc.arena_capa};
  }
  void install(mrb_state* m) const noexcept {
    m->c = m->root_c = context;
    m->jmp = jump;
    m->exc = exception;
    m->gc.arena = arena;
    m->gc.arena_idx = size;
    m->gc.arena_capa = capacity;
  }
};

struct Invocation;
struct Execution {
  mrb_state* ruby = nullptr;
  ExecutionState idle;
  std::vector<Invocation*> calls;
  Invocation* current = nullptr;
  void chain() noexcept;
};

struct Invocation {
  Execution& vm;
  std::unique_lock<std::recursive_timed_mutex>& lock;
  ExecutionState state;
  mrb_value roots = mrb_nil_value();
  void* call_context;
  unsigned depth;
  Invocation* previous;
  inline static thread_local Invocation* caller = nullptr;

  static Invocation* parent(const Execution& vm) noexcept {
    for (auto* p = caller; p; p = p->previous)
      if (&p->vm == &vm)
        return p;
    return nullptr;
  }
  Invocation(Execution& execution, std::unique_lock<std::recursive_timed_mutex>& guard, void* context)
      : vm(execution), lock(guard), call_context(context), depth(parent(vm) ? parent(vm)->depth + 1 : 1),
        previous(caller) {
    assert(lock.owns_lock() && !vm.current);
    if (depth > 64 || vm.calls.size() >= 256)
      throw std::runtime_error("Ruby invocation limit exceeded");
    // Reserve before allocating mruby-owned buffers, so publishing cannot fail.
    vm.calls.reserve(vm.calls.size() + 1);
    auto* m = vm.ruby;
    const auto allocate = [m](size_t bytes) {
      auto* p = mrb_malloc_simple(m, bytes);
      if (!p)
        throw std::bad_alloc();
      std::memset(p, 0, bytes);
      return p;
    };
    try {
      state.context = static_cast<mrb_context*>(allocate(sizeof(mrb_context)));
      auto* c = state.context;
      c->stbase = static_cast<mrb_value*>(allocate(128 * sizeof(mrb_value)));
      c->stend = c->stbase + 128;
      std::fill(c->stbase, c->stend, mrb_nil_value());
      c->cibase = static_cast<mrb_callinfo*>(allocate(32 * sizeof(mrb_callinfo)));
      c->ci = c->cibase;
      c->ciend = c->cibase + 32;
      c->ci->stack = c->stbase;
      c->ci->u.target_class = m->object_class;
      c->ci->vis = MRB_METHOD_PUBLIC_FL;
      c->status = MRB_FIBER_RUNNING;
      state.arena = static_cast<RBasic**>(allocate(MRB_GC_ARENA_SIZE * sizeof(RBasic*)));
      state.capacity = MRB_GC_ARENA_SIZE;
    } catch (...) {
      mrb_free_context(m, state.context);
      throw;
    }
    vm.idle = ExecutionState::save(m);
    vm.calls.push_back(this);
    state.install(m);
    vm.current = this;
    caller = this;
    vm.chain();
  }
  Invocation(const Invocation&) = delete;
  void detach() {
    assert(vm.current == this && caller == this && lock.owns_lock());
    auto* m = vm.ruby;
    const int count = m->gc.arena_idx;
    // Arena entries include temporaries held only on C stacks (sort, Hash
    // default blocks, etc.). Stack scanning alone cannot keep those alive.
    auto keep = mrb_ary_new_capa(m, static_cast<mrb_int>(count) + 1);
    for (int i = 0; i < count; ++i)
      mrb_ary_push(m, keep, mrb_obj_value(m->gc.arena[i]));
    if (m->exc)
      mrb_ary_push(m, keep, mrb_obj_value(m->exc));
    mrb_gc_register(m, keep);
    roots = keep;
    mrb_gc_arena_restore(m, count);
    state = ExecutionState::save(m);
    vm.idle.install(m);
    vm.current = nullptr;
    vm.chain();
    lock.unlock();
  }
  void attach() {
    // Resumption must not time out: our native stack still owns this context.
    lock.lock();
    assert(!vm.current && caller == this);
    vm.idle = ExecutionState::save(vm.ruby);
    state.install(vm.ruby);
    vm.current = this;
    vm.chain();
    mrb_gc_unregister(vm.ruby, roots);
    roots = mrb_nil_value();
  }
  ~Invocation() {
    assert(vm.current == this && lock.owns_lock() && caller == this);
    auto* m = vm.ruby;
    // Protection unwinds to the entry frame, including on Ruby exceptions.
    assert(m->c->ci == m->c->cibase);
    auto* env = mrb_vm_ci_env(m->c->ci);
    if (env && MRB_ENV_ONSTACK_P(env))
      mrb_env_unshare(m, env, true); // Never throw from stack cleanup.
    state = ExecutionState::save(m);
    vm.idle.install(m);
    vm.current = nullptr;
    caller = previous;
    vm.calls.erase(std::find(vm.calls.begin(), vm.calls.end(), this));
    vm.chain();
    mrb_free(m, state.arena);
    mrb_free_context(m, state.context);
  }
};
inline void Execution::chain() noexcept {
  // No Fiber/Task gem is enabled. prev links make *every* suspended context
  // visible to both GC root scans, even if another callback updates a closure
  // whose environment still points into a suspended stack.
  mrb_context* previous = nullptr;
  for (auto* call : calls) {
    if (call == current)
      continue;
    call->state.context->prev = previous;
    previous = call->state.context;
  }
  if (current) {
    idle.context->prev = previous;
    previous = idle.context;
  }
  ruby->c->prev = previous;
}
struct OutsideVM {
  Invocation& invocation;
  explicit OutsideVM(Invocation& call) : invocation(call) { invocation.detach(); }
  ~OutsideVM() { invocation.attach(); }
};
} // namespace garnet
