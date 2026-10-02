# 工事中止後に残る掘削機の影

2026-10-02 UTC。Linux の独立した現在形式 seed42 v20 コピーと、本体の描画コードを使用する常設 GPU readback テストで検証。

## 実表示と原因

修正前、停止中の道路工事を計画一覧の「削除」で中止すると、施工面と掘削機は消えるが、草地に掘削機型の影が残った。後続の再建設・完成・時刻進行でも残り、再読込では消えた。実際の描画品質は標準、1280×768、影あり。起動引数から軽量描画だと推定した初期説明は訂正済みで、軽量描画の不具合としては扱わない。

原因は独立した二点。

1. 計画削除が先に道路エッジを削除し、その後に端点の現存 attachment だけを無効化していた。削除済みエッジの工事描画キャッシュには到達せず、影の描画側には掘削機が残る。
2. `RoadRenderer::eraseEdgeCaches` が工事キャッシュを削除しても、完成道路用キャッシュがなければ `geometryRevision` を更新しなかった。呼出側が明示的に工事キャッシュを消しても、停止中の同一カメラ・太陽では既存の影テクスチャを再利用する。

## 最小修正

- `GameScene_Panels.cpp`: 未完成計画の削除時に、エッジと attachment が存在する間に `invalidateEdgeCache(eid, nodeA, nodeB)` を呼ぶ。
- `RoadRenderer_Streaming.cpp`: 工事キャッシュの存在もリビジョン更新条件に含め、判定後にキャッシュを消す。
- fallback キャッシュ、影の品質、時計・工期、保存形式は変更していない。

## 常設回帰: red → green

`Construction.CancelledShadowClearsWhilePaused` は本体の `RoadRenderer`、工事モデル、`CityLighting` と実際の静的深度テクスチャを使用する。時刻6、同一カメラ・太陽で工事描画をキャッシュし、明示的なエッジ無効化と本体ネットワークAPIによる削除を行う。GUI削除分岐のコピーではなく、描画側の無効化契約を検証する。GUIの欠落呼出しは後述の実アプリで確認した。

修正前は1ケースで意図した4 assertion が失敗。

- geometryRevision: 1 → 1（期待: 増加）
- 占有深度ピクセル: 67,785 → 67,785（期待: 0）
- 静的影の再描画回数: 1（期待: 2）
- 次の停止フレームも67,785ピクセル残留

明示的なエッジ無効化後にカメラを動かすと0ピクセルになり、削除済み深度の再利用問題を切り分けた。

修正後:

- geometryRevision: 1 → 2
- 占有深度ピクセル: 67,785 → 0
- 中止までの静的影再描画: 2回
- 再度の無効化と同一停止ビューではリビジョン・再描画回数が増えず、0ピクセルを維持
- カメラ移動後も0ピクセル。総再描画は3回

最終の関連テストは Construction 15/15、CityLighting 3/3、RenderQuality 1/1 成功。City と CityTests のビルドは終了コード0。全テストスイートや Windows 実機の検証ではない。

## 修正後の実アプリ

同じ現在形式コピー、標準品質1280×768で、実マウス操作により約28.29mの独立道路計画3を生成・着工し、時間停止中に計画一覧の「削除」を押した。

- 施工面・掘削機・その影が消え、既存道路と街灯の影は維持された。
- before frame735 / after frame1005。どちらも `timeSpeed=0`、ゲーム時刻約12.445、内部進行時刻26.8。
- カメラfocusは両方 `(23216.494382, 29.827501, 26533.348315)`、eyeYは129.126064で同じ。
- 操作後に選択計画はなく、再読込や時刻進行を必要とせず消失を確認。
- 描画距離100mの設定は建物・樹木向けで、工事・道路の影を一括非表示にする回避策ではない。

## 証拠

- [修正前の4失敗](../shadow_cli/red/Construction.CancelledShadowClearsWhilePaused/results.json)
- [修正前readback](../shadow_cli/red/Construction.CancelledShadowClearsWhilePaused/readback.json)
- [Construction 15件の成功](../shadow_cli/green/Construction/results.json)
- [修正後readback](../shadow_cli/green/Construction/readback.json)
- [CityLighting](../shadow_cli/green/CityLighting/results.json)、[RenderQuality](../shadow_cli/green/RenderQuality/results.json)
- [実操作前の状態](../shadow_cli/native_before_cancel.json)、[実操作後の状態](../shadow_cli/native_after_cancel.json)
- ローカル画像: `../shadow_cli/native_before_cancel.png`、`../shadow_cli/native_after_cancel.png`
- 実行ログ: `../shadow_cli/shadow-native-debug.log`

大きな測定ログ・画像・状態JSONは作業環境に保持し、この変更のコミット対象はソース3ファイルとこのレビューのみ。
