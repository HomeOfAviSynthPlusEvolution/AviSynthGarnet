#include <avisynth.h>
#include <garnet/engine.h>
#include "result.hpp"
#include <filesystem>
#include <limits>

const AVS_Linkage* AVS_linkage = nullptr;
namespace {
using namespace garnet;
struct Host;
struct Clip {
  Host* host;
  PClip clip;
};
struct Host {
  garnet_host api{};
  garnet_session* session = nullptr;
  ~Host() { garnet_destroy(session); }
};
void GARNET_CALL release_clip(void*, void* p) {
  delete static_cast<Clip*>(p);
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
  } else
    throw std::runtime_error("AVS function values are not yet supported");
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
    host->api = {GARNET_CONTRACT_REVISION, sizeof(garnet_host), host.get(), invoke, retain_clip, release_clip};
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
