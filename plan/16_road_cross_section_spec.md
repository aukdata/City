# 道路部品・断面構成仕様書

## 概要

道路の横断面を**道路部品（RoadPart）の配列**として構成する。各部品は OBJ 形式の 3D モデルを持ち、幅方向のタイリングにより任意の幅に対応する。

車線（Lane）は部品とは独立した配列として RoadEdge に所属し、路盤の上に論理的に配置される。物理構造（部品）と交通運用（車線）の完全な分離を実現する。

```
道路中心
  |
  のり面  歩道  縁石  路盤（車線が乗る）  縁石  歩道  のり面
  ←───  RoadPart の配列（左から右）  ───→
                    ↑
            Lane の配列（路盤上に論理配置）
```

---

## 1. 部品モデルの構造

### 1.1 幅方向タイリング（3分割モデル）

各部品の OBJ モデルは、幅方向に3つのメッシュで構成される:

```
道路中心側                              道路外側
  |                                       |
  [inner]  [center]  [center]  [outer]
  |        ← 繰り返し →        |
  |←────── 指定した幅 ──────→|
```

- **inner**: 道路中心に近い側の端メッシュ
- **center**: 幅方向に繰り返すメッシュ
- **outer**: 道路中心から遠い側の端メッシュ

`center` を必要な回数繰り返し、両端を `inner` / `outer` で閉じることで、任意の幅に対応する。

### 1.2 長手方向の処理

OBJ モデルは短い区間（断面テンプレート）として使用する。長手方向は**既存のベジェサンプリング + 帯メッシュ生成パイプライン**で処理する:

1. OBJ から断面の頂点列（XY 平面の形状）を抽出
2. inner + center×N + outer で目標幅の断面を構成
3. ベジェ曲線に沿ってサンプリングし、各サンプル点に断面を配置
4. 隣接サンプル点間を帯メッシュで接続

ただし、ガードレール等の3D構造物は `TilingMode::Longitudinal` で長手方向にタイル配置する（§1.4 参照）。

### 1.3 OBJ の座標軸規約

```
X = 道路横断方向（幅）  ← inner/center/outer のタイリング方向
Y = 上方向（高さ）
Z = 道路進行方向（長さ）  ← 帯メッシュ押し出し方向
```

### 1.4 タイリングモード

| モード | 説明 | 適するパーツ |
|---|---|---|
| `CrossSection` | 断面押し出し方式。OBJ の断面形状をベジェ曲線に沿って押し出す | 路盤、歩道、中央分離帯、縁石 |
| `Longitudinal` | 長手方向タイル方式。OBJ モデルの一定長区間をカーブに沿って繰り返し配置 | ガードレール支柱、擁壁ブロック |

### 1.5 幅の端数処理

指定幅が center メッシュの整数倍にならない場合、**全 center を均等スケール**する。UV の歪みは微小であり、見た目の均一性を優先する。

---

## 2. データ構造

### 2.1 RoadPartType

```cpp
enum class RoadPartType : uint8 {
    Roadbed,        // 路盤（車線が乗る面）
    Shoulder,       // 路肩
    Median,         // 中央分離帯
    Sidewalk,       // 歩道
    Gutter,         // 側溝
    Guardrail,      // ガードレール・防護柵
    Wall,           // 擁壁・橋の欄干
    Curb,           // 縁石
    Slope,          // のり面（盛土・切土）
    BikeLane,       // 自転車レーン
};
```

### 2.2 TilingMode

```cpp
enum class TilingMode : uint8 {
    CrossSection,   // 断面押し出し方式（路盤・歩道等）
    Longitudinal,   // 長手方向タイル方式（ガードレール等）
};
```

### 2.3 RoadPartDef（部品定義 = アセット）

`assets/road_parts/` に配置される部品アセットの定義。TOML + OBJ のペアで1部品。

```cpp
struct RoadPartDef {
    String         id;              // 一意な識別子（"sidewalk_standard" 等）
    String         name;            // 表示名（"標準歩道" 等）
    RoadPartType   type;            // 部品種別
    TilingMode     tiling;          // タイリングモード
    float          modelUnitWidth;  // center メッシュ1個の幅 [m]
    float          modelUnitLen;    // Longitudinal 用: 1タイルの長さ [m]
    float          heightOffset;    // 路面基準からの高低差 [m]
    bool           symmetric;       // true: outer を省略し inner をミラー
    String         modelPath;       // OBJ ファイルパス
    String         texturePath;     // テクスチャパス
    ColorF         color;           // 基本色
    Array<String>  lodMeshNames;    // LOD段階のメッシュ名 ["center", "center_lod1"]
    Array<float>   lodDistances;    // LOD切替距離 [m] [800.0]
};
```

### 2.4 RoadPart（道路上のインスタンス）

