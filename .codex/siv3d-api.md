# Siv3D API 調査

Siv3D 0.6.16 の API が不明な場合に使う。読み取り専用。[調査メモ](../plan/SIV3D_NOTES.md)を検索し、未解決の点だけ SDK で確認する。

- SDK: `D:/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`（WSL: `/mnt/d/Program Files/Siv3D/OpenSiv3D_0.6.16/include/Siv3D/`）。
- 対象の `ClassName.hpp` を読み、実装が必要なら `detail/*.ipp`、用例が必要ならプロジェクト内の呼出箇所を調べる。既存 API を優先し、自前で再実装しない。
- 根拠となるヘッダと必要なシグネチャ、戻り値・制約を日本語で簡潔に返す。関連しないオーバーロードやクラス一覧は列挙しない。未確認事項は明記する。
