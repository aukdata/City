# 初期生成地区の道路刷新 — 集落3類型の差別化

## Context

日本の街づくり・交通シミュレーションゲームの初期マップ生成において、集落内部の道路が全て「回転した均一格子」で生成されており、集落種別の違いが間隔とサイズだけ、かつ全てが LocalRoad 2車線・ベジェ実質直線で、日本的な街の雰囲気が出ていない。

これを日本の歴史的集落類型に沿って3種類の生成ロジックに差別化する:

- **城下町 (CastleTown)** — 格子状＋街道クランク＋城広場＋武家地/町人地の二層密度＋辺間引き
- **宿場町 (PostTown)** — 街道 + 裏通り1〜2本（閉ループ）＋垂直路3〜5本の "はしご状"
- **農村 (Village)** — 街道 + 短い支線1〜2本（袋小路）の "櫛状" 木構造

併せて内部 enum を `Urban/Suburbs/Rural` → `CastleTown/PostTown/Village` にリネームし、日本語的に曖昧な「街／町」の区別をコード上明示する。セーブ互換は enum 数値（0/1/2）維持で担保。

※ **命名規約**: 本プロジェクトでは `Highway` は高速道路専用語。日本の歴史的街道は **`Kaido`** を用いる。

仕様書 `plan/03_procedural_generation_spec.md` §4〜§5 と乖離している箇所（集落間隔 500m→2800m 等）もこの機に追従更新する。

## 進捗（2026-04-23 時点）

- [x] **Step 1** — enum リネーム（完了・ビルド通過）
- [x] **Step 2** — DistrictRoads.{hpp,cpp} 新規作成＋ `extractKaido` 実装（完了・ビルド通過）
- [ ] **Step 3** — `generateDistrictRoads` ディスパッチ化 + Village 実装（進行中）
- [ ] **Step 4** — PostTown（はしご状）実装
- [ ] **Step 5** — CastleTown 前半（二層格子＋城広場＋辺間引き）
- [ ] **Step 6** — CastleTown 後半（街道クランク）

## 改変・新規ファイル

### 新規（済）
- `src/gen/DistrictRoads.hpp` / `DistrictRoads.cpp`
  - `KaidoSegment` 型、`extractKaido`、`generateCastleTown / generatePostTown / generateVillage`

### 改変
- `src/gen/MapGenerator.hpp` — enum 定義（済）、`generateDistrictRoads` をディスパッチ化（未）
- `src/gen/MapGenerator.cpp` — enum 参照・変数名置換（済）、ディスパッチ実装（未）
- `src/scene/GameScene.{cpp,hpp}` — enum 参照・`m_castleTownCenters` リネーム（済）
- `src/scene/GameScene_Panels.cpp` — UI 表示「[城]/[宿]/[村]」（済）
- `src/render/MinimapRenderer.cpp` — 地名表示条件（済）
- `src/road/RoadNetwork.hpp` — `tier` コメント（済）
- `City.vcxproj` / `City.vcxproj.filters` — DistrictRoads 追加（済）
- `plan/03_procedural_generation_spec.md` — 仕様追従更新（最後のステップ）

### 除外（リネーム対象外・別概念）
- `ZoneTypes.hpp` の `ZoneType::UrbanControl`（市街化調整区域）
- `Chunk.hpp::isUrbanizationArea`（市街化区域フラグ）

## 設計決定

### `Settlement` にフィールド追加 → 不採用
当初計画では `Settlement::kaidoEdgeIds / kaidoDir` を追加予定だったが、**街道情報は `generateDistrictRoads` 実行時に都度計算で十分**と判断し、Settlement は変更しない。セーブ互換の考慮も不要になる。

### 街道抽出の方針
`DistrictRoads::extractKaido()` は実装済み:

1. 集落中心に最も近い既存ノードを `network.findNodeNear` で取得
2. その中心ノードに接続する Arterial/LocalRoad のうち、最も対向する2本を選ぶ（内積最小ペア）
3. 中心ノードから両方向に **直進的に** 辿る（各ジャンクションで `cos > 0.3` の最大一致エッジを継続に選ぶ）
4. 集落影響半径 `searchRadius` を超えたら終了
5. 結合して `KaidoSegment { edgeIds, nodeIds, dirAtCenter, centerNodeId, passesThrough }` を返す

