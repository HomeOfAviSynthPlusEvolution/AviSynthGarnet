#include <avisynth.h>
#include <garnet/engine.h>
#include "result.hpp"
#include <filesystem>
#include <limits>

const AVS_Linkage* AVS_linkage = nullptr;
namespace {
using namespace garnet;
struct Host;
struct Export {
  Host* host;
  garnet_callback callback;
  void* data;
};
struct Clip {
  Host* host;
  PClip clip;
};
struct Function {
  Host* host;
  AVSValue value;
};
struct Host {
  garnet_host api{};
  garnet_session* session = nullptr;
  std::vector<std::unique_ptr<Export>> exports;
  ~Host() { garnet_destroy(session); }
};
void GARNET_CALL release_clip(void*, void* p) {
  delete static_cast<Clip*>(p);
}
void GARNET_CALL release_function(void*, void* p) {
  delete static_cast<Function*>(p);
}
std::unique_ptr<Storage> from_avs(Host& host, const AVSValue& v, int depth = 0) {
  if (depth > 32)
    throw std::runtime_error("AVS array nesting too deep");
  auto p = std::make_unique<Storage>();
  if (!v.Defined())
    return p;
  if (v.IsBool()) {
    p->value.type = GARNET_BOOL;
    p->value.as.integer = v.AsBool();
  } else if (v.IsInt()) {
    p->value.type = GARNET_INT;
    p->value.as.integer = v.AsLong();
  } else if (v.IsFloat()) {
    p->value.type = GARNET_FLOAT;
    p->value.as.floating = v.AsFloat();
  } else if (v.IsString())
    p->set_string(v.AsString());
  else if (v.IsClip()) {
    auto clip = std::make_unique<Clip>(Clip{&host, v.AsClip()});
    p->host = host.api;
    p->value.type = GARNET_CLIP;
    p->value.as.handle = clip.release();
  } else if (v.IsArray()) {
    for (int i = 0; i < v.ArraySize(); ++i)
      p->children.push_back(from_avs(host, v[i], depth + 1));
    p->finish_array();
  } else if (v.IsFunction()) {
    auto function = std::make_unique<Function>(Function{&host, v});
    p->host = host.api;
    p->value.type = GARNET_FUNCTION;
    p->value.as.handle = function.release();
  } else
    throw std::runtime_error("Unsupported AVS value");
  return p;
}
AVSValue to_avs(Host& host, IScriptEnvironment* env, const garnet_value& v, int depth = 0) {
  if (depth > 32)
    throw std::runtime_error("Ruby array nesting too deep");
  switch (v.type) {
    case GARNET_UNDEFINED:
      return AVSValue();
    case GARNET_BOOL:
      return AVSValue(v.as.integer != 0);
    case GARNET_INT:
      return AVSValue(v.as.integer);
    case GARNET_FLOAT:
      return AVSValue(v.as.floating);
    case GARNET_STRING: {
      auto s = text(v.as.string);
      if (s.find('\0') != std::string::npos || s.size() > INT_MAX)
        throw std::runtime_error("AVS strings cannot contain NUL or exceed INT_MAX bytes");
      return AVSValue(env->SaveString(s.data(), static_cast<int>(s.size())));
    }
    case GARNET_CLIP: {
      auto* clip = static_cast<Clip*>(v.as.handle);
      if (!clip || clip->host != &host)
        throw std::runtime_error("Cross-host or null clip");
      return AVSValue(clip->clip);
    }
    case GARNET_ARRAY: {
      if (v.as.array.size > 32767 || (!v.as.array.data && v.as.array.size))
        throw std::runtime_error("Invalid AVS array");
      std::vector<AVSValue> values;
      for (size_t i = 0; i < v.as.array.size; ++i)
        values.push_back(to_avs(host, env, v.as.array.data[i], depth + 1));
      return AVSValue(values.data(), static_cast<int>(values.size()));
    }
    case GARNET_FUNCTION: {
      auto* function = static_cast<Function*>(v.as.handle);
      if (!function || function->host != &host)
        throw std::runtime_error("Cross-host or null function");
      return function->value;
    }
    default:
      throw std::runtime_error("Unknown Garnet value type");
  }
}
garnet_result GARNET_CALL retain_clip(void* identity, void* handle) {
  try {
    auto& host = *static_cast<Host*>(identity);
    auto* clip = static_cast<Clip*>(handle);
    if (!clip || clip->host != &host)
      return error("Cross-host or null clip");
    return result(from_avs(host, AVSValue(clip->clip)));
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown clip retain failure");
  }
}
garnet_result GARNET_CALL retain_function(void* identity, void* handle) {
  try {
    auto& host = *static_cast<Host*>(identity);
    auto* function = static_cast<Function*>(handle);
    if (!function || function->host != &host)
      return error("Cross-host or null function");
    return result(from_avs(host, function->value));
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown function retain failure");
  }
}
garnet_result GARNET_CALL invoke(void* identity, void* context, garnet_string name, const garnet_value* args,
                                 const garnet_string* names, size_t count) {
  try {
    auto& host = *static_cast<Host*>(identity);
    auto* env = static_cast<IScriptEnvironment*>(context);
    if (!env || count > 32767 || (count && (!args || !names)))
      return error("Invalid host call");
    auto function = text(name);
    if (function.empty() || function.find('\0') != std::string::npos)
      return error("Invalid function name");
    std::vector<AVSValue> values;
    std::vector<std::string> strings;
    for (size_t i = 0; i < count; ++i) {
      values.push_back(to_avs(host, env, args[i]));
      strings.push_back(text(names[i]));
      if (strings.back().find('\0') != std::string::npos)
        return error("NUL in keyword");
    }
    std::vector<const char*> pointers;
    for (const auto& s : strings)
      pointers.push_back(s.empty() ? nullptr : s.c_str());
    auto value = env->Invoke(function.c_str(), AVSValue(values.data(), static_cast<int>(count)), pointers.data());
    return result(from_avs(host, value));
  } catch (const IScriptEnvironment::NotFound&) {
    return error("AVS filter or matching overload not found");
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown native invocation failure");
  }
}
struct LocalContext {
  IScriptEnvironment* env;
  explicit LocalContext(IScriptEnvironment* env) : env(env) { env->PushContext(); }
  ~LocalContext() { env->PopContext(); }
};
garnet_result GARNET_CALL invoke_function(void* identity, void* context, void* handle, const garnet_value* args,
                                          const garnet_string* names, size_t count) {
  try {
    auto* function = static_cast<Function*>(handle);
    if (!context || !function || function->host != identity)
      return error("Invalid function invocation");
    auto* env = static_cast<IScriptEnvironment*>(context);
    LocalContext local(env);
    // Public Invoke resolves function-valued variables. Avoid AsFunction(),
    // which has no public linkage entry, and never inspect AVSValue layout.
    env->SetVar("__garnet_function", function->value);
    return invoke(identity, context, span("__garnet_function"), args, names, count);
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown function invocation failure");
  }
}
std::string variable_name(garnet_string name) {
  auto value = text(name);
  if (value.empty() || value.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid variable name");
  return value;
}
garnet_result GARNET_CALL get_var(void* identity, void* context, garnet_string name) {
  try {
    if (!context)
      return error("Missing variable call context");
    auto* env = static_cast<IScriptEnvironment*>(context);
    return result(from_avs(*static_cast<Host*>(identity), env->GetVarDef(variable_name(name).c_str())));
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown variable read failure");
  }
}
garnet_result GARNET_CALL set_var(void* identity, void* context, garnet_string name, const garnet_value* value,
                                  int global) {
  try {
    if (!context || !value)
      return error("Invalid variable assignment");
    auto* env = static_cast<IScriptEnvironment*>(context);
    const auto key = variable_name(name);
    const auto converted = to_avs(*static_cast<Host*>(identity), env, *value);
    const auto* saved = env->SaveString(key.c_str());
    const bool ok = global ? env->SetGlobalVar(saved, converted) : env->SetVar(saved, converted);
    if (!ok)
      return error("Variable assignment failed");
    return {};
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown variable assignment failure");
  }
}
AVSValue __cdecl exported_filter(AVSValue args, void* data, IScriptEnvironment* env) {
  auto& entry = *static_cast<Export*>(data);
  try {
    auto owned = from_avs(*entry.host, args);
    if (owned->value.type != GARNET_ARRAY)
      throw std::runtime_error("Invalid exported argument list");
    ResultGuard r(entry.callback(entry.data, env, owned->value.as.array.data, owned->value.as.array.size));
    r.check();
    return to_avs(*entry.host, env, r.value.value);
  } catch (const std::exception& e) {
    env->ThrowError("Garnet filter: %s", e.what());
  } catch (...) {
    env->ThrowError("Garnet filter: unknown callback failure");
  }
  return AVSValue();
}
garnet_result GARNET_CALL register_filter(void* identity, void* context, garnet_string name, garnet_string signature,
                                          garnet_callback callback, void* data) {
  try {
    auto& host = *static_cast<Host*>(identity);
    auto* env = static_cast<IScriptEnvironment*>(context);
    if (!env || !callback)
      return error("Invalid filter registration");
    const auto function = text(name), params = text(signature);
    if (function.empty() || function.find('\0') != std::string::npos || params.find('\0') != std::string::npos)
      return error("Invalid filter registration strings");
    if (env->FunctionExists(function.c_str()))
      return error(("Exported filter name already exists: " + function).c_str());
    auto entry = std::make_unique<Export>(Export{&host, callback, data});
    auto* token = entry.get();
    host.exports.push_back(std::move(entry));
    env->AddFunction(env->SaveString(function.c_str()), env->SaveString(params.c_str()), exported_filter, token);
    return {};
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown filter registration failure");
  }
}
AVSValue __cdecl import_ruby(AVSValue args, void* data, IScriptEnvironment* env) {
  auto& host = *static_cast<Host*>(data);
  try {
    auto path = std::filesystem::u8path(args[0].AsString());
    if (path.is_relative()) {
      auto dir = env->GetVarDef("$ScriptDirUtf8$");
      if (dir.IsString())
        path = std::filesystem::u8path(dir.AsString()) / path;
    }
    path = std::filesystem::absolute(path).lexically_normal();
    const auto filename = path.u8string();
    ResultGuard r(garnet_import(host.session, env, span(filename)));
    if (r.value.status != GARNET_OK)
      throw std::runtime_error(filename + ": " + text(r.value.error));
    return to_avs(host, env, r.value.value);
  } catch (const std::exception& e) {
    env->ThrowError("ImportRuby: %s", e.what());
  }
  return AVSValue();
}
void __cdecl shutdown(void* p, IScriptEnvironment*) {
  delete static_cast<Host*>(p);
}
} // namespace
#ifdef _WIN32
#define GARNET_EXPORT __declspec(dllexport)
#else
#define GARNET_EXPORT __attribute__((visibility("default")))
#endif
extern "C" GARNET_EXPORT const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env,
                                                                   const AVS_Linkage* linkage) {
  AVS_linkage = linkage;
  env->CheckVersion(11);
  try {
    if (env->FunctionExists("ImportRuby"))
      throw std::runtime_error("ImportRuby already registered");
    auto host = std::make_unique<Host>();
    host->api = {GARNET_CONTRACT_REVISION,
                 sizeof(garnet_host),
                 host.get(),
                 invoke,
                 retain_clip,
                 release_clip,
                 register_filter,
                 get_var,
                 set_var,
                 retain_function,
                 release_function,
                 invoke_function};
    ResultGuard r(garnet_create(&host->api, &host->session));
    r.check();
    env->AtExit(shutdown, host.get());
    auto* owned = host.release();
    env->AddFunction("ImportRuby", "s", import_ruby, owned);
    return "Garnet Ruby binding";
  } catch (const std::exception& e) {
    env->ThrowError("Garnet initialization: %s", e.what());
  }
  return nullptr;
}
