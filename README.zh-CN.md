# AviSynth — Garnet

[English](README.md) | **简体中文** | [日本語](README.ja.md)

AviSynth — Garnet 是与 AviSynthMinus 协同开发的可嵌入式 Ruby 脚本绑定。它让你使用 Ruby 编写主脚本和可复用的滤镜库，同时继续使用 AviSynth 原生的滤镜、clip 和帧调度机制。

独立的 `Garnet.dll` 通过 AviSynth+ 的公开 C++ 插件接口加载，无需修改核心。同一套脚本引擎也提供私有 C host 接口，供其他宿主嵌入。内部实现使用 C++，并内嵌固定版本的 mruby 运行时；最终用户无需安装 Ruby。

Garnet 目前处于面向开发者试用和反馈的实验阶段。它提供另一种脚本编写方式，不替换 AviSynth 现有的脚本语言，也不重新实现视频滤镜。

## 可以用它做什么

- 直接编写 Ruby 顶层主脚本，不强制定义 class 或入口函数。
- 使用 `clip.BicubicResize(...)` 这样的原生滤镜链，支持位置参数和命名参数。
- 使用 Ruby 方法、模块、类、数组、哈希、预设和闭包组织工具库。
- 显式导出滤镜，让 `.avs` 和 Ruby 脚本使用同一个库。
- 混合使用 AVS/Ruby 管线阶段，并在两种语言之间传递函数值。
- 为 `ScriptClip` 等原生运行时滤镜提供延迟执行的 Ruby 回调。

Garnet 当前内嵌 mruby 4.0.0 和经过选择的扩展，并非完整的 CRuby 环境，也不能直接运行任意 RubyGems 或原生 Ruby 扩展。Ruby 负责构建原生滤镜图，底层视频处理仍由 AviSynth 及其插件完成。

## 使用 DLL

