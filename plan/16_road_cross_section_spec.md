# 道路断面プロファイル仕様書

## 概要

道路の横断面（cross-section）をスロット列として定義し、ユーザーが歩道・のり面・橋梁欄干などを自由にカスタマイズできるようにする。

現状の `RoadStyle` は路面（車線 + 路肩 + 中央帯）しか表現できないため、これを汎用的なスロットモデルに置き換える。

---

## 1. スロットモデル

道路の断面を「中心線からのオフセットで配置されるスロットの並び」として表現する。

```
中心線
  |
のり面  歩道  縁石  車線  車線  中央帯  車線  車線  縁石  歩道  のり面
          ←  offset で位置を指定  →
```

スロット間に隙間（何もない空間）を設けることもできる。offset + width で次のスロットの開始位置が決まるため、offset を飛ばせば空白が生まれる。

```
  [ 歩道 ]          [ 車線 ][ 車線 ]          [ 歩道 ]
  -8.0  -5.5  -4.0  -3.5    0   3.5  4.0   5.5    8.0
               ^^^^                   ^^^^
               隙間                   隙間
```

---

## 2. データ構造

### 2.1 SlotType

```cpp
enum class SlotType : uint8 {
    Lane,           // 車線（既存の Lane 構造体と 1:1 対応）
    Shoulder,       // 路肩
    Median,         // 中央帯・中央分離帯
    Sidewalk,       // 歩道
    Slope,          // のり面（盛土・切土）
    Guardrail,      // ガードレール・防護柵
    BikeLane,       // 自転車レーン
    Gutter,         // 側溝・排水溝
    Wall,           // 擁壁・橋の欄干
    Curb,           // 縁石
};
```

### 2.2 CrossSectionSlot

```cpp
struct CrossSectionSlot {
    SlotType type;

    // 配置
    float offset;        // 道路中心線からの左端位置 [m]（左がマイナス）
    float width;         // 幅 [m]
    float heightOffset;  // 路面（車線面）からの高低差 [m]

    // 描画
    ColorF color;
    Optional<Texture> texture;
};
```

**heightOffset の解釈:**

| type | heightOffset の意味 |
|------|-------------------|
| Lane, Shoulder, Median, BikeLane | 路面基準からの高低差（通常 0） |
| Sidewalk | 路面からの隆起量（日本の標準: +0.15m） |
| Curb | 縁石上面の路面からの高さ（+0.15m 等） |
| Wall, Guardrail | 構造物の高さ（路面からの上端位置） |
| Slope | **特殊**: 地形高さまで線形に傾斜する（§4 参照） |
| Gutter | 路面からの沈み込み量（-0.05m 等） |

### 2.3 CrossSectionProfile

```cpp
struct CrossSectionProfile {
    String id;            // 一意な識別子（"2lane_sidewalk" 等）
    String name;          // 表示名（"2車線歩道付き" 等）
    Array<CrossSectionSlot> slots;  // 左から右へ並ぶスロット列

    // --- 車線マーキング（Lane スロット間の区画線） ---
    LineMarkStyle laneMarking;      // 同方向車線間の区画線
    LineMarkStyle centerLine;       // 対向車線境界のセンターライン
};
```

**slots の制約:**
- `offset` の昇順にソートされていること
- スロット同士が重なってはならない（`slots[i].offset + slots[i].width <= slots[i+1].offset`）
- `type == Lane` のスロット数は `RoadEdge.lanes` の要素数と一致すること

---

## 3. 既存構造との関係

### 3.1 RoadEdge との統合

```cpp
struct RoadEdge {
    // ... 既存フィールド ...

    String profileId;  // 使用する CrossSectionProfile の ID
                       // 空文字列なら RoadType のデフォルトを使用
};
```

**適用の優先順位:**
1. `edge.profileId` が設定されていればそのプロファイル
2. 未設定なら `RoadStyleRegistry` の RoadType 別デフォルトプロファイル

### 3.2 Lane との対応

`CrossSectionSlot` の `type == Lane` であるスロットが、`RoadEdge.lanes[]` と左から右の順で 1:1 対応する。

```
Profile slots:  [Slope] [Sidewalk] [Curb] [Lane₀] [Lane₁] [Curb] [Sidewalk] [Slope]
                                           ↓        ↓
RoadEdge.lanes:                         lanes[0]  lanes[1]

Lane₀.offset = -3.5,  width = 3.5  →  lanes[0] (Backward)
Lane₁.offset =  0.0,  width = 3.5  →  lanes[1] (Forward)
```

車線の操作軸（dir, op, TempOp, PlannedChange）は従来通り `Lane` 構造体が管理する。プロファイルは**物理的な断面形状のみ**を担当し、交通運用には関与しない。

### 3.3 RoadStyle の移行

現在の `RoadStyle` のフィールドは以下のように移行する:

