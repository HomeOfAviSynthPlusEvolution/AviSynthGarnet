# AviSynth — Garnet

**English** | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

AviSynth — Garnet is an embeddable Ruby scripting binding for AviSynth, developed alongside AviSynthMinus. It lets you write main scripts and reusable filter libraries in Ruby while continuing to use AviSynth's native filters, clips, and frame scheduling.

The standalone `Garnet.dll` loads through AviSynth+'s public C++ plugin interface and does not require changes to the core. The same scripting engine also exposes a private C host interface for embedding. Its implementation uses C++ and embeds a pinned mruby runtime; end users do not need to install Ruby.

Garnet is an experimental project for developer testing and feedback. It adds another way to write scripts, rather than replacing AviSynth's existing language or reimplementing its video filters.

## What you can write

- Top-level Ruby main scripts, without a mandatory class or entry-point function.
- Native filter chains such as `clip.BicubicResize(...)`, with positional and named arguments.
- Libraries using Ruby methods, modules, classes, arrays, hashes, presets, and closures.
- Explicitly exported filters that can be called from both `.avs` and Ruby scripts.
- Mixed AVS/Ruby pipeline stages and callable values passed between the two languages.
- Deferred Ruby callbacks for native runtime filters such as `ScriptClip`.

Garnet currently embeds mruby 4.0.0 with a selected set of extensions. It is not a full CRuby installation or a drop-in host for arbitrary RubyGems and native Ruby extensions. Ruby constructs the native filter graph; AviSynth and its plugins perform the underlying video processing.

## Using the DLL

