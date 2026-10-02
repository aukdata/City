# 道路インスペクタの速度変更を交通へ反映

2026-10-02、道路速度の編集・描画・保存は成功していたが、走行／経路探索用のコピーに変更が届いていなかった問題を修正した。

## 原因と修正範囲

`GameScene::drawEdgePanel()` は速度変更時に描画キャッシュのみを無効化していた。自動車の IDM 目標速度は `SimGraph`、経路探索の区間コストはそのスナップショットから構築した `TrafficGraph` を参照するため、単独の速度編集だけでは古い速度が残る。定期的な再同期はなく、別の近傍変更通知や再読込まで持ち越されていた。

`RoadInspectorEdit::editSpeedLimit()` を実際のインスペクタと恒常テストで共用した。数値入力は元の `edge.speedLimit` への参照を保つため、アドレスをキーにするテキスト編集状態を変えない。編集前後の値が異なる場合だけ既存の `notifyNetworkChanged({nodeA, nodeB})` を呼ぶ。車線・閉鎖・他の編集項目の通知は変更していない。

## Red → Green

- 恒常テスト `RoadIntegrity.InspectorSpeedRefreshesSimulation` は、実製品と同じ編集／通知ヘルパーを呼ぶ。テストの `SimGraph`／`TrafficGraph` 更新も、その通知コールバックが呼ばれた場合に限る。編集後にテスト側から無条件で同期する抜け道はない。
- [Red](red/RoadIntegrity.InspectorSpeedRefreshesSimulation/results.json): 1件失敗、期待した5アサーション。通知回数0、スナップショット30のまま、100m道路の両方向区間コスト12秒のまま（期待9秒）、最終通知回数も0。値の参照アドレス、変更なし判定、無関係道路の速度保持は失敗しなかった。
- 修正は実変更後の通知呼出しを追加する最小差分。30→40で道路とスナップショットが40、区間コスト12→9秒、通知は1回。変更なし30と同値40の再入力では追加通知せず、無関係道路の50km/hを保持する。
- [RoadIntegrity](green/RoadIntegrity/results.json) 20/20、[SharedTransport](green/SharedTransport/results.json) 9/9、合計29/29成功。
- 最終の Linux `City`／`CityTests` [ビルドログ](build.log) と [終了コード](build.exit): 0。ログに警告・エラーなし。

## ネイティブ実操作

専用セーブコピーで既存道路41610を選択し、速度欄のホイールで30→40→30を操作した。再読込や無関係な道路編集なしで、同一選択の道路値とライブ `SimGraph` 値が一致した。

| 状態 | フレーム | 道路 | SimGraph |
|---|---:|---:|---:|
| 変更前 | 210 | 30 | 30 |
| ホイール変更後 | 375 | 40 | 40 |
| 元の値へ復元 | 510 | 30 | 30 |

[保持した数値要約](native_summary.json)。元のローカル記録は `native_before.json`、`native_changed.json`、`native_restored.json`、`native-debug.log`。ログには対象操作に対応する2回のネットワーク更新がある。復元後は保存せず正常終了し（[終了コード0](native.exit)）、専用セーブは変更しなかった。

この検証はライブスナップショット反映と経路グラフの区間コストを対象とする。実際の自動車の走行軌跡・加減速・所要時間を測るベンチマーク、および道路網全体の経路選択変化の計測は実施していない。今回の修正以前の保存・再読込・地図開閉は [前段の実操作](../road_inspector_cycle/REVIEW.md) に記録済み。