| 旧 RoadStyle | 移行先 |
|---|---|
| `defaultLaneWidth` | `CrossSectionProfile` の Lane スロットの `width` |
| `shoulderWidth` | Shoulder スロットの `width` |
| `medianWidth` | Median スロットの `width` |
| `laneMarking` | `CrossSectionProfile.laneMarking` |
| `centerLine` | `CrossSectionProfile.centerLine` |
| `surface` | Lane / Shoulder / Median 各スロットの `color` / `texture` |

---

## 4. のり面（Slope）の特殊処理

のり面は道路面と地形面を接続する傾斜部。他のスロットと異なり、`heightOffset` は固定値ではなく地形追従の処理が必要。

### 描画アルゴリズム

```
道路面の高さ: roadHeight（道路の TerrainLift を含む）
地形の高さ:   terrainHeight（World.computeHeight() で取得）

Slope スロットの外側端の高さ = terrainHeight
Slope スロットの内側端の高さ = roadHeight

→ 内側から外側に向かって線形補間で傾斜を描画
```

### 盛土と切土

```
盛土（道路が地形より高い場合）:
  道路面 ─────┐
              │＼  ← のり面
              │  ＼
  地形面 ─────┘

切土（道路が地形より低い場合）:
  地形面 ─────┐
              │／  ← のり面（法面）
              │ ／
  道路面 ─────┘
```

道路面と地形面の高低差が小さい場合（閾値: 0.3m 未満）、のり面は描画しない。

### 橋梁区間

道路面が地形より大幅に高い場合（閾値: 3.0m 以上）、のり面の代わりに橋脚を配置する（§5 参照）。この場合 Slope スロットは描画をスキップする。

---

## 5. 橋梁・高架の扱い

橋梁区間ではのり面が消え、代わりに橋脚・欄干が必要になる。

### 判定

```cpp
float gap = roadHeight - terrainHeight;
if (gap >= 3.0f) {
    // 橋梁区間: Slope スロットを非表示、Wall スロットで欄干を描画
    // 橋脚を一定間隔で配置（§5.1）
}
```

### 5.1 橋脚の配置

橋脚はプロファイルのスロットとしてではなく、`RoadEdge` の弧長に沿って一定間隔（30〜50m）で自動配置する付属構造物として扱う。プロファイルは断面形状のみを定義する。

### 5.2 プロファイルの使い分け

同一の `RoadEdge` でも区間によって橋梁になる場合がある。プロファイル自体は変えず、描画時に各サンプル点で地形との高低差を判定し、Slope の描画/非描画を切り替える。

---

## 6. プロファイルの定義例

### 6.1 一般道（2車線・歩道付き）

```toml
id = "local_2lane_sidewalk"
name = "2車線歩道付き"

[[slots]]
type = "Slope"
offset = -7.7
width = 2.0
heightOffset = 0.0

[[slots]]
type = "Sidewalk"
offset = -5.7
width = 2.5
heightOffset = 0.15

[[slots]]
type = "Curb"
offset = -3.2
width = 0.2
heightOffset = 0.15

[[slots]]
type = "Lane"
offset = -3.0
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = 0.0
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Curb"
offset = 3.0
width = 0.2
heightOffset = 0.15

[[slots]]
type = "Sidewalk"
offset = 3.2
width = 2.5
heightOffset = 0.15

[[slots]]
type = "Slope"
offset = 5.7
width = 2.0
heightOffset = 0.0
```

### 6.2 バイパス（4車線・中央分離帯・路肩）

```toml
id = "bypass_4lane"
name = "4車線バイパス"

[[slots]]
type = "Slope"
offset = -11.5
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Guardrail"
offset = -8.5
width = 0.5
heightOffset = 0.8

[[slots]]
type = "Shoulder"
offset = -8.0
width = 1.0
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = -7.0
width = 3.5
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = -3.5
width = 3.5
heightOffset = 0.0

[[slots]]
type = "Median"
offset = 0.0
width = 2.0
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = 2.0
width = 3.5
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = 5.5
width = 3.5
heightOffset = 0.0

[[slots]]
type = "Shoulder"
offset = 9.0
width = 1.0
heightOffset = 0.0

[[slots]]
type = "Guardrail"
offset = 10.0
width = 0.5
heightOffset = 0.8

[[slots]]
type = "Slope"
offset = 10.5
width = 3.0
heightOffset = 0.0
```

### 6.3 橋梁区間（2車線）

```toml
id = "bridge_2lane"
name = "2車線橋梁"

[[slots]]
type = "Wall"
offset = -4.5
width = 0.5
heightOffset = 1.2

[[slots]]
type = "Sidewalk"
offset = -4.0
width = 1.0
heightOffset = 0.15

[[slots]]
type = "Lane"
offset = -3.0
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = 0.0
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Sidewalk"
offset = 3.0
width = 1.0
heightOffset = 0.15

[[slots]]
type = "Wall"
offset = 4.0
width = 0.5
heightOffset = 1.2
```

