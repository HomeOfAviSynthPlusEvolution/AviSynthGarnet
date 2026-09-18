# AviSynth — Garnet

[English](README.md) | [简体中文](README.zh-CN.md) | **日本語**

AviSynth — Garnet は、AviSynthMinus とともに開発されている、組み込み可能な AviSynth 用 Ruby スクリプトバインディングです。AviSynth のネイティブフィルター、クリップ、フレームスケジューリングをそのまま利用しながら、メインスクリプトや再利用可能なフィルターライブラリを Ruby で記述できます。

単体の `Garnet.dll` は AviSynth+ の公開 C++ プラグインインターフェースで読み込めるため、コアの変更は不要です。同じスクリプトエンジンには、ホストへの組み込み用の非公開 C インターフェースもあります。実装には C++ を使用し、固定バージョンの mruby ランタイムを内蔵しています。利用者が Ruby をインストールする必要はありません。

Garnet は現在、開発者による試用とフィードバックを目的とした実験的なプロジェクトです。既存の AviSynth スクリプト言語を置き換えたり、映像フィルターを再実装したりするものではなく、スクリプトを記述する別の方法を提供します。

## できること

- クラスやエントリーポイント関数を必須としない、トップレベルの Ruby メインスクリプト。
- 位置引数と名前付き引数を使う `clip.BicubicResize(...)` のようなネイティブフィルターチェーン。
- Ruby のメソッド、モジュール、クラス、配列、ハッシュ、プリセット、クロージャーを利用したライブラリ。
- `.avs` と Ruby の両方から呼び出せる、明示的に公開したフィルター。
- AVS と Ruby を組み合わせたパイプラインと、両言語間での関数値の受け渡し。
- `ScriptClip` などのネイティブ実行時フィルターで使用する、遅延実行の Ruby コールバック。

現在は mruby 4.0.0 と選択した拡張を組み込んでいます。完全な CRuby 環境ではなく、任意の RubyGems やネイティブ Ruby 拡張をそのまま実行するためのホストでもありません。Ruby がネイティブのフィルターグラフを構築し、実際の映像処理は AviSynth とそのプラグインが担当します。

## DLL の利用