```cpp
struct RoadPart {
    String     defId;     // RoadPartDef への参照
    float      width;     // この部品の幅 [m]
    float      offset;    // 道路中心線からの左端位置 [m]（左がマイナス）
    BuildState build;     // 建設状態（NotBuilt / UnderConstruction / Built / StubEnd）
};
```

BuildState は `07_road_lane_spec.md` で定義する列挙型を共用する。

---

## 3. OBJ メッシュ命名規約

### 3.1 基本命名

```
o inner            # 道路中心に近い側の端メッシュ（LOD0）
o outer            # 道路中心から遠い側の端メッシュ（LOD0）
o center           # 幅方向に繰り返すメッシュ（LOD0）
o inner_lod1       # LOD1
o outer_lod1       # LOD1
o center_lod1      # LOD1
```

`symmetric = true` の場合、`outer` / `outer_lod1` は省略可能。描画時に `inner` を X 軸ミラーして使用する。

### 3.2 配置イメージ

```
道路中心
  |
  |   [inner] [center] [center] [center] [outer]
  |   ← 中心側                        外側 →
```

中央分離帯のように道路中心を跨ぐパーツでは、inner が左側、outer が右側になるが、`offset` で配置位置が決まるので問題ない。

### 3.3 Siv3D での読み込み

Siv3D の `Model` クラスは GPU 静的メッシュのため頂点変形ができない。OBJ を自前パースして `MeshData`（`Array<Vertex3D>` + `Array<TriangleIndex32>`）として保持し、メッシュ名で inner/outer/center を識別する。

```cpp
// OBJ パース後のデータ
struct PartModelData {
    String name;                     // メッシュ名
    Array<Vertex3D> vertices;
    Array<TriangleIndex32> indices;
};

struct RoadPartModel {
    PartModelData inner;
    PartModelData center;
    PartModelData outer;             // symmetric 時は空
    // LOD 段階ごとに同構造
    Array<RoadPartModel> lods;       // lods[0] = LOD1, lods[1] = LOD2, ...
};
```

---

## 4. TOML メタデータ

### 4.1 ファイル配置

```
App/assets/road_parts/
  sidewalk_standard.toml
  sidewalk_standard.obj
  sidewalk_standard.png
  guardrail_steel.toml
  guardrail_steel.obj
  ...
```

TOML ファイルと OBJ ファイルは同名・同ディレクトリに配置する。

### 4.2 フォーマット

```toml
id = "sidewalk_standard"
name = "標準歩道"
type = "Sidewalk"
tiling = "CrossSection"
model = "sidewalk_standard.obj"
model_unit_width = 0.5        # center 1個の幅 [m]
model_unit_len = 1.0          # Longitudinal 用（CrossSection では参考値）
height_offset = 0.15          # 路面からの高低差 [m]
symmetric = true              # outer を inner のミラーで生成

[material]
texture = "sidewalk_tile.png"
color = [0.7, 0.7, 0.7]

[lod]
mesh_names = ["center", "center_lod1"]
distances = [800.0]           # LOD 切替距離 [m]
```

### 4.3 ガードレールの例（Longitudinal）

```toml
id = "guardrail_steel"
name = "鋼製ガードレール"
type = "Guardrail"
tiling = "Longitudinal"
model = "guardrail_steel.obj"
model_unit_width = 0.3
model_unit_len = 4.0          # 1タイル = 4m（支柱間隔）
height_offset = 0.0
symmetric = false             # inner（道路側）と outer（外側）は非対称

[material]
texture = "guardrail.png"
color = [0.8, 0.8, 0.8]

[lod]
mesh_names = ["center", "center_lod1"]
distances = [400.0]
```

---

## 5. のり面（Slope）の特殊処理

のり面は道路面と地形面を接続する傾斜部。他の部品と異なり、高さが地形に追従する。

### 5.1 描画アルゴリズム

```
道路面の高さ: roadHeight（道路の TerrainLift を含む）
地形の高さ:   terrainHeight（World.computeHeight() で取得）

Slope 部品の内側端の高さ = roadHeight
Slope 部品の外側端の高さ = terrainHeight

→ 内側から外側に向かって線形補間で傾斜を描画
```

### 5.2 盛土と切土

```
盛土（道路が地形より高い場合）:
  道路面 ─────┐
              |＼  ← のり面
              |  ＼
  地形面 ─────┘

切土（道路が地形より低い場合）:
  地形面 ─────┐
              |／  ← のり面（法面）
              | ／
  道路面 ─────┘
```

- 高低差 0.3m 未満: のり面を描画しない
- 高低差 3.0m 以上: のり面の代わりに橋脚を配置（§6 参照）

### 5.3 OBJ モデルとの関係

