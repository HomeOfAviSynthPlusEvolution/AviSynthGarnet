#include <avisynth.h>
#include <garnet/engine.h>
#include "result.hpp"
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <mutex>

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
  IScriptEnvironment* environment = nullptr;
  garnet_session* session = nullptr;
  std::vector<std::unique_ptr<Export>> exports;
  std::mutex exports_gate;
  std::unordered_set<std::string> export_names;
  std::vector<std::unique_ptr<Export>> functions;
  std::mutex functions_gate;
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
      if (s.find('\0') != std::string::npos || s.size() > (std::numeric_limits<int>::max)())
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
    // Runtime-created filters may keep their constructor environment until
    // Ruby GC releases them, after the Prefetch worker has gone away. The
    // initialization environment lives through AtExit and dispatches runtime
    // variables through AviSynth's current-thread TLS.
    auto value =
        host.environment->Invoke(function.c_str(), AVSValue(values.data(), static_cast<int>(count)), pointers.data());
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
// Script variables cannot count nested imports: each AVS function/context
// hides its caller's locals. Keep the guard on the native thread instead.
struct PipelineDepth {
  static thread_local PipelineDepth* current;
  PipelineDepth* previous;
  const std::filesystem::path& path;
  unsigned depth;
  explicit PipelineDepth(const std::filesystem::path& path)
      : previous(current), path(path), depth(previous ? previous->depth + 1 : 1) {
    for (auto* entry = previous; entry; entry = entry->previous)
      if (std::filesystem::equivalent(entry->path, path))
        throw std::runtime_error("Circular pipeline import");
    // Native Import/Eval consumes much more stack than a Ruby VM call.
    if (depth > 16)
      throw std::runtime_error("Pipeline script nesting limit exceeded");
    current = this;
  }
  ~PipelineDepth() { current = previous; }
};
thread_local PipelineDepth* PipelineDepth::current = nullptr;