[CI ワークフロー](https://github.com/HomeOfAviSynthPlusEvolution/AviSynthGarnet/actions/workflows/ci.yml) は、それぞれのテストが成功した後に Windows x64 のビルド成果物をアップロードします。

- `Garnet-windows-x64-msvc`
- `Garnet-windows-x64-clang-cl`

どちらにも `Garnet.dll` が含まれます。片方を選び、両方を同時に読み込まないでください。DLL のアーキテクチャは AviSynth ホストと合わせてください。プラグインの自動読み込みディレクトリに配置するか、`ImportRuby` を呼び出す前に明示的に読み込みます。

アダプターは公開 V11+ C++ SDK インターフェースを使用します。現在の Windows 統合テストは上流の AviSynth+ 3.7.6-dev（r4626）を対象としており、すべての古いランタイムを検証済みという意味ではありません。初めて試す場合は、この開発ベースラインのビルドを使うのが簡単です。

次の内容を `main.avs.rb` として保存します。

```ruby
source = AVS.ColorBars(width: 720, height: 480, pixel_type: 'YV12')
source.BilinearResize(360, 240)
```

通常の AviSynth アプリケーションでは、短い `main.avs` ラッパーを開きます。

```avs
# Garnet が自動読み込みされる場合、LoadPlugin は省略できます。
LoadPlugin("C:/path/to/Garnet.dll")
ImportRuby("main.avs.rb")
```

`ImportRuby` は Ruby ファイルの最後の式の値を返します。`.avs.rb` は命名上の慣例であり、アプリケーションがこの拡張子を直接認識する必要はありません。外部のソースフィルターやその他のプラグインは通常どおり AviSynth から読み込み、Garnet には同梱されません。

## スクリプト、ライブラリ、インポート

`AVS.FilterName(...)` はネイティブフィルターを呼び出します。`clip.FilterName(...)` はレシーバーを最初のクリップ引数として渡します。AviSynth のフィルター名を使用してください。Garnet は名前を自動的に snake_case へ変換しません。

ライブラリ内の通常の Ruby メソッドは、そのままでは Ruby のメソッドです。AviSynth から呼び出せるフィルターとして公開するには、`AVS.filter` を使用します。

```ruby
AVS.filter :ExampleResize,
  args: {clip: :clip},
  options: {width: :int, height: :int} do |clip, width: 360, height: 240|
  clip.BilinearResize(width, height)
end
```

このライブラリをインポートすると、AVS では `ExampleResize(clip, width=360, height=240)`、Ruby では `clip.ExampleResize(width: 360, height: 240)` と呼び出せます。必須引数は宣言順に渡され、省略可能な引数は Ruby のキーワード引数になります。省略した引数にはブロックのデフォルト値を使用します。`false` と `0` は値であり、引数の省略を意味しません。値が `nil` の省略可能なキーワード引数は省略されます。

実装に Ruby のモジュールやクラスを使用していても、公開名は AviSynth の関数名前空間に登録されます。ライブラリ固有の接頭辞を推奨します。ライブラリがクリップを返す必要はありません。関数の登録は初期化時に行い、フレームコールバック内では行わないでください。

ファイルの目的に応じて、読み込み方法を選びます。

| 操作 | 用途 |
|---|---|
| `ImportRuby("file.avs.rb")` | 環境ごとに Ruby ファイルを一度だけ読み込み、最後の式のキャッシュ済みの値を返します。ライブラリや、一度だけ読み込むメインスクリプトに適しています。 |
| `require_relative 'lib/helper.avs.rb'` | 呼び出し元の Ruby ファイルを基準に依存ファイルを読み込みます。`ImportRuby` と読み込みキャッシュを共有し、今回新たに読み込んだかどうかを返します。 |
| `ImportScript(clip, "stage.avs.rb")` | 呼び出すたびに AVS または Ruby の変換処理を実行し、入力クリップを `last` に設定します。その処理はクリップを返す必要があります。 |
| `clip.import_relative('stage.avs')` | Ruby からパイプラインの処理段階を呼び出すための簡便な形式です。パスは呼び出し元の Ruby ファイルを基準に解決します。 |

Ruby のパイプライン処理内では、`last` は AviSynth が渡した入力を読み取ります。通常の Ruby 式は `last` を更新しません。中間クリップを変数に代入するか呼び出しを連結し、意図した出力を最後の式にしてください。パイプラインのインポートは AVS のローカル変数と `last` をスコープで分離しますが、明示的なグローバル変数、登録済みフィルター、Ruby VM 全体の状態を分離するものではありません。

## サンプル

読み込み方法と推奨する読む順序を記載した [examples/resize.avs](examples/resize.avs) から始めてください。すべてのエントリーポイントは合成映像の ColorBars と組み込みフィルターを使用するため、外部の映像ファイルやサードパーティーのフィルターライブラリは不要です。

| エントリーポイント | 典型的な構成と使い方 |
|---|---|
| [resize.avs](examples/resize.avs) | トップレベルの Ruby メインスクリプト、ネイティブフィルター呼び出し、最後の式による戻り値。 |
| [library_from_avs.avs](examples/library_from_avs.avs) | 小さな Ruby ライブラリを AVS フィルターとして公開。 |
| [library_from_ruby.avs](examples/library_from_ruby.avs) | 同じライブラリを Ruby から直接利用し、公開した関数を外側の AVS スクリプトからも参照。 |
| [pipeline.avs](examples/pipeline.avs) | AVS → Ruby → AVS の変換パイプラインと、各段階の入力クリップのスコープ。 |
| [preset_filter.avs](examples/preset_filter.avs) | プリセット、キーワードによる上書き、入力検証、読みやすい設定表示を備えたクラスベースのヘルパー。 |
| [function_values.avs](examples/function_values.avs) | ネイティブ関数と Ruby で作成した関数値を言語境界の両側で受け渡し。 |
| [frame_callback.avs](examples/frame_callback.avs) | 構築済みのネイティブグラフを遅延実行のコールバックで選択し、`Prefetch` と併用。 |

これらは説明コメント付きの小さな使用例です。QTGMC、LSFmod、その他の完全なスクリプトライブラリを同梱したものではありません。

## 実行時の注意点

AVS 境界を越えて渡せる値は、undefined/nil、真偽値、数値、文字列、配列、クリップ、明示的な関数値に限られます。Ruby 内のコレクションは通常の Ruby の動作に従いますが、AVS 境界を越えて共有・変更可能な Ruby 配列を作ったり、任意の Ruby オブジェクトやハッシュを直接公開したりすることはできません。コールバックは明示的な引数宣言と `AVS.function` で包み、通常の Proc が暗黙に変換されるとは考えないでください。ネイティブ側との往復後も関数を呼び出せますが、同一の Ruby ラッパーオブジェクトが返る保証はありません。

フレームコールバックは、メインスクリプトが戻った後、ワーカースレッド上で、繰り返し、またはフレーム順とは異なる順序で実行される場合があります。再利用する分岐は初期化時に構築し、`AVS[:current_frame]` などの要求コンテキストに基づいて判断してください。共有のインクリメントカウンターや、フレームごとのコールバックグラフの再構築は避けてください。セッション内の Ruby 実行は直列化されますが、ネイティブホスト呼び出し中は VM の実行権を解放します。そのため、これらの呼び出しをまたぐコールバック全体がアトミックな操作になるわけではありません。

主な想定ライフサイクルは、スクリプトを一度読み込み、フレーム処理を完了し、環境を破棄するというものです。Ruby クロージャーとネイティブグラフの間の循環参照の一部は、環境を閉じるまで残る場合があります。汎用的な言語間の循環参照回収は未実装です。環境を無期限に再利用しながらコールバックグラフを入れ替え続ける場合、現在はメモリ使用量が一定の範囲に収まる保証はありません。

## ビルド

必要な環境：

- CMake 3.24 以降、Git、ネイティブビルド用の C/C++ ツールチェーン。Garnet は C++17 を使用します。固定バージョンの mruby のビルドには C++20 のサポートも必要です。
- `PATH` 上のビルド用 Ruby インタープリター（CI では Ruby 3.3）と、選択したビルドツール。以下のコマンドでは Ninja を使用します。
- プラグインをビルドする場合は、`avisynth.h` を含む AviSynth+ SDK ディレクトリ。

CMake が固定リビジョンの mruby 4.0.0 ソースを取得し、その設定とビルドを管理します。mruby を別途インストールする必要はありません。mruby のパーサーソースを再生成する場合は、`PATH` 上に Bison と gperf も必要です。

### Windows プラグイン

Ruby、CMake、Ninja を利用できる x64 Visual Studio 開発者シェルで実行します。

```powershell
cmake -S . -B build/plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DGARNET_BUILD_PLUGIN=ON -DGARNET_BUILD_TESTS=ON -DGARNET_AVS_SDK_DIR=C:/src/AviSynthPlus/avs_core/include
cmake --build build/plugin --parallel 4
ctest --test-dir build/plugin --output-on-failure
```

DLL は `build/plugin/Garnet.dll` に生成されます。clang-cl を使う場合は、同様の開発者環境で別のビルドディレクトリを用意し、`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` を設定してください。Windows の公開 C++ プラグインには MSVC または clang-cl が必要で、MinGW は対応していません。CMake は、コンパイラー、アーキテクチャ、ビルド構成、CRT の選択を組み込み mruby のビルドと揃えます。

実際の AviSynth の読み込みとフレームテストを含めるには、適合する既存のランタイムを指定して再構成します。

```powershell
cmake -S . -B build/plugin -DGARNET_TEST_AVS_RUNTIME=C:/path/to/AviSynth.dll
cmake --build build/plugin --parallel 4
ctest --test-dir build/plugin --output-on-failure
```

テストは独自のプロセスで指定したランタイムを読み込み、システムのプラグインをインストールしたり置き換えたりしません。`GARNET_TEST_AVS_RUNTIME` を指定しない場合も、単体エンジンのテストは実行されます。

### 単体エンジンと組み込み

エンジンは AviSynth SDK やプラグインなしでビルドできます。

```sh
cmake -S . -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DGARNET_BUILD_PLUGIN=OFF -DGARNET_BUILD_TESTS=ON
cmake --build build/engine --parallel 4
ctest --test-dir build/engine --output-on-failure
```

Linux と macOS でも、`GARNET_BUILD_PLUGIN` を有効にして SDK ディレクトリを指定するとプラグインをビルドできます。CI は Linux x64/ARM64 と macOS x64/ARM64 でビルドと単体テストを実行します。ランタイム統合テスト用のハーネスは現在 Windows で動作します。クロスコンパイルと、一度のビルドでの macOS ユニバーサルバイナリー生成には現在対応していません。

ローカルまたはオフラインの mruby ソースを使う場合は、`GARNET_MRUBY_SOURCE_DIR` に `831da26b9021de0369d17b71b5667e2941a1a32d` をチェックアウトした Git 作業ディレクトリを指定します。CMake はそのリビジョンを検査します。`GARNET_MRUBY_BUILD_DIR` は、Garnet がビルドした mruby を対応するマニフェストとともに再利用するための高度な代替オプションです。任意のシステム mruby ライブラリを指定するものではありません。両方を同時に設定しないでください。

ソースとともに組み込むホストでは、次のように使用できます。

```cmake
add_subdirectory(third_party/garnet)
target_link_libraries(MyHost PRIVATE Garnet::Engine)
```

組み込み用のエントリーポイントは [include/garnet/engine.h](include/garnet/engine.h)、ホストのコールバック契約は [include/garnet/host.h](include/garnet/host.h) にあります。これはリビジョンの完全一致を必要とする非公開 C インターフェースであり、安定したバイナリー SDK ではありません。対応するヘッダーとライブラリを一緒にビルドしてください。呼び出しコンテキストとネイティブハンドルはホストが所有し、終了処理と所有権の契約を守る必要があります。C インターフェースを提供しても、ネイティブグラフの所有権やスケジューリングが Garnet の責任になるわけではありません。AviSynthMinus の内部ホストへの接続はまだ統合されていません。

## 開発と貢献

メンテナーが技術的な方向性、変更のレビュー、リリースに責任を持ちます。不具合報告、提案、貢献を歓迎します。スクリプトから見える動作、ホストインターフェース、大きなアーキテクチャの変更については、実装前に目的と方針を相談してください。

本プロジェクトは、実装、テスト作成、コードレビューに AI を活用しています。貢献時には、問題、実装方針、検証方法、AI の関与について説明し、リポジトリのフォーマット規約に従ってください。不具合報告には、Garnet のコミットまたは CI 成果物、AviSynth のバージョン、ホストアプリケーション、OS とアーキテクチャ、関連プラグインのバージョン、最小の再現例を含めてください。

## ライセンス

Garnet は GNU General Public License バージョン 2 以降（GPL-2.0-or-later）に AviSynth のリンク例外を付加した条件で公開されます。全文は [LICENSE](LICENSE) を参照してください。例外は `avisynth.h` に定義されたインターフェースを対象とする元の範囲を維持し、Garnet の非公開ホスト C インターフェースには拡張されません。

サードパーティーのコンポーネントは、それぞれのライセンスと著作権表示を維持します。組み込みの mruby ランタイムは MIT ライセンスです。原文は [LICENSES/mruby.txt](LICENSES/mruby.txt) に収録しています。

## 謝辞

フレームサーバーとプラグインのエコシステムを提供する AviSynth、AviSynth+、AviSynthMinus とその貢献者、および組み込み Ruby ランタイムを提供する [mruby](https://github.com/mruby/mruby) の貢献者に感謝します。また、バインディングの試用とインターフェースの改善に協力してくださるスクリプト作者と開発者にも感謝します。

本プロジェクトの開発に使用する LLM サブスクリプションを支援してくださる [SB.SB](https://sb.sb) に感謝します。
