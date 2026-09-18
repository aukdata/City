# ビルド・実行

ビルド、コンパイル診断、本体起動が必要な場合に参照する。起動の可否は [AGENTS.md](../AGENTS.md) に従う。

## ビルド

Visual Studio 2026 / MSVC v145、環境変数 `SIV3D_0_6_16` が必要。リポジトリ直下で実行する。

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' City.sln -target:City -p:Configuration=Release -p:Platform=x64 -p:PreferredToolArchitecture=x64 -m:1 -verbosity:minimal -noLogo
```

- テストは `-target:Test`、Debug は `-p:Configuration=Debug`。テスト実行と結果は [Test/AGENTS.md](../Test/AGENTS.md)。
- WSL の実行ファイルは `/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe`。引数は同じ。
- ビルドのみの依頼ではソースを編集しない。失敗時はエラー箇所・理由を調べ、実装修正まで依頼されていればその範囲で修正する。
- ログ全文はローカルに保存し、終了コード・エラー・警告・出力先を簡潔に報告する。LNK1168 も失敗として扱い、実行中の本体を無断で終了しない。

## 本体の起動

- 出力は `App/City.exe`（Debug: `City(debug).exe`）。実行時の作業ディレクトリは必ず `App/`、実行後はルートへ戻す。
- `--load <saveName>` でロード、`--new` で新規生成。`--seed <数値>` 等のオプションより後、末尾に `--load` / `--new` を置く。
- バックグラウンド起動は PowerShell `Start-Process -WindowStyle Hidden`。括弧入りパスで失敗する `cmd.exe /c start` は使わない。