### 6.4 路面電車付き道路

```toml
id = "tram_4lane"
name = "4車線路面電車付き"

[[slots]]
type = "Sidewalk"
offset = -10.0
width = 2.5
heightOffset = 0.15

[[slots]]
type = "Curb"
offset = -7.5
width = 0.2
heightOffset = 0.15

[[slots]]
type = "Lane"
offset = -7.3
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = -4.3
width = 3.0
heightOffset = 0.0

# 中央に軌道用の空間（スロットなし = 隙間）
# offset -1.3 ~ +1.3 は空白 → 軌道は別システムで描画

[[slots]]
type = "Lane"
offset = 1.3
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Lane"
offset = 4.3
width = 3.0
heightOffset = 0.0

[[slots]]
type = "Curb"
offset = 7.3
width = 0.2
heightOffset = 0.15

[[slots]]
type = "Sidewalk"
offset = 7.5
width = 2.5
heightOffset = 0.15
```

---

## 7. プロファイル管理

### 7.1 プリセット

RoadType ごとにデフォルトプロファイルを用意する。

| RoadType | デフォルトプロファイル |
|---|---|
| LocalRoad | `local_2lane`（路肩のみ、歩道なし） |
| Arterial | `arterial_2lane_sidewalk`（歩道付き） |
| Expressway | `expressway_4lane`（中央分離帯・ガードレール） |
| Highway | `highway_4lane`（中央分離帯・路肩広め） |

### 7.2 ユーザーカスタムプロファイル

ユーザーは以下の方法でプロファイルを作成・編集できる:

1. **プリセットからの複製・編集**: 既存プロファイルを複製してスロットを追加・削除・幅変更
2. **断面エディタ UI**: 横断面図を表示し、スロットをドラッグ操作で編集（§8 参照）
3. **TOML ファイルの直接編集**: `App/assets/profiles/` に配置

カスタムプロファイルは `App/assets/profiles/custom/` に保存され、セーブデータにも含まれる。

### 7.3 プロファイルの適用

道路建設時のフロー:

1. RoadType を選択 → デフォルトプロファイルが適用される
2. ユーザーが任意でプロファイルを変更できる（道路計画モード内）
3. 既存道路のプロファイルも後から変更可能（工事期間を伴う）

---

## 8. 断面エディタ UI

### 8.1 表示

道路を横から見た断面図を2Dで表示する。

```
  ┌─────────────────────────────────┐
  │    ╱  歩道  ┃  車線  車線  ┃ 歩道  ╲    │
  │   ╱        ┃             ┃       ╲   │
  │──╱─────────┸─────────────┸────────╲──│ ← 地形面
  └─────────────────────────────────┘
       Slope   Curb  Lane  Lane  Curb   Slope

  [+ スロット追加]  [プリセット▼]  [保存]
```

### 8.2 操作

- **スロットの追加**: 種別を選んで配置位置をクリック
- **スロットの削除**: 右クリック → 削除
- **幅の変更**: スロット端をドラッグ
- **位置の変更**: スロット中央をドラッグ（offset 変更）
- **高さの変更**: 上下ドラッグ（heightOffset 変更）
- **プロパティ編集**: スロットをダブルクリック → 詳細パネル（色・テクスチャ等）

---

## 9. 描画への影響

### 9.1 メッシュ生成

現在の `RoadRenderer` は `totalWidth()` で一枚の帯メッシュを生成している。プロファイル導入後は、スロットごとに個別の帯メッシュを生成する。

```
現在:  [ ←── 一枚の帯メッシュ ──→ ]
変更後: [Slope][Sidewalk][Curb][Lane][Lane][Curb][Sidewalk][Slope]
         各スロットが独立した帯メッシュ
```

**利点:**
- スロットごとに異なるテクスチャ・色・高さを適用できる
- 隙間（空白区間）を自然に表現できる

### 9.2 交差点の処理

交差点（ノードキャップ）では、Lane スロットのみをフィレット曲線で接続し、歩道・のり面は交差点の外縁に沿って別途処理する。詳細は実装時に検討。

---

## 10. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `07_road_lane_spec.md` | Lane 構造体の物理軸・運用軸はそのまま維持。プロファイルは物理的な断面形状のみ担当 |
| `12_visual_spec.md` | 道路描画の詳細。メッシュ生成をスロットベースに変更 |
| `04_gameplay_detail_spec.md` | 建設コスト。プロファイルの複雑さに応じたコスト計算が必要 |
| `08_pathfinding_spec.md` | 経路探索には影響なし（Lane の操作軸で判断するため） |
| `14_save_spec.md` | カスタムプロファイルの永続化が必要 |