のり面はモデルの断面形状を使いつつ、outer 端の Y 座標を地形高さに動的に調整する。inner 端は roadHeight に固定。

---

## 6. 橋梁・高架の扱い

### 6.1 判定

```cpp
float gap = roadHeight - terrainHeight;
if (gap >= 3.0f) {
    // 橋梁区間: Slope 部品を非表示、Wall 部品で欄干を描画
    // 橋脚を一定間隔で配置
}
```

### 6.2 橋脚の配置

橋脚は部品配列の要素ではなく、`RoadEdge` の弧長に沿って一定間隔（30〜50m）で自動配置する付属構造物として扱う。

### 6.3 区間による自動切替

同一の `RoadEdge` でも区間によって橋梁になる場合がある。部品配列自体は変えず、描画時に各サンプル点で地形との高低差を判定し、Slope の描画/非描画を切り替える。

---

## 7. プリセットプロファイル

### 7.1 RoadType 別デフォルト構成

RoadType ごとにデフォルトの部品配列を定義する。

| RoadType | デフォルト構成 |
|---|---|
| LocalRoad | Slope + Roadbed + Slope |
| Arterial | Slope + Sidewalk + Curb + Roadbed + Curb + Sidewalk + Slope |
| Expressway | Slope + Guardrail + Shoulder + Roadbed + Median + Roadbed + Shoulder + Guardrail + Slope |
| Highway | Slope + Guardrail + Shoulder + Roadbed + Median + Roadbed + Shoulder + Guardrail + Slope |

### 7.2 プリセット定義例

#### 一般道（2車線・歩道付き）

```
部品配列（左から右）:
  Slope        offset=-7.7  width=2.0
  Sidewalk     offset=-5.7  width=2.5  heightOffset=0.15
  Curb         offset=-3.2  width=0.2  heightOffset=0.15
  Roadbed      offset=-3.0  width=6.0  heightOffset=0.0
  Curb         offset= 3.0  width=0.2  heightOffset=0.15
  Sidewalk     offset= 3.2  width=2.5  heightOffset=0.15
  Slope        offset= 5.7  width=2.0

車線配列:
  Lane0  offsetA_L=-3.0  offsetA_R=0.0  dir=Backward
  Lane1  offsetA_L= 0.0  offsetA_R=3.0  dir=Forward
```

#### バイパス（4車線・中央分離帯）

```
部品配列:
  Slope        offset=-11.5 width=3.0
  Guardrail    offset=-8.5  width=0.5
  Shoulder     offset=-8.0  width=1.0
  Roadbed      offset=-7.0  width=7.0
  Median       offset= 0.0  width=2.0
  Roadbed      offset= 2.0  width=7.0
  Shoulder     offset= 9.0  width=1.0
  Guardrail    offset= 10.0 width=0.5
  Slope        offset= 10.5 width=3.0

車線配列:
  Lane0  offsetA_L=-7.0  offsetA_R=-3.5  dir=Backward
  Lane1  offsetA_L=-3.5  offsetA_R= 0.0  dir=Backward
  Lane2  offsetA_L= 2.0  offsetA_R= 5.5  dir=Forward
  Lane3  offsetA_L= 5.5  offsetA_R= 9.0  dir=Forward
```

#### 橋梁区間（2車線）

```
部品配列:
  Wall         offset=-4.5  width=0.5  heightOffset=1.2
  Sidewalk     offset=-4.0  width=1.0  heightOffset=0.15
  Roadbed      offset=-3.0  width=6.0
  Sidewalk     offset= 3.0  width=1.0  heightOffset=0.15
  Wall         offset= 4.0  width=0.5  heightOffset=1.2
```

### 7.3 ユーザーカスタム

ユーザーは以下の方法で部品構成を編集できる:

1. **プリセットからの複製・編集**: 既存構成を複製して部品を追加・削除・幅変更
2. **断面エディタ UI**: 横断面図を表示し、部品をドラッグ操作で編集（§8 参照）
3. **TOML ファイルの直接編集**: `App/assets/profiles/` に配置

カスタムプロファイルは `App/assets/profiles/custom/` に保存され、セーブデータにも含まれる。

### 7.4 プロファイルの適用

道路建設時のフロー:

1. RoadType を選択 → デフォルトの部品構成が適用される
2. ユーザーが任意で部品を追加・削除・幅変更できる（道路計画モード内）
3. 既存道路の部品構成も後から変更可能（工事期間を伴う）

---

## 8. 断面エディタ UI

### 8.1 表示

道路を横から見た断面図を2Dで表示する。

```
  ┌─────────────────────────────────────┐
  |    ╱  歩道  ┃  路盤（車線×2） ┃ 歩道  ╲    |
  |   ╱        ┃                 ┃       ╲   |
  |──╱─────────┸─────────────────┸────────╲──| ← 地形面
  └─────────────────────────────────────┘
       Slope   Curb    Roadbed    Curb   Slope

  [+ 部品追加]  [プリセット▼]  [保存]
```

