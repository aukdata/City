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

1. 同じ `RoadPartType` の部品をペアにする（offset が近いもの優先）
2. ペアが見つかった部品: 幅・offset を線形補間でモーフィング
3. ペアが見つからない部品: 遷移ゾーン内で幅を 0 にテーパー（消滅）

```
Edge A: [Slope, Sidewalk, Curb, Roadbed, Curb, Sidewalk, Slope]
Edge B: [Slope, Roadbed, Slope]

マッチング結果:
  Roadbed ↔ Roadbed: 14m → 7m テーパー
  Slope   ↔ Slope:   幅を補間
  Sidewalk → (なし): テーパーで消滅
  Curb    → (なし): テーパーで消滅
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

## 7. Intersection（交差点）

### 7.1 概要

接続エッジが3本以上で、`isThrough` が2本揃っていない場合。全エッジが対等に交わる。

### 7.2 ジオメトリ

既存のフィレット曲線方式を拡張する。

```
1. Roadbed 部品のみ:
   各エッジの Roadbed の左右コーナーを算出（lateralOffset 加味）
   → 隣接エッジ間をフィレット曲線で接続
   → 中心をファン三角形で埋める

2. Sidewalk / Curb:
   交差点の外縁に沿って「歩道島」を生成
   各フィレット曲線の外側に歩道面を配置

3. Guardrail / Wall:
   交差点の手前で終端（キャップで閉じる）

4. Slope:
   交差点外周から地形面に接続
```

### 7.3 フィレット曲線生成（既存ロジックの拡張）

```cpp
// 各エッジの切断点情報を収集（lateralOffset を反映）
struct EdgeInfo {
    Vec3   capTan;       // ノードから外向きの接線
    Vec3   leftCorner;   // 切断点左端（lateralOffset 加味）
    Vec3   rightCorner;  // 切断点右端（lateralOffset 加味）
    double angle;        // XZ 平面の角度（ソート用）
};

// 隣接エッジ間のフィレット:
// infos[i].leftCorner → infos[next].rightCorner を3次ベジェで接続
```

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
