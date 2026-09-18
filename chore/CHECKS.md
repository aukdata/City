# 短い検証結果を返す共通コマンド

Windows の Python 3.9 以降（追加パッケージ不要）で実行する。作業ディレクトリはスクリプトが設定する。実行ログは `artifacts/local_runs/checks/<実行ID>/`、標準出力は短い JSON だけ。

```powershell
python chore/run_check.py build --target City
python chore/run_check.py build --target Test
python chore/run_check.py test --filter SaveTransaction
python chore/run_check.py playtest --mode streaming --seed 42
python chore/run_check.py playtest --mode navigation --seed 42
```

- `build` はビルドのみ。`test` / `playtest` は既存の実行ファイルを使うので、コード変更後は先に対象をビルドする。`test` のフィルター省略は全件。
- 既定は Release、`--configuration Debug` で切替。既定の制限時間は900秒、`--timeout 1800` 等で変更する。
- `playtest` は新規生成した街で自動終了する既存シナリオを実行する。streaming は720サンプルの飛行、navigation は6段階の視点変更。画像は会話へ送らない。起動条件は [AGENTS.md](../AGENTS.md) に従う。
- 既に City/Test が動いている場合、または別の共通コマンドが実行中なら失敗として返す。既存プロセスは終了しない。制限時間や中断時は、このコマンドで起動したプロセスツリーだけを終了する。

## 結果の扱い

`summary.json` は表示した結果、`command.json` は実行条件。`console.log` は標準出力と標準エラー。ビルドは全文の `build.log`、テストは元の `results.json`、実プレイは CSV・今回追記された `perf.log`・更新された `debug.log` を保存する。

終了コードは成功0、失敗1、引数不正2、制限時間124、中断130。子プロセスの終了コードも結果に残す。古い結果、テスト0件、壊れた集計、不完全なシナリオを合格扱いしない。ビルドの警告は件数と先頭5件を示す。テストの失敗は総数と先頭5件・各3理由までを示し、全文は保存した結果で調べる。

時間はミリ秒の平均・95パーセンタイル（nearest rank）・最大値。CPU時間とフレーム間隔は別項目。navigation の待機フレームも集計し、フェーズ別の CPU 時間を返す。成功は完走とデータ整合性を意味し、性能や景観の合格基準は依頼ごとに比較・判断する。

既存の通常プレイ記録も起動なしで要約できる。この結果は `summarized` で、新しい実行の成功を示さない。

```powershell
python chore/run_check.py summarize App/playtest_frames.csv
python chore/run_check.py summarize App/streaming_frames.csv --mode streaming
```

プロセスを強制終了した等で `.runner.lock` が残った場合は、記載された PID と City/Test が終了していることを確認してから、そのロックファイルだけを削除する。

要約処理・古い結果の拒否・失敗と制限時間の扱いは `python -m unittest discover -s chore -p test_run_check.py` で検証できる。
