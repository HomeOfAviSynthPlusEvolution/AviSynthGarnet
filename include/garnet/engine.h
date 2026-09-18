#ifndef GARNET_ENGINE_H
#define GARNET_ENGINE_H
#include "host.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct garnet_session garnet_session;
/* On failure *out is null. Table is copied; identity remains borrowed. */
garnet_result GARNET_CALL garnet_create(const garnet_host* host, garnet_session** out);
/* Returns the last expression. Fresh parser locals, shared VM/constants.
 * Failed evaluation disables the session. These evaluation entrypoints reject
 * concurrent/reentrant evaluation; file imports and registered callbacks allow
 * bounded same-thread nesting.
 * Source and filename are borrowed UTF-8 spans. */
garnet_result GARNET_CALL garnet_evaluate(garnet_session*, void* call_context, garnet_string source,
                                          garnet_string filename);
/* Import a UTF-8 file path once per session. Repeated imports return the cached
 * final expression; require_relative shares this registry and returns bool. */
garnet_result GARNET_CALL garnet_import(garnet_session*, void* call_context, garnet_string filename);
/* Execute a pipeline file every time, ignoring the library cache. The host
 * supplies a scoped 'last' clip through get_var. Requires a clip result. */
garnet_result GARNET_CALL garnet_run_script(garnet_session*, void* call_context, garnet_string filename);
/* Caller ensures no active calls and releases outstanding results first. */
void GARNET_CALL garnet_destroy(garnet_session*);
#ifdef __cplusplus
}
#endif
#endif
