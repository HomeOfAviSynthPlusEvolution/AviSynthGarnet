#include <garnet/engine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                                             \
      exit(1);                                                                                                         \
    }                                                                                                                  \
  } while (0)
typedef struct fake_host {
  int calls;
  int clips;
  int context;
  garnet_session* session;
  garnet_callback callback;
  void* callback_data;
  garnet_value variable;
} fake_host;
typedef struct fake_clip {
  fake_host* host;
  garnet_callback callback;
  void* callback_data;
} fake_clip;
static garnet_string str(const char* s) {
  garnet_string v = {s, strlen(s)};
  return v;
}
static void release_result(garnet_result r) {
  if (r.release)
    r.release(r.owner);
}
static void GARNET_CALL free_clip(void* identity, void* p) {
  fake_host* host = (fake_host*)identity;
  --host->clips;
  free(p);
}
static void GARNET_CALL free_clip_result(void* p) {
  fake_clip* clip = (fake_clip*)p;
  free_clip(clip->host, p);
}
static garnet_result clip_result(fake_host* host) {
  garnet_result r = {0};
  fake_clip* clip = (fake_clip*)malloc(sizeof(fake_clip));
  CHECK(clip);
  clip->host = host;
  clip->callback = NULL;
  clip->callback_data = NULL;
  ++host->clips;
  r.value.type = GARNET_CLIP;
  r.value.as.handle = clip;
  r.owner = clip;
  r.release = free_clip_result;
  return r;
}
static garnet_result GARNET_CALL retain(void* identity, void* handle) {
  garnet_result r;
  CHECK(((fake_clip*)handle)->host == identity);
  r = clip_result((fake_host*)identity);
  *(fake_clip*)r.value.as.handle = *(fake_clip*)handle;
  return r;
}
static int equal(garnet_string a, const char* b) {
  return a.size == strlen(b) && !memcmp(a.data, b, a.size);
}
static garnet_result GARNET_CALL invoke(void* identity, void* context, garnet_string name, const garnet_value* args,
                                        const garnet_string* names, size_t count) {
  fake_host* host = (fake_host*)identity;
  garnet_result r = {0};
  CHECK(context == &host->context);
  ++host->calls;
  if (equal(name, "Echo")) {
    CHECK(count == 1);
    r.value = args[0];
  } else if (equal(name, "Source")) {
    CHECK(count == 0);
    return clip_result(host);
  } else if (equal(name, "Lambda")) {
    r = clip_result(host);
    r.value.type = GARNET_FUNCTION;
    return r;
  } else if (equal(name, "Resize")) {
    CHECK(count == 3 && args[0].type == GARNET_CLIP && names[0].size == 0);
    CHECK(equal(names[1], "width") && args[1].as.integer == 320);
    CHECK(equal(names[2], "height") && args[2].as.integer == 240);
    return clip_result(host);
  } else if (equal(name, "Keywords")) {
    CHECK(count == 3 && args[0].type == GARNET_UNDEFINED);
    CHECK(equal(names[1], "enabled") && args[1].type == GARNET_BOOL && args[1].as.integer == 0);
    CHECK(equal(names[2], "amount") && args[2].as.integer == 0);
  } else if (equal(name, "Reenter")) {
    garnet_result nested = garnet_evaluate(host->session, context, str("1"), str("nested.rb"));
    CHECK(nested.status == GARNET_BUSY);
    release_result(nested);
  } else if (equal(name, "Twice")) {
    CHECK(host->callback);
    return host->callback(host->callback_data, context, args, count);
  } else {
    r.status = GARNET_ERROR;
    r.error = str("Fake host: unknown filter");
  }
  return r;
}
static garnet_result GARNET_CALL register_filter(void* identity, void* context, garnet_string name,
                                                 garnet_string signature, garnet_callback callback, void* data) {
  fake_host* host = (fake_host*)identity;
  garnet_result r = {0};
  CHECK(context == &host->context);
  CHECK(equal(name, "Twice") && equal(signature, "i"));
  if (host->callback) {
    r.status = GARNET_ERROR;
    r.error = str("duplicate export");
    return r;
  }
  host->callback = callback;
  host->callback_data = data;
  return r;
}
static garnet_result GARNET_CALL get_var(void* identity, void* context, garnet_string name) {
  fake_host* host = (fake_host*)identity;
  garnet_result r = {0};
  CHECK(context == &host->context && equal(name, "test"));
  r.value = host->variable;
  return r;
}
static garnet_result GARNET_CALL set_var(void* identity, void* context, garnet_string name, const garnet_value* value,
                                         int global) {
  fake_host* host = (fake_host*)identity;
  garnet_result r = {0};
  CHECK(context == &host->context && equal(name, "test"));
  CHECK(value->type == GARNET_INT || value->type == GARNET_BOOL || value->type == GARNET_UNDEFINED);
  CHECK(global == 0 || global == 1);
  host->variable = *value;
  return r;
}
static garnet_result GARNET_CALL retain_function(void* identity, void* handle) {
  garnet_result r = retain(identity, handle);
  r.value.type = GARNET_FUNCTION;
  return r;
}
static garnet_result GARNET_CALL invoke_function(void* identity, void* context, void* handle, const garnet_value* args,
                                                 const garnet_string* names, size_t count) {
  fake_host* host = (fake_host*)identity;
  garnet_result r = {0};
  CHECK(((fake_clip*)handle)->host == host && context == &host->context);
  if (((fake_clip*)handle)->callback)
    return ((fake_clip*)handle)->callback(((fake_clip*)handle)->callback_data, context, args, count);
  CHECK(count == 1 && args[0].type == GARNET_INT && names[0].size == 0);
  r.value.type = GARNET_INT;
  r.value.as.integer = args[0].as.integer * 2;
  return r;
}
static garnet_result GARNET_CALL make_function(void* identity, void* context, garnet_string signature,
                                               garnet_callback callback, void* data) {
  fake_host* host = (fake_host*)identity;
  garnet_result r = clip_result(host);
  CHECK(context == &host->context && equal(signature, "i"));
  r.value.type = GARNET_FUNCTION;
  ((fake_clip*)r.value.as.handle)->callback = callback;
  ((fake_clip*)r.value.as.handle)->callback_data = data;
  return r;
}
static garnet_host api(fake_host* host) {
  garnet_host result = {GARNET_CONTRACT_REVISION,
                        sizeof(garnet_host),
                        host,
                        invoke,
                        retain,
                        free_clip,
                        register_filter,
                        get_var,
                        set_var,
                        retain_function,
                        free_clip,
                        invoke_function,
                        make_function};
  return result;
}
static garnet_session* create(fake_host* host) {
  garnet_session* s = NULL;
  garnet_host h = api(host);
  garnet_result r = garnet_create(&h, &s);
  CHECK(r.status == GARNET_OK && s);
  release_result(r);
  host->session = s;
  return s;
}
static garnet_result eval(fake_host* host, const char* code) {
  return garnet_evaluate(host->session, &host->context, str(code), str("contract.avs.rb"));
}
static void failure_one_of(const char* code, const char* expected, const char* alternative) {
  fake_host host = {0};
  garnet_session* s = create(&host);
  garnet_result r = eval(&host, code);
  const int matches = r.error.data &&
      (strstr(r.error.data, expected) || (alternative && strstr(r.error.data, alternative)));
  if (r.status == GARNET_OK || !matches)
    fprintf(stderr, "Ruby: %s\nExpected error: %s%s%s\nActual (%d): %.*s\n", code, expected,
            alternative ? " OR " : "", alternative ? alternative : "",
            (int)r.status, (int)r.error.size, r.error.data ? r.error.data : "");
  CHECK(r.status != GARNET_OK);
  CHECK(matches);
  release_result(r);
  r = eval(&host, "1");
  CHECK(r.status != GARNET_OK);
  release_result(r);
  garnet_destroy(s);
  CHECK(host.clips == 0);
}
static void failure(const char* code, const char* expected) {
  failure_one_of(code, expected, NULL);
}
int main(int argc, char** argv) {
  fake_host host = {0};
  garnet_host h = api(&host);
  garnet_session* s = NULL;
  garnet_result r;
  h.revision++;
  r = garnet_create(&h, &s);
  CHECK(r.status == GARNET_INVALID_CONTRACT && !s);
  release_result(r);
  h = api(&host);
  h.size--;
  r = garnet_create(&h, &s);
  CHECK(r.status == GARNET_INVALID_CONTRACT && !s);
  release_result(r);
  s = create(&host);
  r = eval(&host, "9223372036854775807");
  if (r.status != GARNET_OK)
    fprintf(stderr, "INT64_MAX: %.*s\n", (int)r.error.size, r.error.data);
  CHECK(r.status == GARNET_OK && r.value.as.integer == INT64_MAX);
  release_result(r);
  r = eval(&host, "-9223372036854775808");
  if (r.status != GARNET_OK)
    fprintf(stderr, "INT64_MIN: %.*s\n", (int)r.error.size, r.error.data);
  CHECK(r.status == GARNET_OK && r.value.as.integer == INT64_MIN);
  release_result(r);
  r = eval(&host, "wide = 9223372036854775808; AVS.Echo(wide - 1)");
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_INT && r.value.as.integer == INT64_MAX);
  release_result(r);
  r = eval(&host, "raise unless MRUBY_VERSION == '4.0.0'; "
                  "case {size: [320, 240]}; in {size: [width, height]}; width + height; else; 0; end");
  if (r.status != GARNET_OK)
    fprintf(stderr, "%.*s\n", (int)r.error.size, r.error.data);
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_INT && r.value.as.integer == 560);
  release_result(r);
  r = eval(&host, "temporary = 42; temporary");
  CHECK(r.status == GARNET_OK && r.value.as.integer == 42);
  release_result(r);
  r = eval(&host, "begin; temporary; false; rescue NameError; true; end");
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_BOOL && r.value.as.integer);
  release_result(r);
  r = eval(&host, "temporary = 17; $saved = -> { temporary }; nil");
  CHECK(r.status == GARNET_OK);
  release_result(r);
  r = eval(&host, "temporary = 99; $saved.call");
  CHECK(r.status == GARNET_OK && r.value.as.integer == 17);
  release_result(r);
  r = eval(&host,
           "AVS.Echo([nil, false, 0, 9223372036854775807, -9223372036854775808, 1.23456789012345, \"a\\0b\", [7]])");
  if (r.status != GARNET_OK)
    fprintf(stderr, "%.*s\n", (int)r.error.size, r.error.data);
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_ARRAY && r.value.as.array.size == 8);
  CHECK(r.value.as.array.data[3].as.integer == INT64_MAX);
  CHECK(r.value.as.array.data[4].as.integer == INT64_MIN);
  CHECK(r.value.as.array.data[5].as.floating == 1.23456789012345);
  CHECK(r.value.as.array.data[6].as.string.size == 3);
  release_result(r);
  r = eval(&host, "AVS.Keywords(nil, omitted: nil, enabled: false, amount: 0)");
  CHECK(r.status == GARNET_OK);
  release_result(r);
  r = eval(&host, "AVS.Reenter");
  CHECK(r.status == GARNET_OK);
  release_result(r);
  r = eval(&host, "factor = 2; $rubyfn = AVS.function(args: {x: :int}, returns: :int) { |x| x * factor }; GC.start; "
                  "$rubyfn.call(21)");
  if (r.status != GARNET_OK)
    fprintf(stderr, "%.*s\n", (int)r.error.size, r.error.data);
  CHECK(r.status == GARNET_OK && r.value.as.integer == 42);
  release_result(r);
  r = eval(&host, "$fn = AVS.Lambda; GC.start; $fn.call(21)");
  CHECK(r.status == GARNET_OK && r.value.as.integer == 42);
  release_result(r);
  r = eval(&host, "AVS.send(:remove_const, :Function); GC.start; AVS.Lambda");
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_FUNCTION);
  release_result(r);
  r = eval(&host, "raise unless AVS.get_var(:test, 42) == 42; AVS[:test] = false; raise unless AVS[:test] == false; "
                  "AVS.set_global_var(:test, 0); AVS.get_var(:test, 42)");
  CHECK(r.status == GARNET_OK && r.value.as.integer == 0);
  release_result(r);
  r = eval(&host, "factor = 2; AVS.export(:Twice, 'i') { |x| x == 0 ? 0 : AVS.Twice(x - 1) + factor }; AVS.Twice(21)");
  if (r.status != GARNET_OK)
    fprintf(stderr, "%.*s\n", (int)r.error.size, r.error.data);
  CHECK(r.status == GARNET_OK && r.value.as.integer == 42);
  release_result(r);
  r = eval(&host, "GC.start; AVS.Twice(3)");
  CHECK(r.status == GARNET_OK && r.value.as.integer == 6);
  release_result(r);
  r = eval(&host, "clip = AVS.Source; clip.Resize(width: 320, height: 240)");
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_CLIP);
  release_result(r);
  r = eval(&host, "GC.start; nil");
  CHECK(r.status == GARNET_OK);
  release_result(r);
  r = eval(&host, "AVS.send(:remove_const, :Clip); nil");
  CHECK(r.status == GARNET_OK);
  release_result(r);
  r = eval(&host, "GC.start; AVS.Source");
  CHECK(r.status == GARNET_OK && r.value.type == GARNET_CLIP);
  release_result(r);
  garnet_destroy(s);
  CHECK(host.clips == 0);
  failure("AVS.Echo(Width: 1, width: 2)", "Duplicate");
  failure("AVS.Echo(9223372036854775808)", "RangeError");
  failure("AVS.Echo(-9223372036854775809)", "RangeError");
  failure("a = []; a << a; a", "nesting");
  failure("raise 'deliberate'", "deliberate");
  failure("raise 'line check'", "contract.avs.rb:1");
  failure("def broken(", "syntax");
  failure("AVS.Missing", "unknown filter");
  failure("{}", "Expected");
  failure("AVS.function(args: {x: :int}, returns: :int) { |x| 'wrong' }.call(1)", "return type mismatch");
  failure("AVS.__function('[__garnet_token]i', 'i') {}", "Invalid or duplicate");
  failure("AVS.export(:Twice, 'i') {} ; AVS.export(:Twice, 'i') {}", "duplicate export");
  failure("AVS.export(:Twice, 'i') { raise 'callback failed' }; AVS.Twice(1)", "callback failed");
  // mruby lowers its call limit from 512 to 128 under ASan. Either it or
  // Garnet can reject recursion first; both must leave the session disabled.
  failure_one_of("AVS.export(:Twice, 'i') { |x| AVS.Twice(x) }; AVS.Twice(1)",
                 "Ruby callback nesting limit exceeded", "SystemStackError: stack level too deep");
  failure("AVS.export(:Twice, '[Width]i[width]i') {}", "duplicate");
  failure("AVS.filter(:Twice, args: {value: :unknown}) {}", "unknown AVS type");
  CHECK(argc == 2);
  {
    fake_host pipeline = {0};
    char path[4096];
    garnet_session* runner = create(&pipeline);
    snprintf(path, sizeof(path), "%s/pipeline-count.avs.rb", argv[1]);
    r = garnet_import(runner, &pipeline.context, str(path));
    CHECK(r.status == GARNET_OK && r.value.type == GARNET_CLIP);
    release_result(r);
    for (int i = 0; i < 2; ++i) {
      r = garnet_run_script(runner, &pipeline.context, str(path));
      CHECK(r.status == GARNET_OK && r.value.type == GARNET_CLIP);
      release_result(r);
    }
    r = eval(&pipeline, "$pipeline_runs");
    CHECK(r.status == GARNET_OK && r.value.as.integer == 3);
    release_result(r);
    snprintf(path, sizeof(path), "%s/pipeline-value.avs.rb", argv[1]);
    r = garnet_run_script(runner, &pipeline.context, str(path));
    CHECK(r.status != GARNET_OK && strstr(r.error.data, "must return a clip"));
    release_result(r);
    r = eval(&pipeline, "42");
    CHECK(r.status != GARNET_OK && strstr(r.error.data, "disabled"));
    release_result(r);
    garnet_destroy(runner);
    CHECK(pipeline.clips == 0);
  }
  {
    fake_host imports = {0};
    char path[4096];
    garnet_session* loader = create(&imports);
    snprintf(path, sizeof(path), "%s/main.avs.rb", argv[1]);
    r = garnet_import(loader, &imports.context, str(path));
    if (r.status != GARNET_OK)
      fprintf(stderr, "%.*s\n", (int)r.error.size, r.error.data);
    CHECK(r.status == GARNET_OK && r.value.as.integer == 42);
    release_result(r);
    r = garnet_import(loader, &imports.context, str(path));
    CHECK(r.status == GARNET_OK && r.value.as.integer == 42);
    release_result(r);
    r = garnet_evaluate(loader, &imports.context, str("require_relative 'main.avs.rb'"), str(path));
    CHECK(r.status == GARNET_OK && r.value.type == GARNET_BOOL && !r.value.as.integer);
    release_result(r);
    r = eval(&imports, "$garnet_late.call");
    CHECK(r.status == GARNET_OK && r.value.as.integer == 19);
    release_result(r);
    garnet_destroy(loader);
    loader = create(&imports);
    snprintf(path, sizeof(path), "%s/cycle.avs.rb", argv[1]);
    r = garnet_import(loader, &imports.context, str(path));
    CHECK(r.status != GARNET_OK && strstr(r.error.data, "Circular"));
    release_result(r);
    garnet_destroy(loader);
    loader = create(&imports);
    r = garnet_evaluate(loader, &imports.context, str("begin; require_relative 'broken'; rescue; end; 42"), str(path));
    CHECK(r.status != GARNET_OK && strstr(r.error.data, "failed nested import"));
    release_result(r);
    r = eval(&imports, "42");
    CHECK(r.status != GARNET_OK && strstr(r.error.data, "disabled"));
    release_result(r);
    garnet_destroy(loader);
  }
  puts("Garnet C host contract tests passed");
  return 0;
}
