# ロード失敗からタイトルへ戻る

2026-10-02 UTC。ゲーム継続・操作性レビューで確認した、失敗時の復帰経路のみを対象とする。性能最適化・セーブ互換性の変更は含まない。

## 再現と原因

変更前の実アプリで存在しないスロットをロードすると、0%のロード画面に赤い失敗メッセージを表示する。開始14秒時点のEsc入力後も24秒時点で同じ画面から戻れず、Alt+F4で終了した。既存セーブの破損・削除は行っていない。

`GameScene::update()` はLoading中に通常入力を通さず、`updateLoading()`へ進む。失敗したfutureを消費した後は毎フレームエラー画面を描くだけで、戻るボタンも入力処理もなかった。

## 対応と安全境界

失敗が確定しバックグラウンド処理が終了した場合だけ、エラーの下に「タイトルに戻る (Esc)」を表示する。クリックまたはバッファ済みEscでタイトルへ戻る。進行中の処理や正常完了画面からの中断は追加しない。

ロード前に既存セーブを書き換えず、失敗画面からの復帰でも保存・削除を行わない。タイトルでは通常どおり別のセーブか新しい街を選べる。

## 検証状況

- 修正前の実アプリ再現: 確認済み（専任の実プレイ担当による確認）
- Test UI preview `UI.LoadingFailureRecovery`: 1件成功。GPU描画検証成功。実プレイ担当の目視確認でも中央の日本語ラベル・コントラスト・収まりに問題なし
- 最終City/CityTestsビルド: 成功（共有ビルドログ `../../../build-tools/latest/build-road-edit-green.log`、警告・エラーなし）
- 最終focused test: `UI.LoadingFailureRecovery` 1件、`Clock.` 1件、`UI.PauseMenuAndControlHelp` 1件成功
- 実アプリ: 失敗画面のEscでタイトルへ復帰し、既存の使い捨てテスト都市を選択して実際のプレイ状態まで進めた。道路選択・削除操作も同じ実行で確認済み
- ライブでのボタンクリック復帰: 未実施。急角度コーナー問題の確認を優先したため。ボタン内外の入力判定と表示は上記focused testで確認済み

previewはボタン内クリック／範囲外クリック、短いEsc、処理中・成功時の非作動と、GPU readbackによる日本語ラベルの描画・ボタンからのはみ出しを検証する。ユーザーが今回許可した画像確認を実プレイ担当が行った。ユーザーへの画像添付は行っていない。

## 関連コードの限定レビュー

ポーズメニューは開く前のTimeSpeedを別途保存し、戻る時にその値を復元する。メニュー表示中は通常の速度操作を通さない。シーン破棄時にテキストフォーカスを解放し、シミュレーションを停止する。これらはコード確認であり、新規の実プレイ合格を意味しない。

futureはパイプラインが`this`を参照するため、復帰を`m_loadingFailed && !m_generationFuture.valid()`に限定する。`future.get()`で成功・例外を受け取った後にのみこの条件が成立する。新しいキャンセル処理やワーカーの強制停止は追加していない。

## 証拠

- `baseline-runtime.log`, `baseline-debug.log`: 修正前の失敗画面
- `preview-results.json`: 本体組込み前のUI preview 1/1
- `../road_edit_cycle/green/UI.LoadingFailureRecovery/results.json`: 最終UI test 1/1
- `green/`: 最終Clock・PauseMenuテスト結果
- `fixed-escape-debug.log`: `[Loading] Return to title after failure`（36.7197秒）
- `fixed-runtime.log`: 修正後の実行記録。失敗からタイトルへの復帰後に別都市をロード

全スイートの実行・Windows実機での確認は今回行っていない。
