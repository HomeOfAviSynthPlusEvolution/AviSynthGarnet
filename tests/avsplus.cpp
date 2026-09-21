#define AVSC_NO_DECLSPEC
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <avisynth_c.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: avs_tests runtime plugin script [stamp | --reference native.avs] [--autoload dir]\n");
    return 2;
  }
  const char* runtime_path = argv[1];
  const char* plugin_path = argv[2];
  const char* script_path = argv[3];
  const char* reference_path = nullptr;
  std::vector<const char*> autoload_dirs;
  bool stamp = false;

  for (int i = 4; i < argc; ++i) {
    if (std::strcmp(argv[i], "stamp") == 0) {
      stamp = true;
    } else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc) {
      reference_path = argv[++i];
    } else if (std::strcmp(argv[i], "--autoload") == 0 && i + 1 < argc) {
      autoload_dirs.push_back(argv[++i]);
    } else {
      std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
      return 2;
    }
  }
  const bool reference_script = reference_path != nullptr;
  auto dll = LoadLibraryA(runtime_path);
  if (!dll) {
    std::fprintf(stderr, "Cannot load runtime: %lu\n", GetLastError());
    return 1;
  }
#define LOAD(name)                                                                                                     \
  auto name = reinterpret_cast<name##_func>(GetProcAddress(dll, #name));                                               \
  if (!name) {                                                                                                         \
    std::fprintf(stderr, "Missing %s\n", #name);                                                                       \
    FreeLibrary(dll);                                                                                                  \
    return 1;                                                                                                          \
  }
  LOAD(avs_create_script_environment)
  LOAD(avs_delete_script_environment)
  LOAD(avs_invoke)
  LOAD(avs_release_value)
  LOAD(avs_take_clip)
  LOAD(avs_release_clip)
  LOAD(avs_get_video_info)
  LOAD(avs_get_frame)
  LOAD(avs_release_video_frame)
  LOAD(avs_get_read_ptr_p)
  LOAD(avs_get_pitch_p)
  LOAD(avs_get_row_size_p)
  LOAD(avs_get_height_p)
  LOAD(avs_clip_get_error)
  LOAD(avs_get_frame_props_ro)
  LOAD(avs_prop_get_int)
#undef LOAD
  auto* env = avs_create_script_environment(6);
  if (!env) {
    FreeLibrary(dll);
    return 1;
  }
  AVS_Clip *ruby = nullptr, *reference = nullptr;
  int status = 0;
  try {
    const auto call = [&](const char* name, const char* arg) {
      auto r = avs_invoke(env, name, avs_new_value_string(arg), nullptr);
      if (avs_is_error(r)) {
        const std::string message = avs_as_string(r);
        avs_release_value(r);
        throw std::runtime_error(message);
      }
      return r;
    };
    // Isolate the test from user autoloads, including an older Garnet.dll.
    auto cleared = avs_invoke(env, "ClearAutoloadDirs", avs_new_value_array(nullptr, 0), nullptr);
    if (avs_is_error(cleared)) {
      const std::string message = avs_as_string(cleared);
      avs_release_value(cleared);
      throw std::runtime_error(message);
    }
    avs_release_value(cleared);
    for (const char* dir : autoload_dirs) {
      AVS_Value args[2] = {avs_new_value_string(dir), avs_new_value_bool(false)};
      auto added = avs_invoke(env, "AddAutoloadDir", avs_new_value_array(args, 2), nullptr);
      if (avs_is_error(added)) {
        const std::string message = avs_as_string(added);
        avs_release_value(added);
        throw std::runtime_error(message);
      }
      avs_release_value(added);
    }
    auto loaded = call("LoadPlugin", plugin_path);
    avs_release_value(loaded);
    auto output = call("Import", script_path);
    if (!avs_is_clip(output)) {
      avs_release_value(output);
      throw std::runtime_error("Ruby main did not return clip");
    }
    ruby = avs_take_clip(output, env);
    avs_release_value(output);
    auto expected =
        reference_script
            ? call("Import", reference_path)
            : call("Eval", stamp
                               ? "ColorBars(width=720,height=480,pixel_type=\"YV12\").ScriptClip(\"last.BilinearResize("
                                 "360,240).PointResize(720,480)\").PointResize(360,240)"
                               : "ColorBars(width=720,height=480,pixel_type=\"YV12\").BilinearResize(360,240)");
    if (!avs_is_clip(expected)) {
      avs_release_value(expected);
      throw std::runtime_error("Native reference did not return clip");
    }
    reference = avs_take_clip(expected, env);
    avs_release_value(expected);
    const auto* vi = avs_get_video_info(ruby);
    const auto* ref_vi = avs_get_video_info(reference);
    if (vi->width != ref_vi->width || vi->height != ref_vi->height || vi->pixel_type != ref_vi->pixel_type ||
        vi->fps_numerator != ref_vi->fps_numerator || vi->fps_denominator != ref_vi->fps_denominator ||
        (reference_script && vi->num_frames != ref_vi->num_frames))
      throw std::runtime_error("Video metadata differs from native AVS graph");
    std::vector<int> requests{0, 1, 3, 1};
    if (reference_script) {
      if (vi->num_frames <= 0)
        throw std::runtime_error("Example returned an empty clip");
      requests.clear();
      // Examples are short synthetic clips. Read every frame up to this bound,
      // then seek/repeat to catch callbacks that assume monotonic frame requests.
      for (int n = 0; n < (std::min)(vi->num_frames, 64); ++n)
        requests.push_back(n);
      for (int n : {vi->num_frames - 1, 0, vi->num_frames / 2, 0})
        requests.push_back(n);
    } else if (stamp) {
      requests.clear();
      for (int n = 0; n < 64; ++n)
        requests.push_back(n);
      for (int n : {31, 2, 48, 1})
        requests.push_back(n);
    }
    for (int n : requests) {
      auto* actual_frame = avs_get_frame(ruby, n);
      if (!actual_frame)
        throw std::runtime_error(avs_clip_get_error(ruby));
      auto* expected_frame = avs_get_frame(reference, n);
      if (!expected_frame) {
        avs_release_video_frame(actual_frame);
        throw std::runtime_error(avs_clip_get_error(reference));
      }
      bool equal = true;
      if (stamp) {
        int error = 0;
        const auto* properties = avs_get_frame_props_ro(env, actual_frame);
        const auto stamp = avs_prop_get_int(env, properties, "garnet_n", 0, &error);
        equal = !error && stamp == n;
        if (!equal)
          std::fprintf(stderr, "Frame %d: missing/wrong Ruby stamp (error=%d, stamp=%lld)\n", n, error,
                       static_cast<long long>(stamp));
      }
      for (int plane : {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V}) {
        int rows = avs_get_height_p(actual_frame, plane), bytes = avs_get_row_size_p(actual_frame, plane);
        const auto* a = avs_get_read_ptr_p(actual_frame, plane);
        const auto* b = avs_get_read_ptr_p(expected_frame, plane);
        if (rows != avs_get_height_p(expected_frame, plane) || bytes != avs_get_row_size_p(expected_frame, plane))
          equal = false;
        else
          for (int y = 0; y < rows; ++y)
            if (std::memcmp(a + y * avs_get_pitch_p(actual_frame, plane),
                            b + y * avs_get_pitch_p(expected_frame, plane), bytes))
              equal = false;
      }
      avs_release_video_frame(expected_frame);
      avs_release_video_frame(actual_frame);
      if (!equal)
        throw std::runtime_error("Frame differs from native AVS graph");
    }
    std::printf("Script returned %dx%d; %zu frame requests match native AVS byte-for-byte\n", vi->width, vi->height,
                requests.size());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    status = 1;
  }
  if (reference)
    avs_release_clip(reference);
  if (ruby)
    avs_release_clip(ruby);
  avs_delete_script_environment(env);
  FreeLibrary(dll);
  return status;
}
