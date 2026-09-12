# 道路オブジェクト・高架橋仕様書

## 概要

道路エッジに紐づく汎用オブジェクト（**RoadObject**）システムを導入する。橋脚・街灯・標識・電柱・街路樹など、道路に沿って配置されるあらゆるオブジェクトをこの仕組みで統一的に扱う。

初期実装として**橋脚（Pier）**を対象とし、高架道路の自動検出と橋脚の自動配置を実現する。

---

## 1. RoadObject データ構造

### 1.1 RoadObject

```cpp
struct RoadObject
{
    int             id = -1;
    int             parentEdgeId;       ///< 親エッジ ID
    float           arcPos;             ///< エッジ上の弧長位置 [m]
    float           lateralOffset = 0;  ///< 横方向オフセット [m]（道路中心基準）
    RoadObjectType  type;               ///< オブジェクト種別
    float           scale = 1.0f;       ///< スケール倍率
    float           yawOffset = 0.0f;   ///< Y軸回転オフセット [rad]

    // 種別ごとの追加パラメータ
    float           heightOverride = -1.0f;  ///< -1 = 地形まで自動延伸（Pier 用）
};
```

### 1.2 RoadObjectType

```cpp
enum class RoadObjectType : uint8
{
    // Phase 1（初期実装）
    Pier,           ///< 橋脚

    // Phase 2（将来拡張）
    StreetLight,    ///< 街灯
    TrafficSign,    ///< 交通標識
    UtilityPole,    ///< 電柱
    StreetTree,     ///< 街路樹
    SoundBarrier,   ///< 遮音壁
    Guardrail,      ///< ガードレール支柱
    TollGate,       ///< 料金所ゲート
};
```

### 1.3 管理場所

`RoadNetwork` に `Array<RoadObject> m_objects` を追加。エッジと同様に ID 管理する。

```cpp
class RoadNetwork
{
    // 既存
    Array<RoadEdge> m_edges;
    Array<RoadNode> m_nodes;

    // 追加
    Array<RoadObject> m_objects;
    int m_nextObjectId = 0;

public:
    int addObject(RoadObject obj);
    void removeObject(int objectId);
    void removeObjectsByEdge(int edgeId);  ///< エッジ削除時に連動
    const Array<RoadObject>& objects() const;
    RoadObject* getObject(int id);
};
```

---

## 2. 高架道路

### 2.1 高架判定

道路の**路面高さ**と**地形高さ**の差（ギャップ）が閾値以上の区間を高架とみなす。

```
gap(s) = roadSurfaceY(s) - terrainY(s)
```

| 条件 | 状態 |
|---|---|
| `gap < 1.0m` | 地上道路（通常） |
| `1.0m ≤ gap < 3.0m` | 盛土区間（Slope パーツで法面表示） |
| `gap ≥ 3.0m` | **高架区間**（Slope 非表示、橋脚を配置） |

### 2.2 路面高さの決定

現在の実装では道路は常に地形追従している（`makeSlice()` で `computeHeight` を使用）。高架道路を実現するために、**RoadEdge のベジェ曲線の Y 座標を路面高さとして使用する**オプションを追加する。

```cpp
struct RoadEdge
{
    // 既存フィールド ...

    bool useElevation = false;  ///< true: ベジェ Y を路面高さとして使用
                                ///< false: 地形追従（従来動作）
};
```

`useElevation = true` の場合:
- `makeSlice()` で `computeHeight` の代わりにベジェ曲線の Y 座標を路面高さとする
- ノードの Y 座標がそのまま道路の高さになる

### 2.3 橋脚の自動配置

`useElevation = true` のエッジに対して、以下のアルゴリズムで橋脚を自動配置する。

```
入力: edge（高架エッジ）
定数: PIER_INTERVAL = 30.0m, PIER_THRESHOLD = 3.0m

1. エッジのベジェ曲線を PIER_INTERVAL 間隔でサンプリング
2. 各サンプル点で gap = bezierY(s) - terrainY(s) を計算
3. gap >= PIER_THRESHOLD の点に Pier オブジェクトを配置
4. heightOverride = -1（地形まで自動延伸）
```

自動配置は以下のタイミングで実行:
- エッジ作成時（`useElevation = true` 設定後）
- エッジのベジェ曲線変更時（制御点ドラッグ後）

自動配置されたオブジェクトはプレイヤーが個別に移動・削除可能。

---

## 3. 橋脚の描画

### 3.1 メッシュ生成

橋脚は以下の手順で描画する:

1. `arcPos` からベジェ曲線上の位置 P と接線 T を取得
2. 地形高さ `terrainY = computeHeight(P.x, P.z)` を取得
3. 路面高さ `roadY = P.y`（useElevation 時）または `terrainY + terrainLift`
4. 橋脚の上端 = `roadY - 路盤厚（≈0.5m）`、下端 = `terrainY`
5. 接線 T から right ベクトルを計算し、橋脚の向きを決定
6. 直方体または T 型断面のメッシュを生成

