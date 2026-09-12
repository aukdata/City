# 道路ノード接続仕様書

## 概要

道路ノード（RoadNode）は道路エッジの接続点であり、端点・継ぎ目・交差点・分岐合流の4種類がある。ノードの種類に応じて異なるジオメトリを生成し、部品・車線の幅や構成が異なる道路同士を違和感なく接続する。

---

## 1. ノード種別

```cpp
enum class NodeType : uint8 {
    Endpoint,       // 1本: 端点（道路の終端）
    Joint,          // 2本: 継ぎ目（道路の接続点）
    Intersection,   // 3本+: 交差点（フィレット曲線で接続）
    Diverge,        // 3本+: 分岐合流（本線継続 + ランプ剥離）
};
```

### 判定ルール

| 条件 | NodeType |
|---|---|
| 接続 1本 | Endpoint |
| 接続 2本 | Joint |
| 接続 3本+、`isThrough` が2本ある | Diverge |
| 接続 3本+、`isThrough` が2本ない | Intersection |

---

## 2. 遷移モード

2本接続（Joint）時に、部品・幅の違いをどう処理するかを制御する。

```cpp
/// @brief ノードでの遷移方式（Joint 時のみ有効）
enum class NodeTransition : uint8 {
    Blend,    // 部品を滑らかにモーフィング（車線減少・幅変化）
    Abrupt,   // ノード中心で不連続に切替（延伸端・道路種別境界）
};
```

| モード | 用途 | ジオメトリ |
|---|---|---|
| Blend | 4車線→2車線、歩道付き→歩道なし | 遷移ゾーンで部品が滑らかにテーパー |
| Abrupt | 延伸準備構造と既設道路、異なる道路種別の境界 | 各エッジがノード中心まで延長、段差をキャップで閉じる |

Intersection / Diverge ノードでは `transition` は無視される（それぞれ専用ジオメトリを使用）。

---

## 3. データ構造

### 3.1 EdgeAttachment

```cpp
/// @brief エッジのノードへの接続情報
struct EdgeAttachment {
    int   edgeId;
    /// ノード中心からの横方向オフセット [m]
    /// エッジの外向き接線に対して右が正
    float lateralOffset = 0.0f;
    /// Diverge ノード専用: 本線エッジなら true
    bool  isThrough = false;
};
```

### 3.2 RoadNode

```cpp
struct RoadNode {
    int                    id = -1;
    Vec3                   position;
    NodeType               type = NodeType::Endpoint;
    NodeTransition         transition = NodeTransition::Blend;
    Array<EdgeAttachment>  attachments;

    /// @brief 接続エッジID一覧を返すヘルパー（旧 edgeIds 互換）
    Array<int> edgeIds() const {
        Array<int> ids;
        for (const auto& a : attachments) ids << a.edgeId;
        return ids;
    }
};
```

旧 `Array<int> edgeIds` は `attachments` に統合される。

---

## 4. lateralOffset の効果

### 4.1 基本動作

ノードキャップ / 遷移メッシュの生成時に、エッジの接続点を横方向にずらす。

```cpp
// ノード付近でのエッジの左右コーナー算出
Vec3 capCenter = bezier.positionAt(cutoffS);
Vec3 right     = calcRight(bezier.tangentAt(cutoffS));

// 旧: オフセットなし
// leftCorner  = capCenter - right * halfW;
// rightCorner = capCenter + right * halfW;

// 新: lateralOffset を加味
Vec3 shifted    = capCenter + right * attachment.lateralOffset;
Vec3 leftCorner  = shifted - right * halfW;
Vec3 rightCorner = shifted + right * halfW;
```

### 4.2 図解

```
lateralOffset = 0:
  エッジの中心線がノード位置を通る（従来通り）
  ══════╗
       Node
  ══════╝

lateralOffset = +5.0:
  エッジの中心線がノード位置から右に 5m ずれる
                ╔══════
       Node     ║ (+5m)
                ╚══════
```

### 4.3 制約

`lateralOffset` の絶対値はエッジの総幅の半分以下であること。超えるとエッジが重なる。

---

## 5. Endpoint（端点）

### 5.1 概要

接続エッジが1本。道路が終端する。

### 5.2 ジオメトリ

各部品がそれぞれ終端キャップで閉じる。

