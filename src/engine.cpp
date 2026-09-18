#include <garnet/engine.h>
#include "result.hpp"
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/compile.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mutex>
#include <unordered_set>

static_assert(sizeof(mrb_int) == 8, "Garnet requires 64-bit mruby integers");
static_assert(sizeof(mrb_float) == 8, "Garnet requires double precision mruby");
struct garnet_session {
  garnet_host host{};
  mrb_state* ruby = nullptr;
  RClass* clip_class = nullptr;
  void* call_context = nullptr;
  std::recursive_mutex gate;
  bool active = false;
  bool poisoned = false;
  ~garnet_session() {
    if (ruby)
      mrb_close(ruby);
  }
};
namespace {
using namespace garnet;
garnet_session& session(mrb_state* mrb) {
  return *static_cast<garnet_session*>(mrb->ud);
}
struct Clip {
  void* identity;
  ResultGuard retained;
  Clip(const garnet_host& host, void* handle)
      : identity(host.identity), retained(host.retain_clip(host.identity, handle)) {
    retained.check();
    if (retained.value.value.type != GARNET_CLIP || !retained.value.value.as.handle)
      throw std::runtime_error("Invalid retained clip");
  }
};
void free_clip(mrb_state*, void* p) {
  delete static_cast<Clip*>(p);
}
const mrb_data_type clip_type{"AVS::Clip", free_clip};

std::unique_ptr<Storage> from_ruby(mrb_state* mrb, mrb_value value, int depth = 0) {
  if (depth > 32)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Array nesting too deep or cyclic");
  auto out = std::make_unique<Storage>();
  if (mrb_nil_p(value))
    return out;
  if (mrb_true_p(value) || mrb_false_p(value)) {
    out->value.type = GARNET_BOOL;
    out->value.as.integer = mrb_true_p(value);
  } else if (mrb_integer_p(value)) {
    out->value.type = GARNET_INT;
    out->value.as.integer = mrb_integer(value);
  } else if (mrb_float_p(value)) {
    out->value.type = GARNET_FLOAT;
    out->value.as.floating = mrb_float(value);
  } else if (mrb_string_p(value)) {
    out->set_string(std::string(RSTRING_PTR(value), RSTRING_LEN(value)));
  } else if (mrb_array_p(value)) {
    if (RARRAY_LEN(value) > 32767)
      mrb_raise(mrb, E_RANGE_ERROR, "Array too large");
    for (mrb_int i = 0; i < RARRAY_LEN(value); ++i)
      out->children.push_back(from_ruby(mrb, mrb_ary_ref(mrb, value, i), depth + 1));
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
  } else
    mrb_raise(mrb, E_TYPE_ERROR, "Expected nil, bool, integer, float, string, array or AVS::Clip");
  return out;
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
      auto p = std::make_unique<Clip>(session(mrb).host, value.as.handle);
      auto* object = mrb_data_object_alloc(mrb, session(mrb).clip_class, p.get(), &clip_type);
      p.release();
      return mrb_obj_value(object);
    }
    default:
      mrb_raise(mrb, E_TYPE_ERROR, "Unsupported host value");
  }
}
std::string name_of(mrb_state* mrb, mrb_value value) {
  if (mrb_symbol_p(value))
    return mrb_sym_name(mrb, mrb_symbol(value));
  return mrb_string_cstr(mrb, value);
}
std::string fold(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return s;
}
mrb_value invoke(mrb_state* mrb, mrb_value self) {
  mrb_value *values, block, keywords;
  mrb_int count;
  mrb_kwargs kwargs{0, 0, nullptr, nullptr, &keywords};
  mrb_get_args(mrb, "*:&", &values, &count, &kwargs, &block);
  if (count < 1)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Expected filter name");
  if (!mrb_nil_p(block))
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Native filter blocks are not yet supported");
  try {
    const auto name = name_of(mrb, values[0]);
    std::vector<std::unique_ptr<Storage>> owned;
    std::vector<std::string> names;
    if (mrb_data_p(self) && DATA_TYPE(self) == &clip_type)
      owned.push_back(from_ruby(mrb, self));
    for (mrb_int i = 1; i < count; ++i)
      owned.push_back(from_ruby(mrb, values[i]));
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
        owned.push_back(from_ruby(mrb, value));
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
    auto& s = session(mrb);
    ResultGuard r(
        s.host.invoke(s.host.identity, s.call_context, span(name), args.data(), arg_names.data(), args.size()));
    r.check();
    return to_ruby(mrb, r.value.value);
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
mrb_value setup(mrb_state* mrb, void*) {
  auto* avs = mrb_define_module(mrb, "AVS");
  auto* clip = mrb_define_class_under(mrb, avs, "Clip", mrb->object_class);
  session(mrb).clip_class = clip;
  // Native cached class pointers are not traced through mrb->ud. Keep it alive
  // even if user code removes or replaces AVS::Clip.
  mrb_gc_register(mrb, mrb_obj_value(clip));
  MRB_SET_INSTANCE_TT(clip, MRB_TT_DATA);
  const auto args = MRB_ARGS_ANY() | MRB_ARGS_KEY(0, 1) | MRB_ARGS_BLOCK();
  mrb_define_method(mrb, clip, "filter", invoke, args);
  mrb_define_method(mrb, clip, "method_missing", invoke, args);
  mrb_define_class_method(mrb, avs, "call", invoke, args);
  mrb_define_class_method(mrb, avs, "method_missing", invoke, args);
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
  std::unique_ptr<Storage> output;
};
mrb_value evaluate_body(mrb_state* mrb, void* data) {
  try {
    auto& e = *static_cast<Evaluation*>(data);
    const auto deleter = [mrb](mrb_ccontext* p) {
      mrb_ccontext_free(mrb, p);
    };
    std::unique_ptr<mrb_ccontext, decltype(deleter)> context(mrb_ccontext_new(mrb), deleter);
    if (!context)
      throw std::bad_alloc();
    mrb_ccontext_filename(mrb, context.get(), e.filename.c_str());
    context->capture_errors = true;
    auto value = mrb_load_nstring_cxt(mrb, e.source.data ? e.source.data : "", e.source.size, context.get());
    if (!mrb->exc)
      e.output = from_ruby(mrb, value);
    return value;
  } catch (const std::exception& e) {
    mrb_raise(mrb, E_RUNTIME_ERROR, e.what());
  }
}
struct Active {
  garnet_session& s;
  int arena;
  Active(garnet_session& session, void* context) : s(session), arena(mrb_gc_arena_save(s.ruby)) {
    s.call_context = context;
    s.active = true;
  }
  ~Active() {
    s.call_context = nullptr;
    s.ruby->exc = nullptr;
    s.active = false;
    mrb_gc_arena_restore(s.ruby, arena);
  }
};
} // namespace

extern "C" garnet_result GARNET_CALL garnet_create(const garnet_host* host, garnet_session** out) {
  if (out)
    *out = nullptr;
  if (!out || !host || host->revision != GARNET_CONTRACT_REVISION || host->size != sizeof(garnet_host) ||
      !host->identity || !host->invoke || !host->retain_clip || !host->release_clip)
    return garnet::error("Invalid Garnet host contract", GARNET_INVALID_CONTRACT);
  try {
    auto s = std::make_unique<garnet_session>();
    s->host = *host;
    s->ruby = mrb_open();
    if (!s->ruby)
      throw std::bad_alloc();
    s->ruby->ud = s.get();
    mrb_bool failed = false;
    auto r = mrb_protect_error(s->ruby, setup, nullptr, &failed);
    check_ruby(s->ruby, failed, r);
    *out = s.release();
    return {};
  } catch (const std::exception& e) {
    return garnet::error(e.what());
  } catch (...) {
    return garnet::error("Unknown engine initialization error");
  }
}
extern "C" garnet_result GARNET_CALL garnet_evaluate(garnet_session* s, void* context, garnet_string source,
                                                     garnet_string filename) {
  if (!s || (!source.data && source.size))
    return garnet::error("Invalid evaluation input");
  try {
    std::unique_lock<std::recursive_mutex> lock(s->gate, std::try_to_lock);
    if (!lock.owns_lock() || s->active)
      return garnet::error("Concurrent or reentrant Ruby entry", GARNET_BUSY);
    if (s->poisoned)
      return garnet::error("Ruby session disabled after failed evaluation");
    Evaluation e{source, garnet::text(filename), nullptr};
    if (e.filename.find('\0') != std::string::npos)
      return garnet::error("NUL in filename");
    Active active(*s, context);
    s->poisoned = true;
    mrb_bool failed = false;
    auto value = mrb_protect_error(s->ruby, evaluate_body, &e, &failed);
    check_ruby(s->ruby, failed, value);
    auto output = garnet::result(std::move(e.output));
    s->poisoned = false;
    return output;
  } catch (const std::exception& e) {
    return garnet::error(e.what());
  } catch (...) {
    return garnet::error("Unknown Ruby evaluation error");
  }
}
extern "C" void GARNET_CALL garnet_destroy(garnet_session* s) {
  delete s;
}
