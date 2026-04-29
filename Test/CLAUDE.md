# Test Project

## 概要

City プロジェクトの自動テスト用 Siv3D プロジェクト。
Claude Code が機能の検証を自律的に行うために使用する。

## 目的

本体 (City) の個別機能やレンダリング結果を、独立したプロセスで検証する。

- 特定の機能を単体で実行・描画する
- Logger でログを出力する
- スクリーンショットを保存して描画結果を目視・画像比較する
- テスト結果を Claude Code が読み取り、合否を判断して本体に反映する

## ワークフロー

1. **テストコードの記述** — `Main.cpp` に検証したい処理を記述する
2. **CRLF+BOM 復元** — ビルド前に必ず実行する（LF のままだと C4819/C2447 エラー）
   ```bash
   python3 chore/convert_line_endings.py to-crlf -d src Test
   ```
3. **ビルド**
   ```bash
   "/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Test/Test.vcxproj -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
   ```
4. **実行**（自動終了する）
   ```bash
   cd /mnt/d/Users/Takuma/Creations/codes/City/Test/App && "./Test(debug).exe"
   ```
5. **結果の検証** — スクリーンショット・ログを Claude Code が Read して確認する
6. **反映** — テスト結果に基づき本体コードを修正する

## 本体コードの参照

vcxproj の `IncludePath` に `$(ProjectDir)..`（プロジェクトルート）が含まれているため、本体のヘッダを直接参照できる:

```cpp
# include "src/ui/PanelManager.hpp"
# include "src/ui/PanelWidget.hpp"
# include "src/road/RoadTypes.hpp"
# include "src/asset/AssetRegistrar.hpp"
```

本体の `.cpp` をテストで使う場合は `Test.vcxproj` の `<ItemGroup>` に追加する:

```xml
<ClCompile Include="..\src\ui\PanelManager.cpp" />
```

**フォント等のアセットを使う場合**: Main() 冒頭で `RegisterAssets()` を呼ぶこと。

## ログ出力

`Logger <<` を使用する。出力先は Visual Studio の出力ウィンドウおよびログファイル。

```cpp
Logger << U"result = " << value;
```

ログファイルはワーキングディレクトリ (`Test/App/`) に生成される。

## スクリーンショット保存

`ScreenCapture` を使用する。保存は次の `System::Update()` 呼び出し時に実行される（1フレーム遅延）。

```cpp
ScreenCapture::SaveCurrentFrame(U"test_result.png");
```

**保存先**: `Test/App/Screenshot/` ディレクトリに保存される（Siv3D デフォルト動作）。
Claude Code は `Test/App/Screenshot/test_result.png` を Read して確認する。

典型的なパターン（描画して即終了）:

```cpp
void Main()
{
    Scene::SetBackground(ColorF{ 0.2, 0.2, 0.25 });
    RegisterAssets();

    int frame = 0;
    constexpr int kCaptureFrame = 3;  // フォント準備のため数フレーム待つ

    while (System::Update())
    {
        // ... 描画処理 ...

        if (frame == kCaptureFrame)
        {
            ScreenCapture::SaveCurrentFrame(U"test_result.png");
        }
        if (frame > kCaptureFrame)
        {
            break;  // キャプチャ保存後に終了
        }
        ++frame;
    }
}
```

**注意**: フレーム 0 でキャプチャするとフォントが未ロードで描画が不完全になる場合がある。2〜3 フレーム待ってからキャプチャすること。

## 方針

- テストは使い捨て。`Main.cpp` を書き換えて都度ビルド・実行する
- 本体の `src/` コードを可能な限り `#include` して再利用する。テスト専用にロジックを再実装しない
- テストは自動終了する。ユーザー操作を待たず、描画・保存・ログ出力が完了したら即座に終了する
- スクリーンショットは `Test/App/Screenshot/` に保存される
- ログ出力は `Logger <<` を使用する
