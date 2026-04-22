# コーディング規約

## 命名規則

| 対象 | 規則 | 例 |
|---|---|---|
| クラス・構造体 | PascalCase | `RoadNetwork`, `VehicleManager` |
| 列挙型 | `enum class` + PascalCase, 基本型 `uint8` | `enum class NodeType : uint8 { Endpoint, Joint }` |
| 列挙値 | PascalCase | `OpState::Open` |
| 関数 | camelCase | `addNode()`, `getEdge()`, `buildDefaultLanes()` |
| プライベートメンバ変数 | `m_` + camelCase | `m_edges`, `m_nextNodeId` |
| constexpr 定数 | `k` + PascalCase | `kRoadSurfaceLift`, `kDrawMaxDist` |
| ローカル変数 | camelCase | `cutoffArc`, `signalPos` |
| フィールド（構造体） | camelCase | `nodeA`, `speedLimit`, `arcPos` |

## ファイル構成

### ディレクトリ

```
src/
  road/       道路グラフ・部品管理
  traffic/    車両・信号機
  render/     描画
  scene/      シーン管理・UI
  sim/        経路探索スレッド
  save/       セーブ/ロード
  world/      地形
  ui/         UI部品
  time/       ゲーム時刻
  gen/        マップ生成
  asset/      アセット管理
```

### ヘッダ / ソース分割

- インターフェース（構造体・クラス宣言）はヘッダ `.hpp` に記述
- 実装の詳細（パースヘルパー等）はソース `.cpp` の無名名前空間に隠蔽
- ユーティリティ関数群は名前付き namespace（例: `ObjParser`）

## 文字コード・改行

- **UTF-8 BOM + CRLF**: `.cpp` / `.hpp` / `.h` ファイル
- 新規ファイル作成後は `python3 chore/add_bom.py` を実行
- Edit 前に `python3 chore/convert_line_endings.py to-lf`、Edit 後に `to-crlf`

## Include 順序

```cpp
#pragma once
// 1. Siv3D / 標準ライブラリ
#include <Siv3D.hpp>
#include <future>
// 2. プロジェクトヘッダ（相対パス）
#include "../time/GameClock.hpp"
#include "RoadTypes.hpp"
```

## コメント

### Doxygen ドキュメントコメント

```cpp
/// @brief 道路グラフ管理クラス
/// @details エッジ・ノードの追加/削除・交差検出を担う
class RoadNetwork { ... };
```

### フィールドコメント

```cpp
float speedLimit = 60.0f;  ///< 制限速度 [km/h]
int   nodeA      = -1;     ///< 始点ノード ID
```

### セクション区切り

```cpp
// ===== セクションタイトル =====
// ── サブセクション ──
```

## 制御構文

- `if` / `for` / `while` は**必ず `{}` で囲む**（1行でも省略しない）

```cpp
// OK
if (node.id < 0)
{
	continue;
}

// NG
if (node.id < 0) continue;
```

## 定数

- 数値定数は `constexpr` で定義（マジックナンバー禁止）
- クラス定数は `static constexpr`

```cpp
static constexpr double kLodDist   = 800.0;
static constexpr double kLodDistSq = kLodDist * kLodDist;
```

## 型の使い方

### Siv3D 型を優先

| 用途 | 使う型 |
|---|---|
| 配列 | `Array<T>`（STL vector ではなく） |
| 文字列 | `String`（`U"..."` リテラル） |
| ハッシュマップ | `HashTable<K, V>` |
| ハッシュセット | `HashSet<T>` |
| 失敗可能な戻り値 | `Optional<T>` |
| 座標 | `Vec3`, `Vec2`, `Float3`, `Float2` |
| 色 | `ColorF` |
| テクスチャ | `TextureAsset` (共有) / `Texture` (個別所有) |
| フォント | `FontAsset` |

### Array 操作

```cpp
ids << key;                          // 追加
arr.remove_if([](const T& a) { return a.id < 0; });  // 条件削除
arr.sort_by([](const T& a, const T& b) { return a.x < b.x; });
```

## エラーハンドリング

- 失敗可能な関数は **`Optional<T>`** または `bool` を返す
- **`-1` や `nullptr` を失敗値として返すことを禁止** — 代わりに `Optional<T>` を使う
- バイナリ I/O: 読み込み失敗で即座に `return false`
- ユーザ向けエラー: `Console << U"[モジュール名] メッセージ"` で出力

```cpp
// OK: 失敗時は none
Optional<int> addEdge(...);
Optional<int> findNodeNear(Vec3 pos, float radius) const;

// NG: -1 を失敗値として使わない
int addEdge(...);  // 失敗時 -1 を返す ← 禁止
```

## アセット系の活用

- テクスチャ・フォントは **`TextureAsset` / `FontAsset`** を積極的に使い、同一リソースの重複ロードを避ける
- `AssetRegistrar.cpp` で起動時に一括登録し、`Asset::` 名前空間の定数でアクセス
- Registry パターン（後述）でアセットグループを管理

```cpp
// 登録（起動時1回）
TextureAsset::Register(U"Tex_Asphalt", U"assets/textures/asphalt.png", TextureDesc::MippedSRGB);

// 使用（どこからでも）
TextureAsset(U"Tex_Asphalt").draw(...);
```

