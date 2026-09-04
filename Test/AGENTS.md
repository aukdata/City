# Test Project

Cityの自動回帰テスト用Siv3Dプロジェクト。ルートの`AGENTS.md`と`CODING_STYLE.md`を優先する。

## 実行

```bash
"/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Test/Test.vcxproj -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo
cd /mnt/d/Users/Takuma/Creations/codes/City/Test/App && "./Test(debug).exe"
```

- 実行はユーザー操作なしで終了する。
- 成功時は終了コード0、失敗時は終了コード1を返す。
- `Test/App/TestResults/results.json`と`results.xml`へ機械可読な結果を出力する。
- テストケースは常設し、`Main.cpp`を都度書き換えない。
- `.cpp` / `.hpp` / `.h`はUTF-8 BOM + CRLFを保持する。
- 通常編集の前後に`convert_line_endings.py`を実行しない。破損修復時だけルート指示に従って使う。