```
  [Slope][Sidewalk][Curb][Roadbed][Curb][Sidewalk][Slope]|
                                                         |← 端面キャップ
```

- Roadbed: 断面を垂直な面で閉じる
- Sidewalk / Curb: 同上
- Slope: 地形に自然に接続（テーパー）
- cutoff = 0（エッジメッシュはノード位置まで延長）

---

## 6. Joint（継ぎ目）

### 6.1 概要

接続エッジが2本。幅や部品構成が異なる道路を接続する。

### 6.2 Blend モード

遷移ゾーンで部品を滑らかにモーフィングする。

```
Edge A (4車線)          遷移ゾーン           Edge B (2車線)
[Sidewalk][Roadbed 14m][Sidewalk] → [Roadbed 7m]
                        ↕ cutoff    ↕ cutoff
                     ← blendLength →
```

**部品マッチングルール:**

§7.2 の「隣接エッジペア部品マッチング方式」と同じアルゴリズムを使用する（Joint は2本接続の特殊ケース）。

1. 各エッジの部品を中心→外側の順に列挙
2. 同じ `RoadPartType` の部品を順番にペアリング
3. ペアあり: 内側ベジェ〜外側ベジェで帯メッシュを生成
4. ペアなし: 一方の幅を 0 にテーパー

```
Edge A: [Slope, Sidewalk, Curb, Roadbed, Curb, Sidewalk, Slope]
Edge B: [Slope, Roadbed, Slope]

左側マッチング（中心→左端）:
  Roadbed ↔ Roadbed → ペア
  Curb    ↔ (なし)  → テーパーで消滅
  Sidewalk↔ (なし)  → テーパーで消滅
  Slope   ↔ Slope   → ペア
```

**cutoff 計算:**

```cpp
// blendLength = 部品構成の差に応じた遷移距離
float widthDiff = Abs(edgeA.totalPartsWidth() - edgeB.totalPartsWidth());
float blendLength = Max(widthDiff * 2.0f, 10.0f);  // 最低 10m
// 各エッジに blendLength / 2 を cutoff として設定
```

### 6.3 Abrupt モード

各エッジがノード中心まで延長し、不連続に切り替わる。

```
Edge A (既設道路)      Node中心     Edge B (延伸準備 StubEnd)
[Sidewalk][Roadbed]  ---|---  [Roadbed(StubEnd)]
                        |
                   各エッジがここまで延長
```

**ジオメトリ:**

- cutoff ≈ 0.1m（最小限のキャップ厚さ分のみ）
- 各エッジが自分の断面形状をノード中心まで維持
- 幅が異なる場合、広い側の余剰部分を壁面で閉じる:

```
  Edge A (14m)      Edge B (7m)
  ┌────────────┐    ┌──────┐
  │            │    │      │
  │            ├────┤      │
  │            │壁面│      │
  └────────────┘    └──────┘
              ↑
         キャップで段差を埋める
```

---

## 7. Intersection（交差点）/ 共通ノードキャップ描画

### 7.1 概要

接続エッジが3本以上で、`isThrough` が2本揃っていない場合。全エッジが対等に交わる。2本接続（Joint）の場合も同じアルゴリズムの特殊ケースとして処理する。

### 7.2 隣接エッジペア部品マッチング方式

ノードキャップは「隣接エッジペアごとに部品をマッチングし、帯状のフィレット面を生成する」方式で描画する。

#### ステップ1: エッジ情報収集

各接続エッジの cutoff 位置で以下を計算し、角度順にソートする:
- `capTan`: ノード外向きの XZ 正規化接線
- `capCenter`: 切断点の中心座標
- `right`: XZ 直角右ベクトル

#### ステップ2: 隣接ペアごとの部品列挙

角度順で隣接する2本のエッジ (i, i+1) について:

```
エッジ i の「左側部品」= 道路中心 → 左端方向の部品を内側から順に列挙
エッジ i+1 の「右側部品」= 道路中心 → 右端方向の部品を内側から順に列挙

例:
  エッジ i (Arterial) の左側: Roadbed, Curb, Sidewalk, Slope
  エッジ i+1 (LocalRoad) の右側: Roadbed, Slope
```

#### ステップ3: 同種部品のペアリング

左側と右側の部品リストを先頭から順に走査し、同じ `RoadPartType` をペアにする:

