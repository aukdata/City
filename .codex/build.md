# ビルド・実行

起動条件は [AGENTS.md](../AGENTS.md)。Visual Studio 2026 / MSVC v145、`SIV3D_0_6_16`、Windows Python 3.9 以降を使う。リポジトリ直下から次を実行する。

```powershell
python chore/run_check.py build --target City
python chore/run_check.py build --target Test
python chore/run_check.py test --filter SaveTransaction
python chore/run_check.py playtest --mode streaming --seed 42
```

- ビルド・テスト・自動終了するゲーム計測のログ全文は `artifacts/local_runs/checks/<実行ID>/` に保存され、標準出力には成否・件数・主要数値だけが返る。必要な場合だけ保存ログの該当箇所を読む。
- 既定は Release。`--configuration Debug` で切替。コード変更後は対象をビルドしてからテスト・計測する。ビルドだけの依頼ではソースを編集しない。
- コマンドの選び方、制限時間、既存プレイ記録の要約は [CHECKS.md](../chore/CHECKS.md)。Test の追加・描画検証は [Test/AGENTS.md](../Test/AGENTS.md)。
- 通常起動の出力は `App/City.exe`（Debug: `City(debug).exe`）。作業ディレクトリは `App/`、終了後はルートへ戻す。`--load <saveName>` / `--new` は `--seed` 等の前後どちらでも指定できる。開始対象を複数指定した場合は最初の指定を採用し、後続の描画・診断オプションも解析する。
- 手動のバックグラウンド起動は `Start-Process -WindowStyle Hidden`。括弧入りパスで失敗する `cmd.exe /c start` は使わない。実行中の本体を無断で終了しない。
