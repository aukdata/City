# Siv3D 0.6.16 実装留意点

コンパイルエラーや API の使い方で詰まったときに参照すること。

---

## Ray / カメラ

- `BasicCamera3D::screenToRay(Vec2)` → `Ray`
- `Ray::origin` / `Ray::direction` は **`SIMD_Float4` 型**（`.x`/`.y`/`.z` は**存在しない**）
  - 成分取得: `getX()` / `getY()` / `getZ()` / `getW()`、または `xyz()` (→ `Float3`)、または `elem(size_t i)`
- `Ray::intersectsAt(const InfinitePlane&)` → `Optional<Float3>`
- `Ray::intersectsAt(const Plane&)` → `Optional<Float3>`

### 地面（y=0）との交点

```cpp
const Ray ray = m_camera.screenToRay(screenPos);
if (const auto hit = ray.intersectsAt(InfinitePlane{ Float3{ 0, 1, 0 }, Float3{ 0, 0, 0 } }))
    return Vec3{ *hit };
```

### InfinitePlane コンストラクタ

```cpp
// (法線: Float3, 通過点: Float3)
InfinitePlane{ Float3{ 0, 1, 0 }, Float3{ 0, 0, 0 } }  // y=0 の地面平面
```

---

## 型変換

| 変換 | 方法 |
|---|---|
| `Float3` → `Vec3` | `Vec3{ float3_val }` （テンプレートコンストラクタ経由） |
| `Vec3` (double) → `Float3` | `Float3{ vec3_val }` |
| `Vec3::Up()` は double ベース | `Float3` が必要な場合は `Float3{ 0, 1, 0 }` を使う |

---

## Cylinder

```cpp
// 2点間シリンダー（デバッグ線描画に便利）
Cylinder{ Vec3 from, Vec3 to, double r }.draw(color);

// 中心・半径・高さ・向き
Cylinder{ Vec3 center, double r, double h, Quaternion q }.draw(color);
```

## Quaternion

```cpp
// 軸・角度から生成（軸は Float3）
Quaternion q = Quaternion::RotationAxis(Float3{ axis }, double angle);

// Vec3 → Float3 変換が必要
Float3 axis = Float3{ vec3_val };
```

## worldToScreenPoint

```cpp
// BasicCamera3D::worldToScreenPoint(const Float3&) → Float3
// x, y = スクリーン座標、z > 0 でカメラ前方
const Float3 sp = camera.camera3D().worldToScreenPoint(Float3{ worldPos });
if (sp.z > 0.0f)
    font(U"text").draw(Vec2{ sp.x, sp.y }, color);
```

## キー定数

```cpp
KeySlash   // '/' キー (0xBF)
KeyF3      // F3 キー
// F3+X のコード検出パターン
if (KeyF3.pressed() && KeyN.down()) { /* F3+N */ }
```

## SIMD_Float4

`Ray::origin` / `Ray::direction` などで使われる内部型。

```cpp
SIMD_Float4 v = ...;
float x = v.getX();
float y = v.getY();
float z = v.getZ();
Float3 xyz = v.xyz();
float elem0 = v.elem(0);
```

---

## よく使う静的メンバ

```cpp
Vec3::Up()    // {0, 1, 0}  double ベース
Vec3::Right() // {1, 0, 0}  double ベース
Vec3::Zero()  // {0, 0, 0}  double ベース
```

---

## 三角形ワインディング順序

Siv3D の 3D メッシュは **反時計回り (CCW) = 表面** (上方 +Y から見て CCW が見える面)。

```cpp
// 地面上の quad (XZ 平面): 表面が上向き
// v0=(x0,z0), v1=(x1,z0), v2=(x0,z1), v3=(x1,z1) として
TriangleIndex32{ v0, v1, v2 };  // CCW ✓
TriangleIndex32{ v1, v3, v2 };  // CCW ✓

// 道路ポリゴン帯 (iL=左, iR=右, 0=現在, 1=次セグメント)
TriangleIndex32{ iL0, iL1, iR0 };  // CCW ✓
TriangleIndex32{ iR0, iL1, iR1 };  // CCW ✓
// NG 例: { iL0, iR0, iL1 } は CW = 裏面 → カリングで不可視
```

## カメラ移動方向

`yaw=0` でカメラは `focus` の +Z 方向にいる（eye.z > focus.z）。
W キーで視線方向（-Z）へ進むため、`forward` は **`{-sin(yaw), 0, -cos(yaw)}`**。
`{+sin(yaw), 0, +cos(yaw)}` は逆方向になる。

## Grid / Array

- `Grid<T>` の添字: `grid[{ col, row }]` = `grid[{ x, y }]`
- `Array<T>` は `std::vector<T>` ラッパー。`resize()` / `reserve()` / `isEmpty()` 使用可
- `Array<T>` のサイズを `Random(int, int)` に渡すときは `static_cast<int>(arr.size())` で変換すること

---

---

## Sky（大気・空）

```cpp
// GameApp メンバ
Sky m_sky;

// render() の先頭で SetCameraTransform を呼んでからSkyを描画する
// （Sky::draw は現在のカメラ状態を参照するため）
Graphics3D::SetCameraTransform(m_camera.camera3D());

// 太陽方向（昼夜サイクルに応じて動かす例）
float t = (hour - 6.0f) * Pi / 12.0f;          // 6時=日出, 18時=日没
Graphics3D::SetSunDirection(Vec3{ cos(t), sin(t), 0.3 }.normalized());
Graphics3D::SetGlobalAmbientColor(ColorF{ ambientBrightness });

// 空の色
m_sky.zenithColor  = nightZenith.lerp(dawnZenith, dawnF).lerp(dayZenith, dayF);
m_sky.horizonColor = nightHorizon.lerp(dawnHorizon, dawnF).lerp(dayHorizon, dayF);
m_sky.starBrightness = Clamp(1.0 - dayF * 3.0 - dawnF * 2.0, 0.0, 1.0);
m_sky.cloudTime = Scene::Time() * 0.015;  // 雲を動かす
m_sky.draw(exposure);                     // exposure: 夜0.15〜昼1.0 程度
```

- `Sky` のデフォルトコンストラクタは `Sky(double sphereRadius = 8000.0)`
- `Graphics3D::DefaultSunDirection = Vec3{ 1, 1, -2 }.normalized()`
- `sky.cloudsEnabled`, `sky.sunEnabled` (bool フラグ) で要素の ON/OFF 可能

---

## ColorF

```cpp
// lerp (Siv3D 組み込み)
ColorF result = colorA.lerp(colorB, t);  // t: 0.0〜1.0
// Clamp は個別成分への Clamp はないので Clamp(double) で係数側を制御する
```

---

## その他

- `[[maybe_unused]]` — 未使用パラメータの警告抑制に使う
- ヘッダ内インライン関数でローカル変数を定義したが使わない場合は変数ごと削除すること（ODR 違反を避けるため warning を放置しない）