**貫通しない場合の引込み生成**は Step 3 の時点で必要に応じて追加。現状は空セグメント（passesThrough=false）を返すのみ。

## 既存資産の再利用

- `RoadPathfinder`（`src/gen/RoadPathfinder.hpp`）— A* 経路（引込み生成・CastleTown 格子後補正で利用予定）
- `RoadNetwork::splitEdgeAt(edgeId, arcLength)`（`src/road/RoadNetwork.hpp:87`）— 街道クランクの折れ点挿入
- `RoadNetwork::addEdge / removeEdge`（`RoadNetwork.hpp:21,27`）
- `RoadNetwork::findNodeNear`（`RoadNetwork.hpp:63`）— 街道抽出で使用
- `RoadNode::attachments` — ノード接続エッジ走査
- 既存 `generateDistrictRoads` 内ローカル `tryAddEdge` / `kNodeMergeRadius = 25m` のロジックは DistrictRoads.cpp に移植

## 主要シグネチャ（実装済）

```cpp
enum class SettlementKind : uint8
{
    CastleTown = 0,  // 旧 Urban（数値維持でセーブ互換）
    PostTown   = 1,  // 旧 Suburbs
    Village    = 2,  // 旧 Rural
};

namespace DistrictRoads {
    struct KaidoSegment {
        Array<int> edgeIds;            // 連続エッジ列（端 → 端）
        Array<int> nodeIds;            // 対応ノード列（edgeIds.size()+1 個）
        Vec2       dirAtCenter{1, 0};  // 中心付近の街道方向（XZ 単位ベクトル）
        int        centerNodeId = -1;
        bool       passesThrough = false;
    };

    KaidoSegment extractKaido(
        const MapGenerator::Settlement& settlement,
        const RoadNetwork& network,
        float searchRadius);

    void generateCastleTown(uint64 seed, int si, const MapGenerator::Settlement&,
                            const KaidoSegment&, const World&, RoadNetwork&);
    void generatePostTown (uint64 seed, int si, const MapGenerator::Settlement&,
                            const KaidoSegment&, const World&, RoadNetwork&);
    void generateVillage  (uint64 seed, int si, const MapGenerator::Settlement&,
                            const KaidoSegment&, const World&, RoadNetwork&);
}
```

## ステップ詳細

### Step 3 — ディスパッチ化 + Village 実装

**方針:**
1. `MapGenerator::generateDistrictRoads` の本体を分割:
   - 共通処理（nodeHash 構築、タイプ別 kaido 抽出）は MapGenerator.cpp に残す
   - タイプ別生成は `DistrictRoads::generate*` にディスパッチ
   - 既存格子ロジックは CastleTown/PostTown 用に一時的に `generateLegacyGrid()` として DistrictRoads.cpp に移植（Step 5 で置換）
2. `generateVillage` を実装:
   - `kaido.edgeIds` のうち中央付近のエッジを 1〜2 本ランダム選択
   - 各エッジの弧長中点で直交方向（± `kaido.dirAtCenter` の法線）へ長さ 60〜120m の支線を伸ばす
   - 端点に新規ノード追加 → 袋小路
   - `RoadType::LocalRoad, 2車線`、直線ベジェ（ctrl = 1/3, 2/3 点）
   - 傾斜 10% 超と水面（y < 0.5）はスキップ
3. CastleTown/PostTown は `generateLegacyGrid()` を呼ぶスタブで温存

**動作確認**: App 起動 → 複数 seed で農村地区に櫛状路が出ること

### Step 4 — PostTown（はしご状）

1. `kaido.dirAtCenter` を長辺、直交方向を短辺とする
2. 街道長（kaido.edgeIds 総弧長）を 3〜5 等分し、街道上の分割点ノードを取得（`splitEdgeAt` で新規作成）
3. 裏通りノード列: 街道分割点から法線 ±40〜60m オフセット位置に作成（ノイズ入り）
   - 乱数で片側 1 本 or 両側 2 本