### 8.2 操作

- **部品の追加**: 種別を選んで配置位置をクリック
- **部品の削除**: 右クリック → 削除
- **幅の変更**: 部品端をドラッグ
- **位置の変更**: 部品中央をドラッグ（offset 変更）
- **高さの変更**: 上下ドラッグ（heightOffset 変更）
- **モデルの変更**: 部品をダブルクリック → 使用する RoadPartDef の選択

---

## 9. 描画パイプライン

### 9.1 メッシュ生成

RoadPart ごとに独立した帯メッシュを生成する。

```
1. RoadPart ごとに:
   a. RoadPartDef から OBJ の断面頂点列を取得
   b. inner + center × N + outer で目標幅の断面を構成
   c. ベジェ曲線に沿ってサンプリング、各サンプル点で断面を配置
   d. 隣接サンプル点間を帯メッシュで接続
   e. Mesh にアップロード、エッジ ID でキャッシュ

2. Lane の区画線は路盤メッシュの上に別メッシュとして生成
   （詳細は 07_road_lane_spec.md §3 参照）
```

### 9.2 LOD

部品モデルの LOD 段階に加え、ベジェ曲線のサンプリング密度も距離で調整する:

- 近距離 (<800m): 詳細メッシュ + 高サンプリング
- 遠距離 (>=800m): LOD メッシュ + 低サンプリング（1/4）

### 9.3 ノード接続の処理

ノードでの部品の接続（継ぎ目の遷移・交差点のフィレット・分岐合流のゴア）は `17_road_node_spec.md` で定義する。描画パイプラインはノード種別に応じて以下を生成する:

- **Joint (Blend)**: 遷移メッシュ（部品の幅・offset を線形補間）
- **Joint (Abrupt)**: キャップメッシュ（段差の壁面）
- **Intersection**: フィレット曲線メッシュ（Roadbed のみ）+ 歩道島
- **Diverge**: 本線の Blend 遷移 + ランプ剥離メッシュ + ゴアエリア

---

## 10. RoadStyle の移行

現在の `RoadStyle` のフィールドは以下のように移行する:

| 旧 RoadStyle | 移行先 |
|---|---|
| `defaultLaneWidth` | Lane の `nominalWidth`（`07_road_lane_spec.md`） |
| `shoulderWidth` | Shoulder 部品の `width` |
| `medianWidth` | Median 部品の `width` |
| `laneMarking` | Lane の `lineLeft` / `lineRight`（`07_road_lane_spec.md`） |
| `centerLine` | Lane の `lineLeft` / `lineRight`（`07_road_lane_spec.md`） |
| `surface` | Roadbed 部品の RoadPartDef の `color` / `texturePath` |

---

## 11. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `07_road_lane_spec.md` | Lane 構造体は運用軸のみ担当。BuildState は RoadPart に所属。RoadEdge が parts と lanes の両配列を持つ |
| `17_road_node_spec.md` | ノードでの部品接続（遷移・フィレット・ゴア）の詳細ジオメトリ |
| `12_visual_spec.md` | 道路描画の詳細。メッシュ生成を部品ベースに変更 |
| `04_gameplay_detail_spec.md` | 建設コスト。部品の種類・数に応じたコスト計算が必要 |
| `08_pathfinding_spec.md` | 経路探索には影響なし（Lane の運用軸で判断するため） |
| `14_save_spec.md` | カスタムプロファイル・部品配列の永続化が必要 |

### 2026-09-12: OBJの空白処理

OBJのv/vt/vn/f行は連続する空白とタブを区切り文字として処理し、空トークンを座標0として読まない。縁石モデルの全8面が面積を持つことを常設テストで確認する。三角形状の欠けはこの読み込み不良によるもので、モデルの埋め合わせでは修正しない。


2026-09-13 追記: 道路の水平断面・掘割・盛土・ガードレール、河川、立体ブーリアン坑口、道路/線路上の歩行とCtrl加速は [26_landscape_transport_spec.md](26_landscape_transport_spec.md) の修正仕様に従う。

## 鉄道との共通化（2026-09-17）

路盤の上の `Lane` は車道だけでなく `LaneType::Rail` を扱う。バラスト・スラブも `Roadbed` 部品であり、新しい構造種別は増やさない。`TransportMode` によって車と列車の経路を分離し、併用断面では両者が同じエッジの異なる車線を使う。路盤幅、地形との境界、橋脚、保存、断面編集は道路と共通。詳しい寸法・生成制約・運行は [鉄道仕様](10_railway_spec.md#共通路盤と軌道車線) を参照。