```
  Roadbed ↔ Roadbed → ペア（内側ベジェ〜外側ベジェで帯生成）
  Curb    ↔ (なし)  → テーパー（エッジ i 側は本来の幅、i+1 側は幅0）
  Sidewalk↔ (なし)  → テーパー
  Slope   ↔ Slope   → ペア
```

#### ステップ4: 共通輪郭と帯メッシュ生成（2026-09-12）

`JunctionGeometry::build()` が路面・RoadParts・地形切削で共有する輪郭を返す。

- 道路端はエッジと同じ `cutoff - 0.1m` の位置を使い、部品の A/B 端の向きを正規化する。幅は切断位置で補間する。
- 隣接する車道境界の交点を求め、直線の進入部と接線連続の円弧近似ベジェで結ぶ。対向する境界は直線で結び、丁字路の背面が膨らまないようにする。
- 角の半径は歩道等の外側幅を考慮し、外側の帯が反転しない余地を確保する。
- 同種部品を内側から順に対応させ、角の弧長に沿って内外の幅を補間する。部品の OBJ 断面を押し出すため、縁石の立面も維持する。
- 接続先にない部品は幅ゼロへ縮める。縮小先は継続する隣の部品境界とし、側溝に歩道が重ならないようにする。
- 路面は凹輪郭を三角形化する。全端点の凸包で埋める方式は使用しない。
- 折り返し・極端な短辺等で自己交差した輪郭は `Polygon::Correct()` の全領域と進入路支持面を使用して修復する。修復後の車道に入り込む RoadParts は切り取り、欠落数を検査する。
- 鋭角合流で cutoff の先まで隣の車道と重なる場合も、歩道・縁石等を隣接車道の外側に制限する。
- 地表道路は部品の各頂点で地形の高さを取得する。高架はベジェの高さを維持する。エッジと交差点でモデルの高さオフセットを揃える。
- 地形切削も同じ外周を使用する。隣接三角形の結合は和集合が凸の場合だけに限定し、凹角の形は維持する。切削途中で面積ゼロの破片を除去する。
- 交差点の近景・遠景で輪郭を共通にし、LOD 切替による地形との隙間を防ぐ。

`--seed <seed> --capture-roads` は全生成接続のメッシュ欠落を監査し、歩道を持つ幹線の交差点を3か所、近景・上空の6視点で撮影して終了する。

### 7.3 車線区画線の描画

- **Intersection**: 車線区画線は描画しない（交差点内に車線マーキングは不要）
- **Joint**: 車線区画線を描画する（エッジ間で車線をペアリングして接続）
- **Diverge**: 車線区画線を描画する（本線の車線がランプと分岐する様子を表現）

### 7.4 B端・flip の処理

- B端接続エッジは接線を反転（`-tangent`）するため、部品の左右が入れ替わる
- `shouldFlipOffsets` で正規方向を判定し、部品 offset を反転して一貫した左右を保つ

---

## 8. Diverge（分岐合流）

### 8.1 概要

接続エッジが3本以上で、`isThrough == true` のエッジがちょうど2本ある。本線が途切れずに通過し、ランプが側面から剥離/合流する。

```
                    Node
                     |
  ──── Edge A ──────[=]────── Edge B ────   ← 本線（isThrough=true × 2）
                     ╲
                      ╲──── Edge C ────     ← ランプ（isThrough=false）
```

### 8.2 構成ルール

| 要素 | 条件 |
|---|---|
| `isThrough == true` | ちょうど 2本（本線の前後） |
| `isThrough == false` | 1本以上（ランプ） |
| 合計 | 3本以上 |

### 8.3 ジオメトリ（3段階）

#### 段階1: 本線の接続

through エッジ2本を Joint の Blend と同じ方式で接続する。

- 本線の路面は途切れずに通過
- 車線数が変わる場合（加速/減速車線の出現/消滅）はテーパーで遷移
- Blend の部品マッチングルール（§6.2）を適用

#### 段階2: ランプの剥離

各ランプエッジの inner 端が本線の outer 端に沿って走り、`lateralOffset` の距離だけ離れた位置で独立した道路になる。

```
本線                                ランプ
════════╗                          ╔════════
        ║←── ゴア（三角分離帯）──→║
════════╝                          ╚════════
```

#### 段階3: ゴアエリア（ノーズ）

本線とランプの間の三角形状の分離帯。ノード位置がノーズの先端に対応する。