### 3.2 橋脚スタイル（将来拡張）

初期実装ではシンプルな直方体コラム（幅 2m × 奥行 1.5m）。

将来:
- `PierStyle::Column` — 円柱（単柱）
- `PierStyle::TBeam` — T 型橋脚（横梁付き）
- `PierStyle::Wall` — 壁式橋脚

### 3.3 描画タイミング

`RoadRenderer::render()` 内でエッジ描画後に、そのエッジに属する RoadObject を描画する。橋脚はキャッシュ可能（エッジメッシュキャッシュと連動して invalidate）。

---

## 4. 高架区間の断面変化

高架区間では断面パーツの表示が変わる:

| パーツ種別 | 地上 | 高架 |
|---|---|---|
| Roadbed | 表示 | 表示 |
| Shoulder | 表示 | 表示 |
| Sidewalk | 表示 | 表示 |
| Curb | 表示 | 表示 |
| **Slope** | **表示** | **非表示** |
| **Wall** | 非表示 | **表示**（高欄として機能） |
| Median | 表示 | 表示 |

→ 同一エッジ内でも区間ごとに gap を判定し、Slope と Wall の表示を切り替える。

---

## 5. セーブ・ロード

RoadObject は `roads.bin` に RoadEdge/RoadNode と同じバイナリ形式で保存する。

```
[Object Header]
  objectCount: int32
[Object Records] × objectCount
  id: int32
  parentEdgeId: int32
  arcPos: float32
  lateralOffset: float32
  type: uint8
  scale: float32
  yawOffset: float32
  heightOverride: float32
```

---

## 6. UI

### 6.1 高架設定

エッジパネルに高架トグルを追加:

```
[✓] Elevated   (useElevation チェックボックス)
```

ON にすると:
1. ベジェ Y 座標が路面高さとして反映される
2. 橋脚が自動配置される
3. ノードの Y 座標をスピナーで調整可能になる

### 6.2 オブジェクト編集（将来）

選択したオブジェクトのプロパティパネル:
- 位置（arcPos スピナー）
- 横オフセット（lateralOffset スピナー）
- 種別表示
- 削除ボタン

---

## 7. エッジ削除時の連動

`RoadNetwork::removeEdge(edgeId)` 実行時に `removeObjectsByEdge(edgeId)` を自動呼び出し。親を失ったオブジェクトは残さない。

---

## 8. 実装フェーズ

### Phase 1（初期）
- RoadObject データ構造 + RoadNetwork への組み込み
- RoadEdge::useElevation フラグ + makeSlice 分岐
- 橋脚自動配置アルゴリズム
- 橋脚メッシュ描画（直方体コラム）
- セーブ・ロード対応

### Phase 2（拡張）
- 橋脚スタイル追加（T 型、壁式）
- 高架区間の Slope/Wall 自動切替
- UI: オブジェクト選択パネル
- OBJ モデルベースのオブジェクト描画

### Phase 3（汎用化）
- StreetLight / TrafficSign / UtilityPole / StreetTree 追加
- パーツに紐づく自動配置ルール（歩道パーツ → 街灯自動配置など）
- LOD 対応

---

## 9. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `16_road_cross_section_spec.md` | §6 橋梁・高架の判定ロジックを本仕様で詳細化 |
| `07_road_lane_spec.md` | RoadEdge への useElevation フィールド追加 |
| `12_visual_spec.md` | §9 街路要素の自動配置を RoadObject で実現 |
| `14_save_spec.md` | roads.bin に RoadObject セクション追加 |
| `17_road_node_spec.md` | ノード Y 座標の意味が高架時に変わる |

### 2026-09-12: コンクリート電柱

道路付属の電柱は高さ10.2m・下部半径0.18mの12角柱とし、上端を60%へ絞る。2段の腕金、碍子とひだ、柱上変圧器と端子、取付帯、引込管、管理札、昇柱ボルト、弛んだ3本の電線を一体のバッチにまとめる。コンクリート・金属・碍子・電線の材質は分離する。

### 2026-09-12: 道路工事の段階描画と撤去

実装済みの着工・自動撤去・通行制御・実写材質・橋脚の段階形状・撤去履歴の保存は `23_road_construction_spec.md` を参照。

## 2026-09-13 規制・警戒標識の追加

`reference/ks084812_INDEX.md` の標識番号323（最高速度）、326（一方通行）、202（曲がりあり）を参照する。制限速度は赤い円、一方通行は青い矩形の矢印、曲線警戒は黄色い菱形と黒矢印を生成する。制限値・方向をテクスチャキーに含める。矢印面・支柱の向きは進入方向から決定する。道路の速度、方向規制、曲がり角に応じて、自動配置の位置を交差点cutoff外へ制限する。

従来参照だけ存在して欠落していた円形板・逆三角形板のOBJを追加する。`scripts/build_regulatory_boards.py` から再生成可能。案内標識の交差点から30mの除外は別系統として維持する。
