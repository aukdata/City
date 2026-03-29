# 道路部品アセット作成ガイド

## 概要

道路の横断面は複数の「道路部品（RoadPart）」から構成されます。各部品は **TOML メタデータ + OBJ 3Dモデル** のペアで定義します。

```
道路中心
  |
  のり面  歩道  縁石  路盤（車線が乗る）  縁石  歩道  のり面
  ←─── 部品の配列（左から右）───→
```

---

## ファイル構成

1つの部品に対して、同名の TOML と OBJ を同じディレクトリに配置します。

```
assets/road_parts/
  roadbed_asphalt.toml   ← メタデータ
  roadbed_asphalt.obj    ← 3Dモデル
  sidewalk_tile.toml
  sidewalk_tile.obj
  sidewalk_tile.png      ← テクスチャ（任意）
  ...
```

---

## 部品タイプ一覧

| type（TOML値） | 説明 |
|---|---|
| `Roadbed` | 路盤（車線が乗る面） |
| `Shoulder` | 路肩 |
| `Curb` | 縁石 |
| `Gutter` | 側溝 |
| `Sidewalk` | 歩道 |
| `BikeLane` | 自転車レーン |
| `Guardrail` | ガードレール |
| `Wall` | 擁壁・橋の欄干 |
| `Slope` | のり面（盛土・切土） |
| `Median` | 中央分離帯 |

部品の並び順や組み合わせは **ゲーム内で自由に変更可能** です。アセット作成時に配置順の制約はありません。同じ部品を複数配置することもできます。

交差点では、隣接する道路の同じタイプの部品同士が自動的にペアになります。ペアがない部品は徐々に幅0にテーパーして消えます。

---

## OBJ モデルの作り方

### 座標軸

```
X = 道路横断方向（幅）
Y = 上方向（高さ）
Z = 道路進行方向（長さ）
```

### 3分割構造

OBJ 内に以下の名前のオブジェクト（`o` 行）を定義します:

```
o inner          ← 道路中心に近い側の端
o center         ← 幅方向に繰り返されるタイル部分
o outer          ← 道路中心から遠い側の端
```

描画時の配置:
```
道路中心側                              道路外側
  |                                       |
  [inner]  [center]  [center]  [outer]
  |        ← 繰り返し →        |
  |←────── 指定した幅 ──────→|
```

- `center` は `model_unit_width`（TOML で指定）の幅を持つタイル
- 部品の幅が `inner幅 + outer幅` より大きい場合、`center` が繰り返されて埋められる
- 幅が端数の場合、`center` は均等にスケールされる

### symmetric モード

`symmetric = true` の場合、`outer` を省略できます。`inner` を X 軸ミラーして `outer` が自動生成されます。左右対称の部品（路盤、縁石など）で便利です。

### LOD（Level of Detail）

遠距離で使用する簡略メッシュを追加できます:

```
o center_lod1    ← LOD レベル 1（800m以遠）
o inner_lod1
o outer_lod1
```

命名規則: `{inner|center|outer}_lod{数字}`

---

## TOML メタデータ

### 必須フィールド

```toml
id = "sidewalk_tile"          # 一意な識別子（ファイル名と合わせる）
name = "タイル歩道"            # 表示名
type = "Sidewalk"             # 部品タイプ（上の表を参照）
model = "sidewalk_tile.obj"   # OBJ ファイル名（同ディレクトリ）
```

### 寸法

```toml
model_unit_width = 0.5   # center メッシュ1個の幅 [m]
model_unit_len = 1.0     # Longitudinal タイリング用: 1タイルの長さ [m]
height_offset = 0.15     # 路面基準からの高低差 [m]（歩道: +0.15）
```

### タイリングモード

```toml
tiling = "CrossSection"  # 断面押し出し方式（ほとんどの部品）
# tiling = "Longitudinal"  # 長手方向タイル方式（ガードレール等）
```

| モード | 説明 | 適する部品 |
|---|---|---|
| `CrossSection` | OBJ の断面形状をベジェ曲線に沿って押し出す | 路盤、歩道、縁石、のり面 |
| `Longitudinal` | OBJ の一定長区間を道路に沿って繰り返し配置 | ガードレール支柱、擁壁ブロック |

### symmetric

```toml
symmetric = true   # outer を inner のミラーで自動生成（左右対称部品用）
# symmetric = false  # inner と outer を個別に定義（非対称部品用）
```

### マテリアル

```toml
[material]
texture = "sidewalk_tile.png"     # テクスチャパス（相対パス可）
color = [0.72, 0.70, 0.68]       # RGB [0.0~1.0]、テクスチャと乗算
```

テクスチャを指定しない場合、`color` のみで単色描画されます。

### LOD 設定

```toml
[lod]
mesh_names = ["center", "center_lod1"]   # OBJ 内のメッシュ名（LOD0, LOD1, ...）
distances = [800.0]                       # LOD 切替距離 [m]
```

---

## 作成例

### 最小構成（平面部品）

```toml
# flat_surface.toml
id = "flat_surface"
name = "平面サーフェス"
type = "Roadbed"
tiling = "CrossSection"
model = "flat_surface.obj"
model_unit_width = 0.5
height_offset = 0.0
symmetric = true

[material]
color = [0.4, 0.4, 0.4]
```

```obj
# flat_surface.obj
# 最小限: inner + center のみ（symmetric=true で outer は自動生成）

o inner
v 0.0 0.0 0.0
v 0.05 0.0 0.0
v 0.05 0.0 1.0
v 0.0 0.0 1.0
vn 0.0 1.0 0.0
vt 0.0 0.0
vt 0.1 0.0
vt 0.1 1.0
vt 0.0 1.0
f 1/1/1 2/2/1 3/3/1 4/4/1

o center
v 0.0 0.0 0.0
v 0.5 0.0 0.0
v 0.5 0.0 1.0
v 0.0 0.0 1.0
vn 0.0 1.0 0.0
vt 0.0 0.0
vt 1.0 0.0
vt 1.0 1.0
vt 0.0 1.0
f 5/5/2 6/6/2 7/7/2 8/8/2
```

### 高さのある部品（歩道）

歩道は路面より 0.15m 高い位置にあり、inner 側に縁石の垂直面を持ちます:

```toml
height_offset = 0.15   # 路面から +0.15m の位置に描画
```

OBJ の inner に縁石の垂直面（Y=0 → Y=0.15）を含めることで、段差を表現できます。

### 非対称部品（ガードレール）

道路側（inner）と外側（outer）で形状が異なる場合:

```toml
symmetric = false
```

OBJ に `inner` と `outer` の両方を定義します。

---

## ワインディング順序

Siv3D (DirectX) は **時計回り（CW）が表面** です。四角面は以下の頂点順序で定義してください:

```
v0 ── v1
|      |
v3 ── v2

f v0 v1 v2 v3   ← 上から見て反時計回り = 表が上
```

---

## チェックリスト

- [ ] TOML と OBJ が同名・同ディレクトリにある
- [ ] OBJ に `o inner` と `o center` が定義されている
- [ ] `symmetric = false` の場合、`o outer` も定義されている
- [ ] 座標軸: X=幅, Y=上, Z=長さ
- [ ] `model_unit_width` が center メッシュの実際の X 幅と一致している
- [ ] `type` が正しい部品タイプ名である
- [ ] テクスチャパスが正しい（相対パスの場合、TOML からの相対）
