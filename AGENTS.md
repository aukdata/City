# AI 作業ガイド

日本の街づくり・交通シミュレーション。目標は Cities: Skylines 風の遊びと、日本の街・山道・鉄道の自然な景観。C++ / Siv3D 0.6.16、Windows x64 専用。

## 優先順位とユーザー指定

- 新規生成した街のリアルさを優先する。既存セーブの互換性・移行対応には当分作業を割かない。
- 街路・家並み・農地・農道・用排水路の関係を実在の街と照らして改善する。`reference/` の資料も活用する。
- 道路は基本的に地上。勾配を合わせるために街全体を高架化しない。山道は道路種別の勾配・曲率制約と建設費の比較で経路を選び、必要なら九十九折りにする。
- 1フレーム程度の遅延は低優先度。再現しない異常終了は再現待ちとし、景観改善より優先しない。
- **画像データを会話に送らない**（2026-09-13 指定）。`view_image` 等の結果を画像として渡す確認も禁止。スクリーンショットはローカル保存し、ログ・GPU readback・画像の数値解析で検証する。報告はテキストと通常のファイルリンク。ユーザーが画像送信を明示的に再許可した場合のみ変更する。

## 作業の進め方

1. 作業ツリーの既存変更を確認し、今回と無関係な変更を保持する。
2. [仕様の目次](plan/SPEC_INDEX.md)、[現在の実装](plan/00_current_implementation.md)から該当仕様を読む。[既知の問題](ISSUE.md)も確認する。
3. バグ・性能問題は再現条件、座標・ID・状態、処理時間等を計測して原因を確かめる。「推測するな。計測せよ。」
4. [コーディング規約](CODING_STYLE.md)に従い、必要な変更を実装する。
5. 関連する常設テストとビルドを実行し、結果を記録する。新規 UI・レイアウト変更は **Test で表示を検証・レビューしてから本体へ反映**する。
6. 適切な粒度で`git commit`をする。コミットメッセージは、内容がわかる程度に簡素なものでよい。
7. 変更した仕様を更新する。`ISSUE.md` は未解決の問題のみ管理し、修正完了した項目を削除する。

- memory システムは使わない。継続的なルールはこのファイルに集約する。コード規約・仕様・検証結果はそれぞれの専用文書へ置く。
- コミットする場合は意味のある単位に分け、無関係な変更を混ぜない。
- コンパイラ警告は基本的にすべて解決する。将来使用予定のパラメータに `[[maybe_unused]]` を付け、使用時に除去する。

## ファイル編集

- `.editorconfig` に従い、タブ幅4、Doxygen コメントを使用する。
- `.cpp` / `.hpp` / `.h` は **UTF-8 BOM + CRLF**。新規作成時も BOM を付ける。LF に変換してから編集しない。
- PowerShell は `[IO.File]::ReadAllText()` / `WriteAllText()` と `[Text.UTF8Encoding]::new($true)` を使用し、CRLF を保持する。行単位の `Get-Content` / `Set-Content` による書き戻しを避ける。
- `convert_line_endings.py` は破損修復時に限り `-d src Test` で使用する。通常の編集前後には実行しない。

## ビルド・実行

要件: Visual Studio 2026 / MSVC v145、環境変数 `SIV3D_0_6_16`。
リポジトリ直下から PowerShell で実行する。

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' City.sln -target:City -p:Configuration=Release -p:Platform=x64 -p:PreferredToolArchitecture=x64 -m:1 -verbosity:minimal -noLogo
```

- `-target:Test` でテストをビルド。Debug は `-p:Configuration=Debug`。
- WSL では実行ファイルを `/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe` として同じ引数を渡す。
- 本体は `App/City.exe`（Debug: `City(debug).exe`）へ自動コピーされる。**実行時の作業ディレクトリは必ず `App/`**。
- 本体の起動はユーザーが明示的に依頼した場合に行う。「実際に遊んで確認して」という依頼は、その作業に必要な起動を含む。依頼がなければ起動しない。Test は自律実行できる。
- `--load <saveName>` で直接ロード、`--new` で新規生成。`--seed <数値>` 等を使う場合、`--load` / `--new` は最後に置く。
- `cmd.exe /c start` は括弧入りパスで失敗するため使わない。PowerShell の `Start-Process` でバックグラウンド起動する場合は `-WindowStyle Hidden` を指定する。
- 実行後はプロジェクトルートを作業ディレクトリに戻す。テスト手順・出力先は [Test/AGENTS.md](Test/AGENTS.md)。

## デバッグ・SDK 調査

- 本体は原則 `DebugLog::print(U"...")` / `DBG_LOG(U"...")` を使用する。`GameApp::run()` の initialize/shutdown を維持し、状態・ID・座標・時間・失敗理由を `App/debug.log` とオンスクリーンログへ出す。
- ユーザーが「Console に出して」と指定した場合は `Console <<`。`Print` は明示的に指定された場合のみ。
- Siv3D API に迷ったら、まず [SIV3D_NOTES.md](plan/SIV3D_NOTES.md) を検索する。追加調査は `siv3d-api` Agent 経由で SDK ヘッダを読む。既存 API を優先し、自前で再実装しない。
- SDK ヘッダ: `D:/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`（WSL: `/mnt/d/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`）。
- 作業別ガイドは `.codex/`、Claude 用 Agent 定義は `.claude/agents/`。必要なものだけ参照する。ルールの正本はこのファイルとする。