## メモリ管理

- **値型所有**: `Array<T>` でオブジェクトを直接所有
- **ID ルックアップ**: `HashTable<int, int>` で ID → インデックスの O(1) マッピング
- **ソフト削除**: `id = -1`（tombstone）+ フリースロット再利用
- **共有参照**: `std::shared_ptr<const T>` はスレッド間共有時のみ
- **非所有ポインタ**: `T*` は内部データへの参照のみ（所有しない）

## TOML 読み込みパターン

```cpp
const TOMLReader toml{ tomlPath };
if (!toml) { Console << U"[Module] Failed: " << path; return none; }

def.id   = toml[U"id"].getOr<String>(U"");
def.name = toml[U"name"].getOr<String>(def.id);

// テーブル配列 [[items]]
if (const auto arr = toml[U"items"]; arr.isTableArray())
    for (const auto& item : arr.tableArrayView()) { ... }
```

## バイナリ I/O パターン

```cpp
// 書き込み
w.write(kMagic);
w.write(kVersion);
writeString(w, str);

// 読み込み
if (!r.read(magic) || magic != kMagic) return false;
if (!r.read(version) || version != kVersion) {
    Console << U"[Module] Unsupported version: " << version;
    return false;
}
```

## 描画パフォーマンス（カリング・キャッシュ戦略）

Debug ビルドでも 60 FPS を維持することを目標とする。描画系クラスは以下のパターンを徹底する。

### 原則

- **推測するな・計測せよ** — `perf.log` / `Stopwatch` / `m_renderTimings` で実測してからボトルネックを潰す
- 毎フレームループ内で **重い計算を同期呼び出ししない**（allocation・O(N²) 走査・`getBezier`+`positionAt` の連打など）
- **計測コードは残す** — 内訳タイミングは `RenderTimings` 構造体 + `perf.log` に出力する仕組みを保持する

### カリング

描画ループの最初に、安価なチェックから順に早期 `continue` する：

```cpp
// 1. 距離カリング（float 演算のみ、Sqrt なし）
const float distSq = dx * dx + dz * dz;
if (distSq > kDrawMaxDistSqF) { continue; }

// 2. 視錐台カリング（距離を通過した分のみ実施）
if (!frustum.intersects(Sphere{ center, radius })) { continue; }

// 3. LOD 判定（近距離のみ高精細）
const bool isClose = (distSq < kLodDistSqF);
```

距離閾値は `constexpr` で定義し、二乗値も事前計算（`kDrawMaxDist` と `kDrawMaxDistSq` をペアで置く）。

### キャッシュ戦略

**「毎フレーム不変なものは計算しない」** を徹底する。キャッシュは以下の 3 層を使い分ける：

| 層 | 無効化タイミング | 例 |
|---|---|---|
| **ID ベースキャッシュ** | 対象の編集時に `erase(id)` で明示的に落とす | `m_partMeshCache`, `m_boundsCache`（edgeId キー） |
| **状態フラグ** | 参照側の状態変化を検知して全無効化 | `m_signalSummaryCache[node]` を `lastPhaseIdx` の変化で再構築 |
| **件数＋周期キャッシュ** | ネットワーク件数の変化 or N フレーム経過で再構築 | `RoadRouteSignRenderer::m_anchorCache`（30 フレーム周期 + `nodes/edges/routes` 件数監視 + 明示 `invalidate()`） |

設計指針：

- **カメラ依存とネットワーク依存を分離** — アンカー位置はネットワーク依存（キャッシュ）、`worldToScreenPoint` やフェードはカメラ依存（毎フレーム計算）
- **キャッシュは `mutable` で `const` メソッドからも書ける** — 描画メソッドは論理的には `const` なので
- **明示的な `invalidate()` を用意** — 呼び出し側が編集タイミングで叩けるようにする
- **周期再構築を保険に入れる** — 「ベジエ形状ドラッグ」など件数では拾えない変化があるため、N フレーム（例: 30 = 約0.5秒）ごとの再構築を併用する

### 計測を仕込む

新規の描画系クラスは最初から `perf.log` に内訳を出せるようにしておく：

```cpp
// GameScene::RenderTimings に double フィールドを追加
Stopwatch sw{ StartImmediately::Yes };
auto lap = [&](double& out) { out = sw.msF(); sw.restart(); };

m_fooRenderer.render(...); lap(m_renderTimings.foo);
m_barRenderer.render(...); lap(m_renderTimings.bar);
```

ピンポイント内訳（どの呼び出しが重いか）は `Console` に 120 フレームごとの集計を出すと `perf.log` と突き合わせやすい。

## アセット管理パターン（Registry）

```cpp
class FooRegistry {
public:
    bool load(FilePathView dirPath);                     // ディレクトリスキャン
    [[nodiscard]] const FooDef* getDef(StringView id) const;
    [[nodiscard]] const FooModel* getModel(StringView id) const;
    [[nodiscard]] Array<String> defIds() const;
private:
    struct Entry { FooDef def; FooModel model; };
    HashTable<String, Entry> m_entries;
    Optional<Entry> loadEntry(FilePathView tomlPath, FilePathView baseDir);
};
```
