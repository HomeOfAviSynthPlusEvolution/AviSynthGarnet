#ifndef GARNET_HOST_H
#define GARNET_HOST_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#define GARNET_CALL __cdecl
#else
#define GARNET_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Private exact-match contract, not a stable public ABI. No exceptions may
 * cross it. Inputs are borrowed. Release results before destroying the host.
 * Destroy must not race with calls. Handles belong to exactly one host. */
#define GARNET_CONTRACT_REVISION 1u
enum { GARNET_UNDEFINED, GARNET_BOOL, GARNET_INT, GARNET_FLOAT, GARNET_STRING, GARNET_ARRAY, GARNET_CLIP };
enum { GARNET_OK, GARNET_ERROR, GARNET_INVALID_CONTRACT, GARNET_BUSY };
typedef struct garnet_string {
  const char* data;
  size_t size;
} garnet_string;
typedef struct garnet_value garnet_value;
struct garnet_value {
  uint32_t type;
  union {
    int64_t integer;
    double floating;
    garnet_string string;
    struct {
      const garnet_value* data;
      size_t size;
    } array;
    void* handle;
  } as;
};
typedef struct garnet_result {
  uint32_t status;
  garnet_value value;
  garnet_string error;
  void* owner;
  void(GARNET_CALL* release)(void* owner);
} garnet_result;
typedef struct garnet_host {
  uint32_t revision;
  uint32_t size;
  void* identity;
  /* call_context is this invocation's borrowed environment, not a saved env. */
  garnet_result(GARNET_CALL* invoke)(void* identity, void* call_context, garnet_string name, const garnet_value* args,
                                     const garnet_string* names, size_t count);
  /* Retain returns a new owned handle in a result, or an error. */
  garnet_result(GARNET_CALL* retain_clip)(void* identity, void* handle);
  void(GARNET_CALL* release_clip)(void* identity, void* handle);
} garnet_host;
#ifdef __cplusplus
}
#endif
#endif
