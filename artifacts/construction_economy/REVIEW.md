# 通常モードの着工費・工事取消確認

2026-10-02 / 調査対象: `23875a2`。既存セーブを変更せず、実プレイは専用コピー `App/saves/economy_review` を使う。

## 現行仕様と実装

- 計画作成・保存は無料、着工時に概算費を一括支払いする。[04 ゲームプレイ「建設」](../../plan/04_gameplay_detail_spec.md#建設)
- 未確定候補では道路分割・資金消費・撤去を行わず、資金・接続検査に失敗したら候補と街を維持する。成功後の支払いは1回で、同じ計画の再着工は拒否する。[23 工事「着工と通行」「実装の責務」](../../plan/23_road_construction_spec.md)
- サンドボックスは資金確認・引落しを省略する。通常の `--load` は `sandboxMode=true` を設定するため、従来のCLIロードによる取消確認は経済の検証にはならない。[23 工事「サンドボックスと生成曲線」](../../plan/23_road_construction_spec.md#サンドボックスと生成曲線)、[GameLaunchOptions.hpp](../../src/GameLaunchOptions.hpp):103–114
- **現行仕様に取消時の返金を約束する記述はない。返金なしを不具合と判定しない。** 旧案の「残額の20%」違約金は月払い構想の一部であり、現行仕様の根拠には使わない。[仕様目次](../../plan/SPEC_INDEX.md):3、[旧ゲームプレイ案](../../plan/unused/04_gameplay_detail_proposal_2026-04.md):75–89
- 現在の実装は成功時に `funds=max(0,funds-cost)` を実行する。未完成計画の「削除」は道路・計画を削除するが資金を変更せず、既払い額を保持する。[GameScene_Construction.cpp](../../src/scene/GameScene_Construction.cpp):5–33、[GameScene_Panels.cpp](../../src/scene/GameScene_Panels.cpp):1124–1159
- 道路削除後も撤去済み建物を復活させないことは現行仕様に明記されている。[23 工事「永続化」](../../plan/23_road_construction_spec.md#永続化)

## 金額の照合

資金・工費は億円。1km当たり生活道路0.5、幹線1.5、自動車専用道8、高速道路20。計画の換算延長は地上1倍・高架3倍・トンネル6倍（トンネルを優先）。例えば地上の生活道路100mは0.05億円＝500万円。[RoadNetwork_Plans.cpp](../../src/road/RoadNetwork_Plans.cpp):7–15,221–224、[RoadTypes.hpp](../../src/road/RoadTypes.hpp):461–467

画面の丸めた概算ではなく、保存した対象道路の実延長と構造係数から実際の引落額を照合する。計画集計の `totalCost` はfloat、確定時の請求値はdoubleなので、その表現精度も区別する。計画に属さない単独道路は現行仕様どおり実延長を課金に使う。

## 常設テストの範囲と不足

- [RefactoringTests.cpp](../../Test/RefactoringTests.cpp):111–167 は一括着工、資金不足時の無変更、選択入口の同値性、重複着工防止を確認する。ただし請求結果の `receipt.cost` の検証であり、実際の `Economy::funds` 引落しは通らない。
- [TransportPlanningTests.cpp](../../Test/TransportPlanningTests.cpp):123–203 は候補確定トランザクションの成功・拒否と道路網の維持を確認する。資金の実引落しは呼出し側の責務で、ここでは未検証。
- [ComprehensiveTests.cpp](../../Test/ComprehensiveTests.cpp):162–171 の `Comprehensive.SandboxConstruction` は資金制限の迂回を確認するが、サンドボックスの引落し省略を実残高で検証しない。
- [RoadConstructionTests.cpp](../../Test/RoadConstructionTests.cpp):167以降の `Construction.CancelledShadowClearsWhilePaused` は取消後の形状・影を確認し、残高は確認しない。
- [PlayabilityTests.cpp](../../Test/PlayabilityTests.cpp):589–602 の `Economy.GeneratedPopulationAndMaintenance` は未着工・開通後の維持費を確認する。工事中の維持費除外は実装済みだが、このケースには直接の期待値がない。

関連実行フィルタは `Construction`、補助は `Economy.GeneratedPopulationAndMaintenance`。Testのフィルタは単一部分文字列なので別々に実行する。今回コード・テストの変更、ビルド・Test再実行は行っていない。既存本体での実プレイ結果は以下に記録する。

## 実プレイ結果

初回・保存後再ロードの両方で、タイトル画面のサンドボックスを明示的に外し、専用コピー `economy_review` をロードした。[TitleScene.cpp](../../src/scene/TitleScene.cpp):65–72,106 はこのチェック状態をロード開始条件へ渡す。サンドボックス設定はセーブに永続化されないため、再ロードも同じタイトル経路を使用した。

集計: [native_summary.json](native_summary.json)。資金変更コマンドは使用せず、時間停止中に測定した。

| 確認 | 測定値・結果 |
|---|---|
| 着工前・候補作成後の資金 | どちらも30.0億円。候補作成は無料 |
| 対象 | 地上の生活道路、延長30.827463150024414m |
| 期待工費 | 延長÷1000×0.5＝0.015413731575012208億円 |
| 実引落額 | 0.01541373157501269億円。期待値との差は約4.82×10⁻¹⁶億円 |
| 着工後残高 | 29.984586268424987億円 |
| Enter再入力 | 残高29.984586268424987億円、追加請求0 |
| 進捗0%で取消 | 道路・計画を削除。残高29.984586268424987億円、返金0 |
| 保存・再ロード | 残高29.984586268424987億円で完全一致。取消道路が表示されないことも確認 |

実際の引落しにより非サンドボックス経路を端到端で確認できた。着工1回分の請求、再入力時の重複請求防止、取消時の既払い額保持、残高の保存復帰は今回の範囲で合格。返金0は現行実装の観測結果であり、新たな返金仕様を定めるものではない。

資金0での拒否は今回の実プレイでは未実施。常設テストの静的確認を、今回新たに実行した検証として扱わない。初回本体は終了コード0。再ロード側の最終終了結果はこの記録時点で未確認。

新たな経済上の不具合は確認されず、コード変更は行わなかった。元セーブと既存のステージ済み設計文書は変更していない。