4. 分割点 ↔ 裏通りノード を垂直路で接続
5. 裏通りの連続ノードを直線エッジで接続
6. 裏通り両端を街道両端ノードに再接続 → 閉ループ完成
7. 全て `LocalRoad, 2車線`

**動作確認**: 宿場町地区で裏通り 1〜2 本＋垂直路が見えること

### Step 5 — CastleTown 前半（二層格子 + 城広場 + 辺間引き）

1. 格子の基準軸を `kaido.dirAtCenter` に設定（既存の `gridAngle` を置換）
2. `buildTwoTierGrid` で二層密度:
   - 町人地: 中心半径 0〜150m、間隔 50m
   - 武家地: 150m〜`extent/2`、間隔 80m
3. `carveCastleBlock`: ローカル原点付近 60m 角のエッジ生成をスキップ、ノードは残存
4. `pruneGridEdges`: 格子エッジの 5〜15% をランダム削除
   - 削除後の両端ノード次数 ≥ 2 を維持
5. 既存の街道エッジは変更せず、格子エッジが街道ノードに自然合流するよう `kNodeMergeRadius = 25m` で統合

**動作確認**: 城下町地区で二段密度＋中心空白＋ランダム穴を目視

### Step 6 — CastleTown 後半（街道クランク）

1. `kaido.edgeIds` のうち集落中心に最近いエッジを 1 本特定
2. `RoadNetwork::splitEdgeAt` で 2 回分割 → 新規ノード N1, N2
3. N1, N2 を街道法線方向へ ±25〜40m ずらす
4. ベジェ制御点再計算:
   - `splitEdgeAt` が制御点を再設定しているかを `RoadNetwork.cpp` で事前確認
   - 不可なら `removeEdge + addEdge` リビルド方式（`RoadRoute` 整合フック L363 の挙動要確認、国道指定が外れる可能性）
5. クランク後に `updateEdgeElevation` / `rebuildNodeConnectivity` で整合

**動作確認**: 城下町貫通街道が 2 回折れていること、複数 seed で回帰

## 不確実な点・要検証

1. **`RoadEdge` ベジェ制御点書き換え可否** — Step 6 着手時に `RoadNetwork.cpp::splitEdgeAt` と `RoadEdge` 公開 API を確認
2. **`RoadRoute` 整合フック** — クランクで Arterial を分割時に NationalRoute 登録が維持されるか（`RoadNetwork.hpp:363`）
3. **辺間引きの連結性** — 次数 ≥ 2 維持だけでは飛び地クラスタ可能性あり。目視確認 → 必要なら Union-Find 導入
4. **街道貫通しないケース** — 現状 `extractKaido` は空を返す。Step 3 で引込み生成か、格子のみフォールバックかを判断
5. **仕様書追従** — `plan/03_procedural_generation_spec.md` を実装値に合わせて更新（最後のコミット）

## 動作確認手順

- **LF/CRLF 変換**: `python3 chore/convert_line_endings.py to-lf -d src Test` / `to-crlf`
- **ビルド**: `"/mnt/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" City.sln -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal -noLogo`
- **起動**: `cd App && "./City(debug).exe"`（ユーザ明示指示がある時のみ）
- **Test プロジェクト**: `cd Test/App && "./Test(debug).exe"` でスクリーンショット検証
- **回帰**: Step 1 直後の既存セーブロード確認で道路表示が不変
- **ログ**: 各生成関数で `Logger << U"[DistrictRoads]"` 体裁のエッジ数ログ

## Critical Files

- `src/gen/MapGenerator.cpp` / `.hpp` — ディスパッチエントリ
- `src/gen/DistrictRoads.cpp` / `.hpp` — 3類型生成ロジック本体
- `src/road/RoadNetwork.hpp` — `splitEdgeAt`, `addEdge`, `removeEdge`, `findNodeNear`, `attachments`
- `src/gen/RoadPathfinder.hpp` — A*（必要時）
- `src/scene/GameScene.cpp` / `.hpp` — セーブ/ロード経路
- `plan/03_procedural_generation_spec.md` — 仕様ドキュメント（最後に追従）
