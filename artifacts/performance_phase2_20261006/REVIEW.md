# 追加の性能改善（2026-10-06）

提案した道路の空間索引、設備生成の非同期化、GPU転送の分割、初回準備、交通処理の改善を実装した。基準は前回の性能改善コミット `cbaed46`。平均FPSだけでなく、初回負荷と各処理の時間を分けて検証した。

## 実装

- 道路・交差点・計画線・信号の探索にチャンク単位の索引を使う。曲線全体と設備の張り出しを含め、道路の追加・削除・地形編集で更新する。広域の道路バッチも所有チャンクから再構築する。
- 街灯・電線・山道設備・標識のCPU生成を最大2ワーカーへ移した。ワーカーは地形・道路・定義のコピーを使用し、GPU操作は描画スレッドで行う。古い結果は改訂番号で破棄し、全転送完了後に公開する。
- 地形・建物・道路設備のメッシュを原則16,384頂点／8,192三角形以下に分割する。転送は地形・建物約2ms、設備約1msの予算で進める。単一のネイティブGPU呼び出しを途中で中断する保証はない。
- 列車・車両・歩行者のモデル、インスタンス用シェーダ、地図・診断文字、初期視点付近の道路設備をロード中に準備する。seed 42の新規街では約1,623道路分の設備準備に約3.5秒を要した。地図の地形更新は1msずつ進め、完成するまで前の地図と座標範囲を保持する。
- 車両ID索引の毎フレーム再構築をなくし、生成・削除・配列圧縮時に更新する。画面外の交通も更新を続ける。性能計測専用モードは詳細JSONの出力を軽量化し、通常の操作検証では従来の詳細情報を維持する。

## 条件と結果

Windows x64、Siv3D 0.6.16、Release、1920×1080、seed 42。固定視点は現行形式の `App/saves/refactoring_20261006` を使用し、yaw=-0.7、pitch=0.72。旧形式セーブの互換性は対象外。

交通が動く比較は8時・速度1、各視点2,400フレームの後半1,200フレームを集計した。稼働車両は約3,200台。時間停止の比較は13時・速度0、各視点480フレームの後半240フレームを集計した。初回最大はウォームアップも含む。FPSは平均フレーム間隔の逆数で、瞬間FPSの平均ではない。

| 交通が動く視点 | 基準 | 改善後・詳細診断あり | 最終版・軽量診断 |
|---|---:|---:|---:|
| 近景650m：道路CPU平均 | 5.682ms | 3.731ms | 3.761ms |
| 広域3,000m：道路CPU平均 | 3.401ms | 2.290ms | 2.271ms |
| 広域3,000m：交通更新平均 | 2.804ms | 2.541ms | 2.445ms |
| 近景：描画CPU初回最大 | 149.976ms | 43.103ms | 38.880ms |
| 近景：平均フレーム間隔 | 18.736ms | 17.859ms | 17.148ms（約58.3FPS） |
| 広域：平均フレーム間隔 | 16.672ms | 16.671ms | 16.670ms（約60FPS） |

「改善後・詳細診断あり」は初期設備準備などの最終調整前の実行。基準と同じ詳細診断条件でも道路・交通の改善方向を確認できる。最終版は診断出力の負荷も異なるため、FPS差を全てゲーム処理の改善に帰属させない。生データと集計は [traffic_comparison.json](traffic_comparison.json)、[traffic_baseline](traffic_baseline/)、[traffic_final](traffic_final/)、[traffic_perf_final](traffic_perf_final/)。

最終版の交通計測では到着済み移動が175→840件に増加し、稼働車両3,202台、経路失敗0件だった。画面外の交通を停止して負荷を減らした結果ではない。[終了時の状態](traffic_perf_final/state_2.json)。

| 時間停止の視点距離 | 前回版・描画CPU平均 | 最終版・描画CPU平均 | 最終版・初回最大 |
|---|---:|---:|---:|
| 650m | 12.13ms | 10.967ms | 34.976ms |
| 3,000m | 6.25ms | 4.968ms | 13.787ms |
| 6,000m | 6.00ms | 4.706ms | 10.326ms |
| 12,000m | 5.71ms | 3.821ms | 8.848ms |

時間停止では全4視点が平均約60FPS。前回版と最終版は診断出力条件が異なる。前回値は [前回の記録](../performance_20261006/final/summary.json)、今回値は [wide_perf_final/summary.json](wide_perf_final/summary.json)。

同じ新規街の6段階移動テストでは、初回CPU最大199.882→62.859ms、初回徒歩段階の最大78.276→47.144ms。列車の初回描画は93.96→0.90ms、地図は約22→1.76msまで減少した。地形・道路の転送最大は同経路で12.44→3.46ms。[navigation_baseline](navigation_baseline/)、[navigation_final](navigation_final/)、[転送集計](upload_comparison.json)。

広域移動720フレームも完走した。CPU平均7.580→6.189ms、P95 15.715→14.321ms。ただし最大は25.092→28.933msで、全ての最大負荷が改善したわけではない。[streaming_previous](streaming_previous/)、[streaming_final](streaming_final/)。

## 残る負荷

新規街の初回約63ms、徒歩切り替え約47msの短い停止は残る。6段階全体のCPU P95も24.422→24.900msで改善を確認できなかった。ロード中への先行準備はプレイ開始までの待ち時間との交換になる。

稼働車両約3,200台の広域ではフレーム間隔P95が約28ms。25msを超えた298/1,200フレームの平均は描画7.193ms、ゲーム更新3.738ms、`System::Update()` 16.154ms、診断0.047msだった。残る揺れは主にエンジンの更新・画面提示区間で観測されるが、その内部原因までは断定しない。詳細は [frame_diagnostics.json](frame_diagnostics.json)。他シード・街規模・全視点での60FPS維持は保証しない。

## 検証と再実行

本体・Testの最終Releaseビルドは警告0・エラー0、全330件のテストは成功した。成否・実行IDは [validation.json](validation.json)、全件結果は [full_tests/results.json](full_tests/results.json)。追加した4ケースはメッシュ分割の属性・三角形保持、地形スナップショットの独立性、段階更新地図のGPU画素一致、道路索引・非同期設備のGPU画素一致を検証する。道路ケースには負座標を跨ぐ計画線、追加、連続地形編集、ワーカー実行中の削除も含む。既存の交通件数・画面外移動テストも実行した。

以下は1つずつ実行する。固定視点計測には上記の現行セーブが必要。`-Label` には新しい記録名を指定する。

```powershell
python chore/run_check.py build --target City
python chore/run_check.py build --target Test
python chore/run_check.py test
python chore/run_check.py playtest --mode navigation --seed 42
python chore/run_check.py playtest --mode streaming --seed 42
powershell -NoProfile -File artifacts/performance_phase2_20261006/traffic_probe.ps1 -Label traffic_recheck
powershell -NoProfile -File artifacts/performance_phase2_20261006/wide_probe.ps1 -Label wide_recheck
python artifacts/performance_phase2_20261006/summarize.py wide_recheck
```

`measure.py` は保存済み交通比較、`frame_analysis.py` は最終版の区間計測を集計する。最終版より前の `updateMs` 列は描画を含む以前の区間定義なので区間加算には使用しない。`audit_scope.py` は変更ソースのBOM・CRLF・空白・プロジェクト登録とハッシュを確認する。