- 手前: ゼブラ（白線の三角）→ 奥: 物理的な分離帯
- ランプが複数ある場合、隣接ランプ間にもゴアを生成

### 8.4 部品の扱い

| 部品 | 本線側 | ランプ側 |
|---|---|---|
| Roadbed | 途切れずに通過 | ノーズ先端から開始 |
| Shoulder | 分岐点で外側路肩がランプ側に分岐 | ノーズ先端から開始 |
| Guardrail | 分岐直前で途切れ、ノーズ先端にクッションドラム配置 | ノーズから先で開始 |
| Sidewalk | 本線に歩道がない場合は N/A | 同左 |
| Slope | 本線側はそのまま | ランプ側はランプ幅に合わせて生成 |

### 8.5 複数ランプのゴア生成ルール

ランプが複数ある場合:

```
1. ランプを lateralOffset でソートする
2. 本線と最も本線寄りのランプの間にゴアを生成
3. 同じ側のランプ同士の間にもゴアを生成

  本線 ═══════╗          ╔═══ ランプA(offset=+8)
              ║ ゴア1    ║
              ╚════╗ゴア2╔╝
                   ╚═════╝  ランプB(offset=+16)
```

左右両側にランプがある場合、各側で独立にゴアを生成する。

---

## 9. cutoff の自動計算

```cpp
void RoadNetwork::updateNodeCutoffs(int nodeId) {
    const RoadNode* node = getNode(nodeId);
    if (!node) return;

    switch (effectiveNodeType(node)) {
    case NodeType::Endpoint:
        // 端点: cutoff = 0（メッシュはノード位置まで延長）
        break;

    case NodeType::Joint:
        if (node->transition == NodeTransition::Blend) {
            // Blend: 遷移ゾーンの長さ分のマージン
            float widthDiff = /* 両エッジの最大部品幅の差 */;
            float blendHalf = Max(widthDiff, 5.0f);
            // 各エッジに blendHalf を cutoff として設定
        } else {
            // Abrupt: 最小限のマージン
            // cutoff ≈ 0.1m
        }
        break;

    case NodeType::Intersection:
        // 従来のフィレット用マージン計算
        // 最大幅のエッジに基づいて cutoff を算出
        // lateralOffset も考慮して幅を算出
        break;

    case NodeType::Diverge:
        // through エッジ: Blend 相当の cutoff
        // ランプエッジ: ノーズ先端からの距離
        break;
    }
}
```

---

## 10. 具体例

### 例1: 4車線→2車線の車線減少（Joint + Blend）

```
Node:
  type = Joint
  transition = Blend
  attachments:
    { edgeId=20 (4車線側), lateralOffset=0.0, isThrough=false }
    { edgeId=21 (2車線側), lateralOffset=0.0, isThrough=false }

ジオメトリ:
  Roadbed: 14m → 7m にテーパー
  Sidewalk/Curb: テーパーで消滅
  遷移ゾーン長: Max(|14-7|*2, 10) = 14m
```

### 例2: 延伸準備区間との接続（Joint + Abrupt）

```
Node:
  type = Joint
  transition = Abrupt
  attachments:
    { edgeId=30 (既設道路, Built),  lateralOffset=0.0, isThrough=false }
    { edgeId=31 (延伸予定, StubEnd), lateralOffset=0.0, isThrough=false }

ジオメトリ:
  既設道路がノード中心まで描画
  StubEnd 側はバリケード/擁壁で閉じる
  開通時に PlannedChange で transition を Blend に変更可能
```

### 例3: T字路（Intersection）

```
Node:
  type = Intersection  (isThrough が2本ない)
  attachments:
    { edgeId=40 (幹線 左),   lateralOffset=0.0, isThrough=false }
    { edgeId=41 (幹線 右),   lateralOffset=0.0, isThrough=false }
    { edgeId=42 (生活道路),  lateralOffset=0.0, isThrough=false }

ジオメトリ:
  Roadbed: フィレット曲線で接続
  幹線側の歩道: 交差点外縁に回り込む
  生活道路側: 歩道なしのため特別処理なし
```

### 例4: IC ランプ分岐（Diverge, 3本）