The [CI workflow](https://github.com/HomeOfAviSynthPlusEvolution/AviSynthGarnet/actions/workflows/ci.yml) uploads Windows x64 builds after their respective tests pass:

- `Garnet-windows-x64-msvc`
- `Garnet-windows-x64-clang-cl`

Both provide `Garnet.dll`; choose one, not both. Match the DLL architecture to the AviSynth host. Put it in your plugin autoload directory, or load it explicitly before calling `ImportRuby`.

The adapter uses the public V11+ C++ SDK interface. Current Windows integration testing targets upstream AviSynth+ 3.7.6-dev (r4626); this is not a claim that every older runtime has been validated. A build from the current development baseline is the simplest starting point for trying Garnet.

Save the following as `main.avs.rb`:

```ruby
source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
source.BilinearResize(360, 240)
```

Open a small `main.avs` wrapper in your usual AviSynth application:

```avs
# Omit LoadPlugin if Garnet is already autoloaded.
LoadPlugin("C:/path/to/Garnet.dll")
ImportRuby("main.avs.rb")
```

`ImportRuby` returns the Ruby file's final expression. The `.avs.rb` suffix is a naming convention; applications do not need native support for that extension. External source filters and other plugins are still loaded through AviSynth in the usual way, and are not bundled with Garnet.

## Scripts, libraries, and imports

`AVS.FilterName(...)` invokes a native filter. `clip.FilterName(...)` supplies the receiver as its first clip argument. Use the AviSynth filter's name; Garnet does not automatically translate names to snake_case.

For a library, ordinary Ruby methods remain ordinary Ruby methods. Use `AVS.filter` to expose a callable AviSynth filter:

```ruby
AVS.filter :ExampleResize,
  args: {clip: :clip},
  options: {width: :int, height: :int} do |clip, width: 360, height: 240|
  clip.BilinearResize(width, height)
end
```

After importing that library, AVS can call `ExampleResize(clip, width=360, height=240)`, and Ruby can call `clip.ExampleResize(width: 360, height: 240)`. Required arguments follow schema order; optional arguments become Ruby keywords. Omitted optional values use the block's defaults. `false` and `0` are values, not missing arguments; a `nil` optional keyword is omitted.

Exported names live in AviSynth's function namespace, even when their implementations use Ruby modules or classes. Prefer a library-specific prefix. A library does not need to return a clip, and should register its functions during initialization rather than during frame callbacks.

Choose the loading operation according to the file's purpose:

| Operation | Purpose |
|---|---|
| `ImportRuby("file.avs.rb")` | Load a Ruby file once per environment and return its cached final expression. Suitable for libraries and a main script loaded once. |
| `require_relative 'lib/helper.avs.rb'` | Load a Ruby dependency relative to the calling Ruby file. Shares the `ImportRuby` load cache; returns whether the dependency was newly loaded. |
| `ImportScript(clip, "stage.avs.rb")` | Run an AVS or Ruby transformation on each call, with the input clip in `last`. The stage must return a clip. |
| `clip.import_relative('stage.avs')` | Ruby convenience form for a pipeline stage, resolving its path relative to the calling Ruby file. |

Inside a Ruby pipeline stage, `last` reads the input supplied by AviSynth. Ordinary Ruby expressions do not update it: assign intermediate clips or chain calls, and finish with the intended output expression. Pipeline imports scope AVS local variables and `last`, not explicit globals, registered filters, or all Ruby VM state.

## Examples

Start with [examples/resize.avs](examples/resize.avs), which includes loading instructions and a suggested reading order. Each entry point uses synthetic ColorBars and built-in filters, so no external video file or third-party filter library is required.

| Entry point | Structure and usage |
|---|---|
| [resize.avs](examples/resize.avs) | A top-level Ruby main script, native filter calls, and the final-expression result. |
| [library_from_avs.avs](examples/library_from_avs.avs) | A small Ruby library exported as an AVS filter. |
| [library_from_ruby.avs](examples/library_from_ruby.avs) | The same library used directly from Ruby, with its exports also visible to the outer AVS script. |
| [pipeline.avs](examples/pipeline.avs) | An AVS → Ruby → AVS transformation pipeline with scoped input clips. |
| [preset_filter.avs](examples/preset_filter.avs) | A class-based helper with presets, keyword overrides, validation, and readable settings output. |
| [function_values.avs](examples/function_values.avs) | Native and Ruby-created function values passed across the language boundary. |
| [frame_callback.avs](examples/frame_callback.avs) | A deferred callback selecting between prebuilt native graphs, including use with `Prefetch`. |

The examples are deliberately small, annotated usage patterns, not bundled copies of QTGMC, LSFmod, or other complete script libraries.

## Runtime considerations

Values crossing the AVS boundary are limited to undefined/nil, booleans, numbers, strings, arrays, clips, and explicit function values. Ruby collections have normal Ruby behavior within Ruby; crossing the AVS boundary does not create shared mutable Ruby arrays or expose arbitrary Ruby objects and hashes. Wrap a callback with `AVS.function` and an explicit schema rather than passing a plain Proc implicitly. Native function roundtrips preserve callability, but need not return the identical Ruby wrapper object.

Frame callbacks may execute after the main script returns, on worker threads, repeatedly, or out of frame order. Build reusable branches during initialization and base per-frame decisions on the request context, such as `AVS[:current_frame]`. Avoid shared incrementing counters and building new callback graphs on every frame. Ruby execution within a session is serialized, but native host calls release VM ownership: an entire callback is not an atomic operation across those calls.

The intended common lifetime is one script load, frame processing, then environment destruction. Some cycles involving Ruby closures and native graphs can remain alive until the environment closes. General cross-language cycle collection is not implemented; indefinitely reusing an environment while replacing callback graphs is not currently a bounded-memory guarantee.

## Building

Requirements:

- CMake 3.24 or later, Git, and a native C/C++ toolchain. Garnet uses C++17; the pinned mruby build also requires C++20 support.
- A build-time Ruby interpreter on `PATH` (CI uses Ruby 3.3), plus the selected build tool. The commands below use Ninja.
- For the plugin, an AviSynth+ SDK directory containing `avisynth.h`.

CMake fetches the pinned mruby 4.0.0 revision and manages its configuration and build. A separate mruby installation is not required. If regenerating mruby's parser sources, its build also needs Bison and gperf on `PATH`.

### Windows plugin

Run from an x64 Visual Studio developer shell with Ruby, CMake, and Ninja available:

```powershell
cmake -S . -B build/plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DGARNET_BUILD_PLUGIN=ON -DGARNET_BUILD_TESTS=ON -DGARNET_AVS_SDK_DIR=C:/src/AviSynthPlus/avs_core/include
cmake --build build/plugin --parallel 4
ctest --test-dir build/plugin --output-on-failure
```

The DLL is `build/plugin/Garnet.dll`. To use clang-cl, configure a separate build directory with `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` in the same kind of developer environment. The Windows public C++ plugin requires MSVC or clang-cl, not MinGW. CMake keeps the compiler, architecture, configuration, and CRT choices consistent with the embedded mruby build.

To include actual AviSynth loading and frame tests, reconfigure with an existing matching runtime:

```powershell
cmake -S . -B build/plugin -DGARNET_TEST_AVS_RUNTIME=C:/path/to/AviSynth.dll
cmake --build build/plugin --parallel 4
ctest --test-dir build/plugin --output-on-failure
```

These tests load the specified runtime in their own process and do not install or replace system plugins. Without `GARNET_TEST_AVS_RUNTIME`, the standalone engine tests still run.

### Standalone engine and embedding

The engine can be built without the AviSynth SDK or plugin:

```sh
cmake -S . -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DGARNET_BUILD_PLUGIN=OFF -DGARNET_BUILD_TESTS=ON
cmake --build build/engine --parallel 4
ctest --test-dir build/engine --output-on-failure
```

On Linux and macOS, the plugin can also be built by enabling `GARNET_BUILD_PLUGIN` and supplying the SDK directory. CI builds on Linux x64/ARM64 and macOS x64/ARM64 and runs standalone tests; the runtime integration test harness currently runs on Windows. Cross-compilation and a single universal macOS build are not currently supported.

For a local/offline mruby source checkout, set `GARNET_MRUBY_SOURCE_DIR` to a Git checkout at `831da26b9021de0369d17b71b5667e2941a1a32d`. CMake checks that revision. `GARNET_MRUBY_BUILD_DIR` is an advanced alternative for reusing a Garnet-built mruby configuration with its matching manifest, not an arbitrary system mruby library. Do not set both options.

A source-integrated host can use:

```cmake
add_subdirectory(third_party/garnet)
target_link_libraries(MyHost PRIVATE Garnet::Engine)
```

The embedding entry points are in [include/garnet/engine.h](include/garnet/engine.h), with the host callback contract in [include/garnet/host.h](include/garnet/host.h). This is a private, exact-revision C interface, not a stable binary SDK; build matching headers and libraries together. The host owns invocation contexts and native handles and must follow the shutdown and ownership contract. A C interface does not make native graph ownership or scheduling Garnet's responsibility. The AviSynthMinus internal host connection has not yet been integrated.

## Development and contributions

The maintainer directs development, reviews changes, and is responsible for releases. Bug reports, suggestions, and contributions are welcome. Discuss script-facing behavior, host-interface changes, and substantial architectural changes before implementation.

This project uses AI-assisted implementation, tests, and review. Contributions should explain the problem, approach, validation, and how AI was involved. Follow the repository's formatting conventions. For bug reports, include the Garnet commit or CI artifact, AviSynth version, host application, OS and architecture, relevant plugin versions, and a minimal reproducer.

## License

Garnet is licensed under the GNU General Public License, version 2 or later, with the AviSynth linking exception. See [LICENSE](LICENSE) for the full text. The exception retains its original scope, referring to the interfaces defined in `avisynth.h`; it is not extended to Garnet's private host C interface.

Third-party components retain their own licenses and copyright notices. The embedded mruby runtime is MIT-licensed; its notice is reproduced in [LICENSES/mruby.txt](LICENSES/mruby.txt).

## Acknowledgments

Thanks to AviSynth, AviSynth+, AviSynthMinus, and their contributors for the frameserver and plugin ecosystem, and to the [mruby](https://github.com/mruby/mruby) contributors for the embedded Ruby runtime. Thanks also to script authors and developers who help test the binding and refine its interface.

Thanks to [SB.SB](https://sb.sb) for sponsoring the LLM subscription used in this project's development.