[CI 工作流](https://github.com/HomeOfAviSynthPlusEvolution/AviSynthGarnet/actions/workflows/ci.yml) 会在对应测试通过后上传 Windows x64 构建产物：

- `Garnet-windows-x64-msvc`
- `Garnet-windows-x64-clang-cl`

两者均提供 `Garnet.dll`，选择其中一个即可，不要同时加载。DLL 架构须与 AviSynth 宿主一致。可以将其放入插件自动加载目录，也可以在调用 `ImportRuby` 前显式加载。

适配层使用公开的 V11+ C++ SDK 接口。目前 Windows 集成测试以上游 AviSynth+ 3.7.6-dev（r4626）为基线，并不表示所有较旧运行时均已验证。首次试用时，建议从这一开发基线的构建开始。

将以下内容保存为 `main.avs.rb`：

```ruby
source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
source.BilinearResize(360, 240)
```

再用常用的 AviSynth 应用打开一个简短的 `main.avs` 包装脚本：

```avs
# 如果 Garnet 已经自动加载，可以省略 LoadPlugin。
LoadPlugin("C:/path/to/Garnet.dll")
ImportRuby("main.avs.rb")
```

`ImportRuby` 返回 Ruby 文件最后一个表达式的值。`.avs.rb` 只是命名惯例，不要求应用原生识别这一扩展名。外部源滤镜和其他插件仍按 AviSynth 的通常方式加载，不随 Garnet 一起提供。

## 脚本、库与导入

`AVS.FilterName(...)` 调用原生滤镜；`clip.FilterName(...)` 将接收者作为第一个 clip 参数。使用 AviSynth 原有的滤镜名称，Garnet 不会自动把名称转换为 snake_case。

库中的普通 Ruby 方法仍然只是 Ruby 方法。需要将其作为 AviSynth 滤镜使用时，通过 `AVS.filter` 显式导出：

```ruby
AVS.filter :ExampleResize,
  args: {clip: :clip},
  options: {width: :int, height: :int} do |clip, width: 360, height: 240|
  clip.BilinearResize(width, height)
end
```

导入这个库后，AVS 可以调用 `ExampleResize(clip, width=360, height=240)`，Ruby 可以调用 `clip.ExampleResize(width: 360, height: 240)`。必选参数按声明顺序传入，可选参数转换为 Ruby 关键字参数。省略的可选参数使用代码块中的默认值。`false` 和 `0` 是有效值，不表示缺省；值为 `nil` 的可选关键字参数会被省略。

即使实现位于 Ruby 模块或类中，导出名称仍属于 AviSynth 的函数命名空间。建议使用库专属的名称前缀。库不必返回 clip，函数注册应在初始化时完成，不应放在逐帧回调中。

根据文件用途选择加载方式：

| 操作 | 用途 |
|---|---|
| `ImportRuby("file.avs.rb")` | 每个环境只加载一次 Ruby 文件，并返回缓存的最后一个表达式的值。适用于库和只加载一次的主脚本。 |
| `require_relative 'lib/helper.avs.rb'` | 相对于调用它的 Ruby 文件加载依赖。与 `ImportRuby` 共享加载缓存，返回本次是否首次加载该依赖。 |
| `ImportScript(clip, "stage.avs.rb")` | 每次调用都执行 AVS 或 Ruby 变换，将输入 clip 放入 `last`。该阶段必须返回 clip。 |
| `clip.import_relative('stage.avs')` | Ruby 中调用管线阶段的便捷形式，路径相对于调用它的 Ruby 文件解析。 |

在 Ruby 管线阶段中，`last` 读取 AviSynth 提供的输入。普通 Ruby 表达式不会更新它：应将中间 clip 赋给变量或使用链式调用，并以预期输出作为最后一个表达式。管线导入会隔离 AVS 局部变量和 `last`，不会隔离显式全局变量、已注册滤镜或整个 Ruby VM 的状态。

## 样例

从 [examples/resize.avs](examples/resize.avs) 开始，其中包含加载说明和建议阅读顺序。所有入口均使用合成的 ColorBars 和内置滤镜，无需外部视频文件或第三方滤镜库。

| 入口 | 典型结构与用法 |
|---|---|
| [resize.avs](examples/resize.avs) | Ruby 顶层主脚本、原生滤镜调用，以及最后一个表达式作为返回值。 |
| [library_from_avs.avs](examples/library_from_avs.avs) | 将轻量 Ruby 库导出为 AVS 滤镜。 |
| [library_from_ruby.avs](examples/library_from_ruby.avs) | 在 Ruby 中直接使用同一个库，导出函数也对外层 AVS 脚本可见。 |
| [pipeline.avs](examples/pipeline.avs) | AVS → Ruby → AVS 混合变换管线，以及各阶段的输入 clip 作用域。 |
| [preset_filter.avs](examples/preset_filter.avs) | 使用类组织工具，处理预设、关键字覆盖、参数校验和可读的设置输出。 |
| [function_values.avs](examples/function_values.avs) | 在语言边界两侧传递原生函数和 Ruby 创建的函数值。 |
| [frame_callback.avs](examples/frame_callback.avs) | 在预先构建的原生图之间进行延迟选择，并与 `Prefetch` 配合使用。 |

这些是带注释的小型用法示例，不是打包进来的 QTGMC、LSFmod 或其他完整脚本库。

## 运行时注意事项

跨越 AVS 边界的值限于 undefined/nil、布尔值、数值、字符串、数组、clip 和显式函数值。Ruby 集合在 Ruby 内部仍遵循通常的 Ruby 语义；跨越 AVS 边界不会建立可共享修改的 Ruby 数组，也不会直接暴露任意 Ruby 对象或哈希。回调应使用 `AVS.function` 和明确的参数声明包装，普通 Proc 不会被隐式转换。原生函数往返后仍可调用，但不保证得到原来的同一个 Ruby 包装对象。

逐帧回调可能在主脚本返回后执行，也可能在工作线程上重复或乱序执行。应在初始化时构建可复用分支，按 `AVS[:current_frame]` 等请求上下文决定当前帧的处理方式。避免共享自增计数器，也避免每帧重新构建带回调的视频图。单个会话内的 Ruby 执行是串行的，但调用原生宿主时会让出 VM 执行权，因此整个回调跨越这些调用时并不具备原子性。

主要面向的生命周期是：加载一次脚本，完成取帧处理，然后销毁环境。Ruby 闭包与原生图之间的部分循环引用可能保留到环境关闭。通用的跨语言循环回收尚未实现；如果无限期复用一个环境并反复替换回调图，目前不能保证内存始终有界。

## 构建

所需环境：

- CMake 3.24 或更新版本、Git，以及本机构建用的 C/C++ 工具链。Garnet 使用 C++17；固定版本的 mruby 构建还需要 C++20 支持。
- `PATH` 中可用的构建期 Ruby 解释器（CI 使用 Ruby 3.3），以及所选构建工具。下方命令使用 Ninja。
- 构建插件时，还需要包含 `avisynth.h` 的 AviSynth+ SDK 目录。

CMake 会获取固定版本的 mruby 4.0.0 源码，并管理其配置和构建，无需另行安装 mruby。如果需要重新生成 mruby 的解析器源码，还须在 `PATH` 中提供 Bison 和 gperf。

### Windows 插件

在可使用 Ruby、CMake 和 Ninja 的 x64 Visual Studio 开发者命令行中运行：

```powershell
cmake -S . -B build/plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DGARNET_BUILD_PLUGIN=ON -DGARNET_BUILD_TESTS=ON -DGARNET_AVS_SDK_DIR=C:/src/AviSynthPlus/avs_core/include
cmake --build build/plugin --parallel 4
ctest --test-dir build/plugin --output-on-failure
```

产物为 `build/plugin/Garnet.dll`。使用 clang-cl 时，在同类开发者环境中选择独立构建目录，并设置 `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`。Windows 公开 C++ 插件需要 MSVC 或 clang-cl，不支持 MinGW。CMake 会使内嵌 mruby 的编译器、架构、构建配置和 CRT 选择与 Garnet 保持一致。

要加入实际的 AviSynth 加载和取帧测试，可重新配置并指定现有的匹配运行时：

```powershell
cmake -S . -B build/plugin -DGARNET_TEST_AVS_RUNTIME=C:/path/to/AviSynth.dll
cmake --build build/plugin --parallel 4
ctest --test-dir build/plugin --output-on-failure
```

测试在自己的进程中加载指定运行时，不安装或替换系统插件。未设置 `GARNET_TEST_AVS_RUNTIME` 时，仍会运行独立引擎测试。

### 独立引擎与嵌入

引擎可以在没有 AviSynth SDK、也不构建插件的情况下单独构建：

```sh
cmake -S . -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DGARNET_BUILD_PLUGIN=OFF -DGARNET_BUILD_TESTS=ON
cmake --build build/engine --parallel 4
ctest --test-dir build/engine --output-on-failure
```

Linux 和 macOS 也可以通过启用 `GARNET_BUILD_PLUGIN` 并指定 SDK 目录来构建插件。CI 在 Linux x64/ARM64 和 macOS x64/ARM64 上构建并运行独立测试；运行时集成测试工具目前仅在 Windows 上运行。当前不支持交叉编译，也不支持在单次构建中生成 macOS 通用二进制。

使用本地或离线 mruby 源码时，将 `GARNET_MRUBY_SOURCE_DIR` 指向检出到 `831da26b9021de0369d17b71b5667e2941a1a32d` 的 Git 工作目录，CMake 会检查该修订。`GARNET_MRUBY_BUILD_DIR` 是另一种高级用法，用于复用带有匹配清单的 Garnet 构建版 mruby，并不接受任意系统 mruby 库。两个选项不能同时设置。

源码集成的宿主可以使用：

```cmake
add_subdirectory(third_party/garnet)
target_link_libraries(MyHost PRIVATE Garnet::Engine)
```

嵌入入口见 [include/garnet/engine.h](include/garnet/engine.h)，宿主回调契约见 [include/garnet/host.h](include/garnet/host.h)。这是要求修订精确匹配的私有 C 接口，不是稳定的二进制 SDK；应将匹配的头文件与库一起构建。宿主持有调用上下文和原生句柄，并须遵守关闭与所有权契约。提供 C 接口并不意味着原生图的所有权和调度由 Garnet 接管。目前尚未接入 AviSynthMinus 的内部宿主。

## 开发与贡献

维护者负责技术方向、变更审核和最终发布。欢迎问题报告、建议和贡献。涉及脚本可见行为、宿主接口或重大架构调整时，请先讨论目标和方案。

本项目使用 AI 辅助实现、测试编写和代码审查。贡献应说明问题、实现思路、验证方式，以及 AI 的参与方式，并遵循仓库的代码格式约定。报告问题时，请附上 Garnet 提交或 CI 构建产物、AviSynth 版本、宿主应用、操作系统与架构、相关插件版本，以及最小复现。

## 许可证

Garnet 使用 GNU 通用公共许可证第 2 版或更新版本（GPL-2.0-or-later），附带 AviSynth 链接例外。完整条款见 [LICENSE](LICENSE)。例外保留原有范围，仅涉及 `avisynth.h` 中定义的接口，不扩展到 Garnet 的私有宿主 C 接口。

第三方组件保留各自的许可证和版权声明。内嵌的 mruby 运行时使用 MIT 许可证，原文见 [LICENSES/mruby.txt](LICENSES/mruby.txt)。

## 致谢

感谢 AviSynth、AviSynth+、AviSynthMinus 及其贡献者提供的帧服务器和插件生态，感谢 [mruby](https://github.com/mruby/mruby) 贡献者提供的嵌入式 Ruby 运行时。也感谢帮助试用绑定、改进接口的脚本作者和开发者。

感谢 [烧饼论坛](https://sb.sb) 赞助本项目开发所使用的 LLM 订阅。