```
Node:
  type = Diverge
  attachments:
    { edgeId=10 (本線手前, 5車線), lateralOffset=0.0,  isThrough=true  }
    { edgeId=11 (本線奥, 4車線),   lateralOffset=0.0,  isThrough=true  }
    { edgeId=12 (ランプ, 1車線),   lateralOffset=+8.75, isThrough=false }

ジオメトリ:
  本線: 5車線 → 4車線 にテーパー（Blend、減速車線が消滅）
  ランプ: ノーズ先端（幅0）→ 3.5m に徐々に広がる
  ゴア: テーパーで縮小する余剰幅が三角形の分離帯に
```

### 例5: 左右同時分岐（Diverge, 4本）

```
Node:
  type = Diverge
  attachments:
    { edgeId=10, lateralOffset= 0.0, isThrough=true  }  ← 本線手前
    { edgeId=11, lateralOffset= 0.0, isThrough=true  }  ← 本線奥
    { edgeId=12, lateralOffset=+8.0, isThrough=false }  ← ランプ右
    { edgeId=13, lateralOffset=-8.0, isThrough=false }  ← ランプ左

ジオメトリ:
            ╱ ランプ左(offset=-8)
  ═════════╋═════════  本線(through)
            ╲ ランプ右(offset=+8)
  ゴアエリアが左右に1つずつ生成される
```

### 例6: JCT（Diverge, 5本）

```
Node:
  type = Diverge
  attachments:
    { edgeId=20, lateralOffset= 0.0,  isThrough=true  }
    { edgeId=21, lateralOffset= 0.0,  isThrough=true  }
    { edgeId=22, lateralOffset=+8.0,  isThrough=false }  ← ランプA
    { edgeId=23, lateralOffset=+16.0, isThrough=false }  ← ランプB（さらに外側）
    { edgeId=24, lateralOffset=-8.0,  isThrough=false }  ← ランプC

ジオメトリ:
  本線: through 同士を Blend 接続
  右側: ランプA・B が段階的に剥離、ゴア2つ
  左側: ランプC が剥離、ゴア1つ
```

---

## 11. 設計上の制約と注意点

| ルール | 理由 |
|---|---|
| `lateralOffset` の絶対値はエッジの総幅の半分以下 | オフセットが大きすぎるとエッジが重なる |
| Blend は同じ RoadPartType の部品間でのみ機能する | 異種部品のモーフィングは無意味 |
| Intersection ノードでは `transition` は無視される | 常にフィレット曲線ジオメトリを使用 |
| Diverge ノードでは `isThrough == true` がちょうど2本 | 本線が1本の直通路を形成する必要がある |
| Diverge ノードでは `isThrough == false` が1本以上 | ランプがないと Diverge の意味がない |
| Abrupt ノードではフィレット曲線を生成しない | 各エッジが独立にノード中心まで延長 |
| Endpoint では `lateralOffset` は無視される | 接続先がないのでオフセットに意味がない |

---

## 12. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `07_road_lane_spec.md` | Lane のテーパー（offsetA/B）は Blend 遷移ゾーンでの車線幅変化に対応 |
| `16_road_cross_section_spec.md` | RoadPart の配列が遷移/交差点での部品マッチングの基本単位 |
| `02_technical_spec.md` | RoadNode 構造体の正式定義はここ |
| `08_pathfinding_spec.md` | 経路探索グラフはノード種別に依存しない（Lane の接続で判断） |
| `12_visual_spec.md` | ノードキャップ・遷移メッシュ・ゴアエリアの描画 |

### 2026-09-12: 接続数と標示の補正

Intersection / Diverge の自動cutoffは `max(6m, 最大道路幅/2 + 3m)` とし、各辺長の40%以下へ制限する。2本だけ接続するノードは保存された種別がIntersectionでもBlendの区画線を接続する。線の横位置は通常区間と同じ路端インセットを使い、高さは交差点路面の三角形に合わせる。

### 2026-09-12: 鋭角の多差路と短い接続辺

- 鋭角に重なる進入路では、側帯を交差点の路面だけでなく接続道路の車道全体でも切り抜く。区画線も隣接進入路の車道内へ伸びないよう切り抜く。
- 同方向に近い別進入路がある場合は、その重なりに横断歩道を重ねない。
- 短い道路を統合する際は端点移動量をBezier制御点にも加え、進入方向を維持する。長さと接続情報を再計算する。
- 隣接舗装の三角形を16mセルで索引化し、切り抜く元三角形と境界ボックスが重なる候補のみ処理する。実測した一部道路の高負荷へ対応する。
