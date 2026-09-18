# Test 作業ガイド

[ルートの作業指示](../AGENTS.md)と[コーディング規約](../CODING_STYLE.md)に従う。本体の機能と描画を自動終了する独立プロセスで検証する。

## 実行と結果

[ビルド手順](../.codex/build.md) のコマンドで `-target:Test` を指定する。

- 作業ディレクトリ `Test/App/`、実行ファイル `Test.exe`（Debug: `Test(debug).exe`）。`--filter <名前の一部>` で関連ケースを選べる。引数なしで全件。
- 成功時は終了コード0、失敗時は1。`Test/App/TestResults/results.json` / `results.xml` で合否と失敗理由を確認する。
- テストケースは常設して `TestRunner` に登録し、`Main.cpp` を都度書き換えない。本体ロジックをテスト側に複製しない。
- 本体ヘッダは `#include "src/..."` で参照可能。本体 `.cpp` を使う場合は `Test.vcxproj` に追加する。
- フォント等は `RegisterAssets()` とアセットの作業ディレクトリを確認する。ログは `Logger` または個別ファイル、本体ロジックの計測は `DebugLog` を利用する。

## 描画検証

- スクリーンショットは `Test/App/Screenshot/` へ保存する。画像データを会話へ渡さない制約はルートと同じ。
- `ScreenCapture::SaveCurrentFrame()` は次の `System::Update()` で保存される。フォント準備を2〜3フレーム待ち、保存完了後に終了する。
- GPU readback と数値解析を使い、表示位置・色・裏面・重なり等を検証する。ユーザー操作待ちは入れない。
