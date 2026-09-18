#define AVSC_NO_DECLSPEC
#include <windows.h>
#include <cstdlib>
#include <avisynth_c.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: avs_tests runtime plugin script\n");
    return 2;
  }
  auto dll = LoadLibraryA(argv[1]);
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
    auto loaded = call("LoadPlugin", argv[2]);
    avs_release_value(loaded);
    auto output = call("Import", argv[3]);
    if (!avs_is_clip(output)) {
      avs_release_value(output);
      throw std::runtime_error("Ruby main did not return clip");
    }
    ruby = avs_take_clip(output, env);
    avs_release_value(output);
    auto expected = call("Eval", "ColorBars(width=720,height=480,pixel_type=\"YV12\").BilinearResize(360,240)");
    reference = avs_take_clip(expected, env);
    avs_release_value(expected);
    const auto* vi = avs_get_video_info(ruby);
    if (vi->width != 360 || vi->height != 240)
      throw std::runtime_error("Wrong output size");
    for (int n : {0, 1, 3, 1}) {
      auto* actual_frame = avs_get_frame(ruby, n);
      if (!actual_frame)
        throw std::runtime_error(avs_clip_get_error(ruby));
      auto* expected_frame = avs_get_frame(reference, n);
      if (!expected_frame) {
        avs_release_video_frame(actual_frame);
        throw std::runtime_error(avs_clip_get_error(reference));
      }
      bool equal = true;
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
    std::puts("Ruby main returned 360x240 YV12; frames 0,1,3,1 match native AVS byte-for-byte");
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
