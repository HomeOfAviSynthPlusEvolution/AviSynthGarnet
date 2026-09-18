#include <garnet/engine.h>
#include "result.hpp"
#include "runtime.hpp"
#include "execution.hpp"
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/compile.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#include <mruby/proc.h>
#include <mruby/irep.h>
#include <mruby/debug.h>
#include <filesystem>
#include <charconv>
#include <fstream>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <unordered_set>

static_assert(sizeof(mrb_int) == 8, "Garnet requires 64-bit mruby integers");
static_assert(sizeof(mrb_float) == 8, "Garnet requires double precision mruby");
struct Export {
  garnet_session* session;
  mrb_value block;
  char returns = '.';
};
struct RetiredHandle {
  RetiredHandle* next = nullptr;
  virtual ~RetiredHandle() = default;
};
struct garnet_session {
  garnet_host host{};
  mrb_state* ruby = nullptr;
  RClass* clip_class = nullptr;
  RClass* function_class = nullptr;
  std::recursive_timed_mutex gate;
  garnet::Execution execution;
  std::condition_variable_any idle;
  bool poisoned = false;
  bool import_failed = false;
  std::unordered_map<std::string, mrb_value> loaded;
  std::unordered_set<std::string> loading;
  std::vector<std::unique_ptr<Export>> exports;
  RetiredHandle* retired = nullptr;
  bool stopping = false;
  std::mutex release_gate;
  std::condition_variable release_ready;
  std::thread releaser;
  void retire(RetiredHandle* handle) noexcept {
    // Intrusive linking cannot allocate or call back into the host. The queue
    // mutex is never held during destruction or while acquiring VM ownership.
    if (handle) {
      bool wake = false;
      {
        std::lock_guard<std::mutex> lock(release_gate);
        wake = retired == nullptr;
        handle->next = retired;
        retired = handle;
      }
      // A long script must not retain every native argument/result until its
      // outermost invocation returns. Notify only on an empty-to-ready edge.
      if (wake)
        release_ready.notify_one();
    }
  }
  static void release_batch(RetiredHandle* batch) noexcept {
    while (batch) {
      auto* next = batch->next;
      delete batch;
      batch = next;
    }
  }
  void release_loop() noexcept {
    std::unique_lock<std::mutex> lock(release_gate);
    for (;;) {
      release_ready.wait(lock, [&] { return retired || stopping; });
      auto* batch = retired;
      retired = nullptr;
      if (!batch && stopping)
        return;
      lock.unlock();
      // Never destroy a Prefetch graph on one of its own frame workers: its
      // destructor may join those workers. The loop itself makes no mruby calls;
      // host destructors can reenter through the normal guarded engine API.
      release_batch(batch);
      lock.lock();
    }
  }
  ~garnet_session() {
    {
      // Ruby roots can retain a Prefetch graph after the caller drops its copy.
      // Its workers can still be in a callback when AVS runs AtExit. Wait for
      // the VM, reject subsequent entries, and enqueue all remaining handles.
      std::unique_lock<std::recursive_timed_mutex> lock(gate);
      poisoned = true;
      idle.wait(lock, [&] { return execution.calls.empty(); });
      if (ruby) {
        mrb_close(ruby);
        ruby = nullptr;
      }
    }
    {
      std::lock_guard<std::mutex> lock(release_gate);
      stopping = true;
    }
    // Keep callback tokens and host state alive until the cleanup thread has
    // released every batch and native destructors have joined their workers.
    release_ready.notify_all();
    if (releaser.joinable())
      releaser.join();
    else
      release_batch(retired); // Initialization failed before the worker started.
  }
};
namespace {
using namespace garnet;
struct WakeReleaser {
  garnet_session& session;
  ~WakeReleaser() {
    session.release_ready.notify_one();
    session.idle.notify_all();
  }
};
struct Retire {
  garnet_session& session;
  void operator()(RetiredHandle* p) const noexcept { session.retire(p); }
};
struct HostResult : RetiredHandle {
  ResultGuard result{{}};
};
struct HostArguments : RetiredHandle {
  std::vector<std::unique_ptr<Storage>> values;
};
template <typename Call>
auto host_call(garnet_session& s, Call&& call) {
  // Allocate ownership before leaving the VM, including for error results.
  std::unique_ptr<HostResult, Retire> result(new HostResult, Retire{s});
  auto* invocation = s.execution.current;
  assert(invocation);
  auto* context = invocation->call_context;
  {
    OutsideVM outside(*invocation);
    result->result.value = call(context);
  }
  return result;
}
mrb_value load_file(mrb_state*, const std::string&, bool*, bool = true);
std::filesystem::path relative_script_path(mrb_state* mrb, const char* name) {
  auto path = std::filesystem::u8path(name);
  if (path.is_absolute())
    return path;
  for (auto* ci = mrb->c->ci; ci >= mrb->c->cibase; --ci) {
    if (ci->proc && !MRB_PROC_CFUNC_P(ci->proc)) {
      const auto* filename = mrb_debug_get_filename(mrb, ci->proc->body.irep, 0);
      if (filename && *filename)
        return std::filesystem::u8path(filename).parent_path() / path;
    }
    if (ci == mrb->c->cibase)
      break;
  }
  throw std::runtime_error("Relative import has no source filename");
}
mrb_value import_relative(mrb_state* mrb, mrb_value self) {
  const char* name;
  mrb_get_args(mrb, "z", &name);
  try {
    const auto path = relative_script_path(mrb, name).u8string();
    auto avs = mrb_obj_value(mrb_module_get(mrb, "AVS"));
    return mrb_funcall(mrb, avs, "call", 3, mrb_symbol_value(mrb_intern_lit(mrb, "ImportScript")), self,
                       mrb_str_new(mrb, path.data(), path.size()));
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value require_relative(mrb_state* mrb, mrb_value) {
  const char* name;
  mrb_get_args(mrb, "z", &name);
  try {
    const char* caller = nullptr;
    for (auto* ci = mrb->c->ci; ci >= mrb->c->cibase; --ci) {
      if (ci->proc && !MRB_PROC_CFUNC_P(ci->proc)) {
        caller = mrb_debug_get_filename(mrb, ci->proc->body.irep, 0);
        if (caller)
          break;
      }
      if (ci == mrb->c->cibase)
        break;
    }
    if (!caller || !*caller)
      throw std::runtime_error("require_relative has no source filename");
    auto path = std::filesystem::u8path(name);
    if (path.is_relative())
      path = std::filesystem::u8path(caller).parent_path() / path;
    if (!std::filesystem::exists(path) && path.extension().empty())
      path += ".rb";
    bool cached = false;
    load_file(mrb, path.u8string(), &cached);
    return mrb_bool_value(!cached);
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
garnet_session& session(mrb_state* mrb) {
  return *static_cast<garnet_session*>(mrb->ud);
}
struct Clip : RetiredHandle {
  void* identity;
  ResultGuard retained{{}};
  explicit Clip(void* host) : identity(host) {}
};
void free_clip(mrb_state* mrb, void* p) {
  session(mrb).retire(static_cast<Clip*>(p));
}
const mrb_data_type clip_type{"AVS::Clip", free_clip};
struct Function : RetiredHandle {
  void* identity;
  ResultGuard retained{{}};
  explicit Function(void* host) : identity(host) {}
};
void free_function(mrb_state* mrb, void* p) {
  session(mrb).retire(static_cast<Function*>(p));
}
const mrb_data_type function_type{"AVS::Function", free_function};

void from_ruby(mrb_state* mrb, mrb_value value, Storage* out, int depth = 0) {
  if (depth > 32)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Array nesting too deep or cyclic");
  if (mrb_nil_p(value))
    return;
  if (mrb_true_p(value) || mrb_false_p(value)) {
    out->value.type = GARNET_BOOL;
    out->value.as.integer = mrb_true_p(value);
  } else if (mrb_integer_p(value)) {
    out->value.type = GARNET_INT;
    out->value.as.integer = mrb_integer(value);
  } else if (mrb_bigint_p(value)) {
    // mruby 4.0's mrb_as_int rejects INT64_MIN as well as genuine overflow.
    // Use its native formatter (not an overridable Ruby method) and a checked
    // conversion; normal integers retain the allocation-free path above.
    const auto digits = mrb_integer_to_str(mrb, value, 10);
    const auto* begin = RSTRING_PTR(digits);
    const auto* end = begin + RSTRING_LEN(digits);
    int64_t number = 0;
    const auto parsed = std::from_chars(begin, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
      mrb_raise(mrb, E_RANGE_ERROR, "Integer outside signed 64-bit range");
    out->value.type = GARNET_INT;
    out->value.as.integer = number;
  } else if (mrb_float_p(value)) {
    out->value.type = GARNET_FLOAT;
    out->value.as.floating = mrb_float(value);
  } else if (mrb_string_p(value)) {
    out->set_string(std::string(RSTRING_PTR(value), RSTRING_LEN(value)));
  } else if (mrb_array_p(value)) {
    if (RARRAY_LEN(value) > 32767)
      mrb_raise(mrb, E_RANGE_ERROR, "Array too large");
    for (mrb_int i = 0; i < RARRAY_LEN(value); ++i) {
      out->children.push_back(std::make_unique<Storage>());
      from_ruby(mrb, mrb_ary_ref(mrb, value, i), out->children.back().get(), depth + 1);
    }
    out->finish_array();
  } else if (mrb_data_p(value) && DATA_TYPE(value) == &clip_type && DATA_PTR(value)) {
    auto& s = session(mrb);
    auto* clip = static_cast<Clip*>(DATA_PTR(value));
    if (clip->identity != s.host.identity)
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Cross-host clip");
    out->pin = s.host.retain_clip(s.host.identity, clip->retained.value.value.as.handle);
    if (out->pin.status != GARNET_OK)
      throw std::runtime_error(text(out->pin.error));
    out->value = out->pin.value;
  } else if (mrb_data_p(value) && DATA_TYPE(value) == &function_type && DATA_PTR(value)) {
    auto& s = session(mrb);
    auto* function = static_cast<Function*>(DATA_PTR(value));
    if (function->identity != s.host.identity)
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Cross-host function");
    out->pin = s.host.retain_function(s.host.identity, function->retained.value.value.as.handle);
    if (out->pin.status != GARNET_OK)
      throw std::runtime_error(text(out->pin.error));
    out->value = out->pin.value;
  } else
    mrb_raise(mrb, E_TYPE_ERROR, "Expected nil, bool, integer, float, string, array, AVS::Clip or AVS::Function");
}
void append_ruby(mrb_state* mrb, mrb_value value, HostArguments& owner) {
  // Publish ownership BEFORE conversion. A partially converted array can hold
  // native handles even when a later element raises or an allocation fails.
  owner.values.push_back(std::make_unique<Storage>());
  from_ruby(mrb, value, owner.values.back().get());
}

mrb_value to_ruby(mrb_state* mrb, const garnet_value& value, int depth = 0) {
  if (depth > 32)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Host array nesting too deep");
  switch (value.type) {
    case GARNET_UNDEFINED:
      return mrb_nil_value();
    case GARNET_BOOL:
      return mrb_bool_value(value.as.integer != 0);
    case GARNET_INT:
      return mrb_int_value(mrb, value.as.integer);
    case GARNET_FLOAT:
      return mrb_float_value(mrb, value.as.floating);
    case GARNET_STRING: {
      const auto s = text(value.as.string);
      return mrb_str_new(mrb, s.data(), s.size());
    }
    case GARNET_ARRAY: {
      if (value.as.array.size > 32767 || (!value.as.array.data && value.as.array.size))
        mrb_raise(mrb, E_ARGUMENT_ERROR, "Invalid host array");
      auto ary = mrb_ary_new_capa(mrb, static_cast<mrb_int>(value.as.array.size));
      for (size_t i = 0; i < value.as.array.size; ++i)
        mrb_ary_push(mrb, ary, to_ruby(mrb, value.as.array.data[i], depth + 1));
      return ary;
    }
    case GARNET_CLIP: {
      auto& s = session(mrb);
      std::unique_ptr<Clip, Retire> p(new Clip(s.host.identity), Retire{s});
      p->retained.value = s.host.retain_clip(s.host.identity, value.as.handle);
      p->retained.check();
      if (p->retained.value.value.type != GARNET_CLIP || !p->retained.value.value.as.handle)
        throw std::runtime_error("Invalid retained clip");
      auto* object = mrb_data_object_alloc(mrb, session(mrb).clip_class, p.get(), &clip_type);
      p.release();
      return mrb_obj_value(object);
    }
    case GARNET_FUNCTION: {
      auto& s = session(mrb);
      std::unique_ptr<Function, Retire> p(new Function(s.host.identity), Retire{s});
      p->retained.value = s.host.retain_function(s.host.identity, value.as.handle);
      p->retained.check();
      if (p->retained.value.value.type != GARNET_FUNCTION || !p->retained.value.value.as.handle)
        throw std::runtime_error("Invalid retained function");
      auto* object = mrb_data_object_alloc(mrb, session(mrb).function_class, p.get(), &function_type);
      p.release();
      return mrb_obj_value(object);
    }
    default:
      mrb_raise(mrb, E_TYPE_ERROR, "Unsupported host value");
  }
}
std::string name_of(mrb_state* mrb, mrb_value value) {
  if (mrb_symbol_p(value)) {
    mrb_int length = 0;
    const auto* data = mrb_sym_name_len(mrb, mrb_symbol(value), &length);
    const std::string name(data, static_cast<size_t>(length));
    if (name.find('\0') != std::string::npos)
      mrb_raise(mrb, E_ARGUMENT_ERROR, "NUL in name");
    return name;
  }
  return mrb_string_cstr(mrb, value);
}
std::string fold(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return s;
}
garnet_result GARNET_CALL call_export(void*, void*, const garnet_value*, size_t);
bool identifier(const std::string& name) {
  if (name.empty())
    return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i && c >= '0' && c <= '9')))
      return false;
  }
  return true;
}
void validate_signature(const std::string& signature) {
  std::unordered_set<std::string> names;
  size_t count = 0;
  for (size_t i = 0; i < signature.size();) {
    if (++count > 256)
      throw std::runtime_error("Too many exported parameters");
    if (signature[i] == '[') {
      const auto end = signature.find(']', ++i);
      if (end == std::string::npos)
        throw std::runtime_error("Unclosed parameter name");
      auto name = signature.substr(i, end - i);
      if (!identifier(name) || fold(name).find("__garnet_") == 0 || !names.insert(fold(name)).second)
        throw std::runtime_error("Invalid or duplicate exported parameter name");
      i = end + 1;
    }
    if (i == signature.size() || std::string("cbifsn.").find(signature[i++]) == std::string::npos)
      throw std::runtime_error("Unsupported exported parameter type");
  }
}
mrb_value export_filter(mrb_state* mrb, mrb_value) {
  mrb_value name, signature, block;
  mrb_get_args(mrb, "oo&", &name, &signature, &block);
  try {
    auto& s = session(mrb);
    if (mrb_nil_p(block))
      throw std::runtime_error("AVS.export requires a block");
    const auto function = name_of(mrb, name);
    const auto params = name_of(mrb, signature);
    if (!identifier(function))
      throw std::runtime_error("Invalid exported function name");
    validate_signature(params);
    if (s.exports.size() >= 4096)
      throw std::runtime_error("Too many exported functions");
    auto entry = std::make_unique<Export>(Export{&s, block});
    mrb_gc_register(mrb, block);
    // Keep the callback alive even if a host reports failure after registration.
    auto* data = entry.get();
    s.exports.push_back(std::move(entry));
    auto owner = host_call(s, [&](void* context) {
      return s.host.register_filter(s.host.identity, context, span(function), span(params), call_export, data);
    });
    auto& r = owner->result;
    r.check();
    return mrb_nil_value();
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value make_function(mrb_state* mrb, mrb_value) {
  mrb_value signature, returns, block;
  mrb_get_args(mrb, "oo&", &signature, &returns, &block);
  try {
    auto& s = session(mrb);
    if (mrb_nil_p(block))
      throw std::runtime_error("AVS.function requires a block");
    const auto params = name_of(mrb, signature), output = name_of(mrb, returns);
    validate_signature(params);
    if (output.size() != 1 || std::string("cbifsn.").find(output[0]) == std::string::npos)
      throw std::runtime_error("Invalid function return type");
    if (s.exports.size() >= 4096)
      throw std::runtime_error("Too many exported functions");
    auto entry = std::make_unique<Export>(Export{&s, block, output[0]});
    mrb_gc_register(mrb, block);
    auto* data = entry.get();
    s.exports.push_back(std::move(entry));
    auto owner = host_call(s, [&](void* context) {
      return s.host.make_function(s.host.identity, context, span(params), call_export, data);
    });
    auto& r = owner->result;
    r.check();
    if (r.value.value.type != GARNET_FUNCTION)
      throw std::runtime_error("Host did not create a function");
    return to_ruby(mrb, r.value.value);
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value invoke(mrb_state* mrb, mrb_value self) {
  mrb_value *values, block, keywords;
  mrb_int count;
  mrb_kwargs kwargs{0, 0, nullptr, nullptr, &keywords};
  mrb_get_args(mrb, "*:&", &values, &count, &kwargs, &block);
  const bool function_call = mrb_data_p(self) && DATA_TYPE(self) == &function_type && DATA_PTR(self);
  if (!function_call && count < 1)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Expected filter name");
  if (!mrb_nil_p(block))
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Native filter blocks are not yet supported");
  try {
    const auto name = function_call ? std::string() : name_of(mrb, values[0]);
    auto& s = session(mrb);
    std::unique_ptr<HostArguments, Retire> arguments(new HostArguments, Retire{s});
    auto& owned = arguments->values;
    std::vector<std::string> names;
    if (mrb_data_p(self) && DATA_TYPE(self) == &clip_type)
      append_ruby(mrb, self, *arguments);
    for (mrb_int i = function_call ? 0 : 1; i < count; ++i)
      append_ruby(mrb, values[i], *arguments);
    const size_t positional = owned.size();
    if (!mrb_nil_p(keywords)) {
      const auto keys = mrb_hash_keys(mrb, keywords);
      std::unordered_set<std::string> seen;
      for (mrb_int i = 0; i < RARRAY_LEN(keys); ++i) {
        auto key = mrb_ary_ref(mrb, keys, i);
        auto name_key = name_of(mrb, key);
        if (name_key.empty())
          mrb_raise(mrb, E_ARGUMENT_ERROR, "Empty AVS keyword");
        if (!seen.insert(fold(name_key)).second)
          mrb_raise(mrb, E_ARGUMENT_ERROR, "Duplicate case-insensitive AVS keyword");
        auto value = mrb_hash_get(mrb, keywords, key);
        if (mrb_nil_p(value))
          continue;
        names.push_back(std::move(name_key));
        append_ruby(mrb, value, *arguments);
      }
    }
    if (owned.size() > 32767)
      mrb_raise(mrb, E_RANGE_ERROR, "Too many filter arguments");
    std::vector<garnet_value> args;
    for (const auto& v : owned)
      args.push_back(v->value);
    std::vector<garnet_string> arg_names(positional);
    for (const auto& n : names)
      arg_names.push_back(span(n));
    auto* function = function_call ? static_cast<Function*>(DATA_PTR(self)) : nullptr;
    auto* handle = function ? function->retained.value.value.as.handle : nullptr;
    auto owner = host_call(s, [&](void* context) {
      return function
                 ? s.host.invoke_function(s.host.identity, context, handle, args.data(), arg_names.data(), args.size())
                 : s.host.invoke(s.host.identity, context, span(name), args.data(), arg_names.data(), args.size());
    });
    auto& r = owner->result;
    r.check();
    return to_ruby(mrb, r.value.value);
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value get_var(mrb_state* mrb, mrb_value) {
  mrb_value key, fallback = mrb_nil_value();
  mrb_get_args(mrb, "o|o", &key, &fallback);
  try {
    const auto name = name_of(mrb, key);
    auto& s = session(mrb);
    auto owner = host_call(s, [&](void* context) { return s.host.get_var(s.host.identity, context, span(name)); });
    auto& r = owner->result;
    r.check();
    return r.value.value.type == GARNET_UNDEFINED ? fallback : to_ruby(mrb, r.value.value);
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value assign_var(mrb_state* mrb, bool global) {
  mrb_value key, value;
  mrb_get_args(mrb, "oo", &key, &value);
  try {
    const auto name = name_of(mrb, key);
    auto& s = session(mrb);
    std::unique_ptr<HostArguments, Retire> arguments(new HostArguments, Retire{s});
    append_ruby(mrb, value, *arguments);
    auto owner = host_call(s, [&](void* context) {
      return s.host.set_var(s.host.identity, context, span(name), &arguments->values[0]->value, global);
    });
    auto& r = owner->result;
    r.check();
    return value;
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value set_var(mrb_state* mrb, mrb_value) {
  return assign_var(mrb, false);
}
mrb_value set_global_var(mrb_state* mrb, mrb_value) {
  return assign_var(mrb, true);
}
mrb_value setup(mrb_state* mrb, void*) {
  auto* avs = mrb_define_module(mrb, "AVS");
  auto* clip = mrb_define_class_under(mrb, avs, "Clip", mrb->object_class);
  session(mrb).clip_class = clip;
  // Native cached class pointers are not traced through mrb->ud. Keep it alive
  // even if user code removes or replaces AVS::Clip.
  mrb_gc_register(mrb, mrb_obj_value(clip));
  auto* function = mrb_define_class_under(mrb, avs, "Function", mrb->object_class);
  session(mrb).function_class = function;
  mrb_gc_register(mrb, mrb_obj_value(function));
  MRB_SET_INSTANCE_TT(function, MRB_TT_DATA);
  MRB_SET_INSTANCE_TT(clip, MRB_TT_DATA);
  const auto args = MRB_ARGS_ANY() | MRB_ARGS_KEY(0, 1) | MRB_ARGS_BLOCK();
  mrb_define_method(mrb, function, "call", invoke, args);
  mrb_define_method(mrb, clip, "filter", invoke, args);
  mrb_define_method(mrb, clip, "method_missing", invoke, args);
  mrb_define_method(mrb, clip, "import_relative", import_relative, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, avs, "call", invoke, args);
  mrb_define_class_method(mrb, avs, "method_missing", invoke, args);
  mrb_define_class_method(mrb, avs, "export", export_filter, MRB_ARGS_REQ(2) | MRB_ARGS_BLOCK());
  mrb_define_class_method(mrb, avs, "__function", make_function, MRB_ARGS_REQ(2) | MRB_ARGS_BLOCK());
  mrb_define_class_method(mrb, avs, "get_var", get_var, MRB_ARGS_ARG(1, 1));
  mrb_define_class_method(mrb, avs, "[]", get_var, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, avs, "set_var", set_var, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, avs, "[]=", set_var, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, avs, "set_global_var", set_global_var, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, mrb->kernel_module, "require_relative", require_relative, MRB_ARGS_REQ(1));
  mrb_load_nstring(mrb, garnet_runtime, sizeof(garnet_runtime) - 1);
  if (mrb->exc)
    mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
  return mrb_nil_value();
}
mrb_value describe(mrb_state* mrb, void* p) {
  const auto exception = *static_cast<mrb_value*>(p);
  auto message = mrb_inspect(mrb, exception);
  const auto trace = mrb_funcall(mrb, exception, "backtrace", 0);
  if (mrb_array_p(trace)) {
    for (mrb_int i = 0; i < RARRAY_LEN(trace); ++i) {
      const auto line = mrb_ary_ref(mrb, trace, i);
      if (mrb_string_p(line)) {
        mrb_str_cat_lit(mrb, message, "\n");
        mrb_str_concat(mrb, message, line);
      }
    }
  }
  return message;
}
void check_ruby(mrb_state* mrb, mrb_bool failed, mrb_value value) {
  if (!failed && !mrb->exc)
    return;
  auto exception = failed ? value : mrb_obj_value(mrb->exc);
  mrb->exc = nullptr;
  mrb_bool description_failed = false;
  auto message = mrb_protect_error(mrb, describe, &exception, &description_failed);
  if (!description_failed && mrb_string_p(message))
    throw std::runtime_error(std::string(RSTRING_PTR(message), RSTRING_LEN(message)));
  throw std::runtime_error("Ruby exception (formatting failed)");
}
struct Evaluation {
  garnet_string source;
  std::string filename;
  std::unique_ptr<HostArguments, Retire> output;
  bool file = false;
  bool pipeline = false;
};
mrb_value execute_source(mrb_state* mrb, garnet_string source, const std::string& filename) {
  const auto deleter = [mrb](mrb_ccontext* p) {
    mrb_ccontext_free(mrb, p);
  };
  std::unique_ptr<mrb_ccontext, decltype(deleter)> context(mrb_ccontext_new(mrb), deleter);
  if (!context)
    throw std::bad_alloc();
  mrb_ccontext_filename(mrb, context.get(), filename.c_str());
  context->capture_errors = true;
  context->no_exec = true;
  auto proc = mrb_load_nstring_cxt(mrb, source.data ? source.data : "", source.size, context.get());
  if (mrb->exc)
    mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
  auto* compiled = mrb_proc_ptr(proc);
  // Compilation inside require_relative must not capture its Kernel scope.
  compiled->upper = nullptr;
  MRB_PROC_SET_TARGET_CLASS(compiled, mrb->object_class);
  // Unlike mrb_top_run, this preserves the caller's active Ruby stack/locals
  // when a library is loaded from inside another Ruby method.
  return mrb_yield_with_class(mrb, proc, 0, nullptr, mrb_top_self(mrb), mrb->object_class);
}
mrb_value load_file(mrb_state* mrb, const std::string& filename, bool* cached, bool cache_result) {
  auto& s = session(mrb);
  const auto path = std::filesystem::canonical(std::filesystem::u8path(filename));
  const auto resolved = path.u8string();
  auto key = resolved;
#ifdef _WIN32
  key = fold(key);
#endif
  const auto found = s.loaded.find(key);
  if (cache_result && found != s.loaded.end()) {
    if (cached)
      *cached = true;
    return found->second;
  }
  if (s.loading.count(key))
    throw std::runtime_error("Circular Ruby import: " + resolved);
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot open Ruby script: " + resolved);
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad())
    throw std::runtime_error("Cannot read Ruby script: " + resolved);
  s.loading.insert(key);
  try {
    auto value = execute_source(mrb, span(source), resolved);
    if (cache_result) {
      mrb_gc_register(mrb, value);
      try {
        s.loaded.emplace(key, value);
      } catch (...) {
        mrb_gc_unregister(mrb, value);
        throw;
      }
    }
    s.loading.erase(key);
    return value;
  } catch (...) {
    s.loading.erase(key);
    s.import_failed = true;
    throw;
  }
}
mrb_value evaluate_body(mrb_state* mrb, void* data) {
  try {
    auto& e = *static_cast<Evaluation*>(data);
    auto value = e.file ? load_file(mrb, e.filename, nullptr, !e.pipeline) : execute_source(mrb, e.source, e.filename);
    if (!mrb->exc)
      append_ruby(mrb, value, *e.output);
    return value;
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
struct Active {
  garnet_session& s;
  Invocation invocation;
  bool done = false;
  Active(garnet_session& session, std::unique_lock<std::recursive_timed_mutex>& lock, void* context)
      : s(session), invocation(s.execution, lock, context) {}
  ~Active() {
    if (!done)
      s.poisoned = true;
  }
};
struct CallbackCall {
  Export& entry;
  const garnet_value* args;
  size_t count;
  std::unique_ptr<HostArguments, Retire> output;
};
mrb_value callback_body(mrb_state* mrb, void* data) {
  try {
    auto& call = *static_cast<CallbackCall*>(data);
    std::vector<mrb_value> args;
    for (size_t i = 0; i < call.count; ++i)
      args.push_back(to_ruby(mrb, call.args[i]));
    auto value = mrb_yield_argv(mrb, call.entry.block, static_cast<mrb_int>(args.size()), args.data());
    append_ruby(mrb, value, *call.output);
    const auto type = call.output->values[0]->value.type;
    bool valid = false;
    switch (call.entry.returns) {
      case '.':
        valid = true;
        break;
      case 'c':
        valid = type == GARNET_CLIP;
        break;
      case 'b':
        valid = type == GARNET_BOOL;
        break;
      case 'i':
        valid = type == GARNET_INT;
        break;
      case 'f':
        valid = type == GARNET_FLOAT || type == GARNET_INT;
        break;
      case 's':
        valid = type == GARNET_STRING;
        break;
      case 'n':
        valid = type == GARNET_FUNCTION;
        break;
    }
    if (!valid)
      throw std::runtime_error("Ruby function return type mismatch");
    return value;
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
garnet_result GARNET_CALL call_export(void* data, void* context, const garnet_value* args, size_t count) {
  try {
    auto& entry = *static_cast<Export*>(data);
    auto& s = *entry.session;
    WakeReleaser wake{s}; // Destroyed after the lock and Active scope.
    std::unique_lock<std::recursive_timed_mutex> lock(s.gate, std::defer_lock);
    // Ruby itself remains serialized. Host calls temporarily relinquish VM
    // ownership, so another worker can satisfy a synchronous frame dependency.
    if (!lock.try_lock_for(std::chrono::seconds(5)))
      return error("Ruby callback wait exceeded 5 seconds (contention or cross-thread dependency)", GARNET_BUSY);
    if (s.poisoned || s.import_failed)
      return error("Ruby session disabled after failed evaluation");
    if (count > 32767 || (count && !args))
      return error("Invalid callback arguments");
    if (s.execution.current)
      return error("Reentrant host retain is not supported", GARNET_BUSY);
    if (auto* parent = Invocation::parent(s.execution); parent && parent->depth >= 64)
      return error("Ruby callback nesting limit exceeded");
    Active active(s, lock, context);
    CallbackCall call{entry, args, count, {new HostArguments, Retire{s}}};
    mrb_bool failed = false;
    auto value = mrb_protect_error(s.ruby, callback_body, &call, &failed);
    // The protected boundary has unwound this invocation. An ordinary callback
    // exception is a result, not corruption of the shared VM. Keep fail-closed
    // behavior for initialization failures and incomplete native unwinding.
    active.done = true;
    check_ruby(s.ruby, failed, value);
    if (s.poisoned || s.import_failed)
      throw std::runtime_error("Ruby session disabled after nested failure");
    auto out = result(std::move(call.output->values[0]));
    return out;
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown Ruby callback error");
  }
}
} // namespace

extern "C" garnet_result GARNET_CALL garnet_create(const garnet_host* host, garnet_session** out) {
  if (out)
    *out = nullptr;
  if (!out || !host || host->revision != GARNET_CONTRACT_REVISION || host->size != sizeof(garnet_host) ||
      !host->identity || !host->invoke || !host->retain_clip || !host->release_clip || !host->register_filter ||
      !host->get_var || !host->set_var || !host->retain_function || !host->release_function || !host->invoke_function ||
      !host->make_function)
    return garnet::error("Invalid Garnet host contract", GARNET_INVALID_CONTRACT);
  try {
    auto s = std::make_unique<garnet_session>();
    s->host = *host;
    s->ruby = mrb_open();
    if (MRB_OPEN_FAILURE(s->ruby))
      throw std::runtime_error("mruby initialization failed");
    s->ruby->ud = s.get();
    mrb_bool failed = false;
    auto r = mrb_protect_error(s->ruby, setup, nullptr, &failed);
    check_ruby(s->ruby, failed, r);
    if (mrb_class_defined(s->ruby, "Fiber"))
      throw std::runtime_error("Garnet requires mruby without the Fiber gem");
    s->execution.ruby = s->ruby;
    s->releaser = std::thread([session = s.get()] { session->release_loop(); });
    *out = s.release();
    return {};
  } catch (const std::exception& e) {
    return garnet::error(e.what());
  } catch (...) {
    return garnet::error("Unknown engine initialization error");
  }
}
static garnet_result evaluate(garnet_session* s, void* context, garnet_string source, garnet_string filename, bool file,
                              bool pipeline = false) {
  if (!s || (!source.data && source.size))
    return garnet::error("Invalid evaluation input");
  try {
    WakeReleaser wake{*s}; // Host resources are released after VM ownership.
    std::unique_lock<std::recursive_timed_mutex> lock(s->gate, std::try_to_lock);
    if (!lock.owns_lock() || s->execution.current ||
        (!s->execution.calls.empty() && (!file || !Invocation::parent(s->execution))))
      return garnet::error("Concurrent or reentrant Ruby entry", GARNET_BUSY);
    if (s->poisoned)
      return garnet::error("Ruby session disabled after failed evaluation");
    if (auto* parent = Invocation::parent(s->execution); parent && parent->depth >= 64)
      return garnet::error("Ruby script nesting limit exceeded");
    Evaluation e{source, garnet::text(filename), {new HostArguments, Retire{*s}}, file, pipeline};
    if (e.filename.find('\0') != std::string::npos)
      return garnet::error("NUL in filename");
    Active active(*s, lock, context);
    mrb_bool failed = false;
    auto value = mrb_protect_error(s->ruby, evaluate_body, &e, &failed);
    check_ruby(s->ruby, failed, value);
    if (s->poisoned || s->import_failed)
      throw std::runtime_error("Ruby session disabled after a failed nested import");
    if (pipeline && e.output->values[0]->value.type != GARNET_CLIP)
      throw std::runtime_error("Pipeline script must return a clip");
    auto output = garnet::result(std::move(e.output->values[0]));
    active.done = true;
    return output;
  } catch (const std::exception& e) {
    return garnet::error(e.what());
  } catch (...) {
    return garnet::error("Unknown Ruby evaluation error");
  }
}
extern "C" garnet_result GARNET_CALL garnet_evaluate(garnet_session* s, void* context, garnet_string source,
                                                     garnet_string filename) {
  return evaluate(s, context, source, filename, false);
}
extern "C" garnet_result GARNET_CALL garnet_import(garnet_session* s, void* context, garnet_string filename) {
  return evaluate(s, context, {}, filename, true);
}
extern "C" garnet_result GARNET_CALL garnet_run_script(garnet_session* s, void* context, garnet_string filename) {
  return evaluate(s, context, {}, filename, true, true);
}
extern "C" void GARNET_CALL garnet_destroy(garnet_session* s) {
  delete s;
}