struct ScriptMetadata {
  IScriptEnvironment* env;
  const char* names[6] = {"$ScriptName$",     "$ScriptFile$",     "$ScriptDir$",
                          "$ScriptNameUtf8$", "$ScriptFileUtf8$", "$ScriptDirUtf8$"};
  AVSValue values[6];
  explicit ScriptMetadata(IScriptEnvironment* env) : env(env) {
    for (int i = 0; i < 6; ++i)
      values[i] = env->GetVarDef(names[i]);
  }
  ~ScriptMetadata() {
    for (int i = 0; i < 6; ++i)
      env->SetGlobalVar(names[i], values[i]);
  }
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
    // The bool reports insertion versus replacement, not success/failure.
    if (global)
      env->SetGlobalVar(saved, converted);
    else
      env->SetVar(saved, converted);
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
    auto folded = function;
    for (auto& c : folded)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    {
      // Reserve names across concurrent registrations, but never hold this
      // lock across AVS calls: FunctionExists can trigger script autoload.
      std::lock_guard<std::mutex> lock(host.exports_gate);
      const auto inserted = host.export_names.insert(folded);
      if (!inserted.second)
        return error(("Exported filter name already exists: " + function).c_str());
      try {
        host.exports.push_back(std::move(entry));
      } catch (...) {
        host.export_names.erase(inserted.first);
        throw;
      }
    }
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
AVSValue __cdecl dispatch_function(AVSValue args, void* data, IScriptEnvironment* env) {
  auto& host = *static_cast<Host*>(data);
  const auto token = args[0].AsLong();
  Export* entry = nullptr;
  {
    // Lookups occur before entering the VM; another callback may append a
    // function and reallocate the vector. Never hold this lock across Ruby.
    std::lock_guard<std::mutex> lock(host.functions_gate);
    if (token >= 1 && static_cast<uint64_t>(token) <= host.functions.size())
      entry = host.functions[static_cast<size_t>(token - 1)].get();
  }
  if (!entry || !args[1].IsArray())
    env->ThrowError("Garnet: invalid function dispatch");
  return exported_filter(args[1], entry, env);
}
std::string function_source(const std::string& signature) {
  std::string params, values;
  std::unordered_set<std::string> used;
  size_t count = 0;
  for (size_t i = 0; i < signature.size(); ++count) {
    if (count >= 256)
      throw std::runtime_error("Too many function parameters");
    const bool optional = signature[i] == '[';
    std::string name = "__garnet_arg_" + std::to_string(count);
    if (optional) {
      const auto end = signature.find(']', ++i);
      if (end == std::string::npos)
        throw std::runtime_error("Unclosed function parameter");
      name = signature.substr(i, end - i);
      i = end + 1;
      if (name.empty())
        throw std::runtime_error("Empty function parameter");
      for (size_t j = 0; j < name.size(); ++j) {
        const auto c = name[j];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (j && c >= '0' && c <= '9')))
          throw std::runtime_error("Invalid function parameter");
      }
    }
    auto folded = name;
    for (auto& c : folded)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    if ((optional && folded.find("__garnet_") == 0) || !used.insert(folded).second)
      throw std::runtime_error("Reserved or duplicate function parameter");
    if (folded == "function")
      throw std::runtime_error("Reserved AVS function parameter: function");
    if (i == signature.size())
      throw std::runtime_error("Missing function parameter type");
    const char* type;
    switch (signature[i++]) {
      case 'c':
        type = "clip";
        break;
      case 'b':
        type = "bool";
        break;
      case 'i':
        type = "int";
        break;
      case 'f':
        type = "float";
        break;
      case 's':
        type = "string";
        break;
      case 'n':
        type = "func";
        break;
      case '.':
        type = "val";
        break;
      default:
        throw std::runtime_error("Unsupported function parameter type");
    }
    if (count) {
      params += ",";
      values += ",";
    }
    params += std::string(type) + " " + (optional ? "\"" + name + "\"" : name);
    values += name;
  }
  return "function [__garnet_token](" + params + ") { return __GarnetDispatch(__garnet_token, args=[" + values + "]) }";
}
garnet_result GARNET_CALL make_function(void* identity, void* context, garnet_string signature,
                                        garnet_callback callback, void* data) {
  try {
    auto& host = *static_cast<Host*>(identity);
    auto* env = static_cast<IScriptEnvironment*>(context);
    if (!env || !callback)
      return error("Cannot create Garnet function");
    const auto source = function_source(text(signature));
    auto entry = std::make_unique<Export>(Export{&host, callback, data});
    size_t token;
    {
      std::lock_guard<std::mutex> lock(host.functions_gate);
      if (host.functions.size() >= 4096)
        return error("Cannot create Garnet function");
      host.functions.push_back(std::move(entry));
      token = host.functions.size();
    }
    LocalContext local(env);
    env->SetVar("__garnet_token", AVSValue(static_cast<int64_t>(token)));
    const auto value = env->Invoke("Eval", AVSValue(source.c_str()));
    if (!value.IsFunction())
      return error("AVS did not create a function value");
    return result(from_avs(host, value));
  } catch (const AvisynthError& e) {
    return error(e.msg);
  } catch (const std::exception& e) {
    return error(e.what());
  } catch (...) {
    return error("Unknown function creation failure");
  }
}
AVSValue __cdecl import_script(AVSValue args, void* data, IScriptEnvironment* env) {
  auto& host = *static_cast<Host*>(data);
  try {
    auto path = std::filesystem::u8path(args[1].AsString());
    if (path.is_relative()) {
      const auto dir = env->GetVarDef("$ScriptDirUtf8$");
      if (dir.IsString())
        path = std::filesystem::u8path(dir.AsString()) / path;
    }
    path = std::filesystem::canonical(path);
    const auto filename = path.u8string();
    auto extension = path.extension().u8string();
    for (auto& c : extension)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    if (extension != ".rb" && extension != ".avs" && extension != ".avsi")
      throw std::runtime_error("ImportScript expects .avs, .avsi or .rb");
    // Local variables and last are scoped; explicit globals and registered
    // functions keep their normal AVS semantics.
    PipelineDepth depth(path);
    LocalContext local(env);
    env->SetVar("last", args[0]);
    AVSValue value;
    if (extension == ".rb") {
      ResultGuard r(garnet_run_script(host.session, env, span(filename)));
      r.check();
      value = to_avs(host, env, r.value.value);
    } else {
      // filename is UTF-8; request the native Import UTF-8 path mode.
      AVSValue values[] = {env->SaveString(filename.c_str()), true};
      const char* names[] = {nullptr, "utf8"};
      // Some upstream versions restore these globals only on successful Import.
      ScriptMetadata metadata(env);
      value = env->Invoke("Import", AVSValue(values, 2), names);
    }
    if (!value.IsClip())
      throw std::runtime_error("Pipeline script must return a clip");
    return value;
  } catch (const std::exception& e) {
    env->ThrowError("ImportScript: %s", e.what());
  }
  return AVSValue();
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
    if (env->FunctionExists("ImportScript"))
      throw std::runtime_error("ImportScript already registered");
    if (env->FunctionExists("__GarnetDispatch"))
      throw std::runtime_error("Garnet dispatcher already registered");
    auto host = std::make_unique<Host>();
    if (env->GetEnvProperty(AEP_THREAD_ID) != 0)
      throw std::runtime_error("Load Garnet during script initialization, not in a frame worker");
    host->environment = env;
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
                 invoke_function,
                 make_function};
    ResultGuard r(garnet_create(&host->api, &host->session));
    r.check();
    env->AtExit(shutdown, host.get());
    auto* owned = host.release();
    env->AddFunction("ImportRuby", "s", import_ruby, owned);
    env->AddFunction("ImportScript", "cs", import_script, owned);
    env->AddFunction("__GarnetDispatch", "i[args].", dispatch_function, owned);
    return "Garnet Ruby binding";
  } catch (const std::exception& e) {
    env->ThrowError("Garnet initialization: %s", e.what());
  }
  return nullptr;
}
