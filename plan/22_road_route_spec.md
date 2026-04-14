# 道路路線（RoadRoute）管理仕様書

## 概要

「国道19号」「中央自動車道」「県道○○号」のような **名称付き道路路線** を扱うためのデータ構造と運用を定義する。
1 つの RoadRoute は複数の `RoadEdge` を順序付きで束ね、ミニマップ表示・情報パネル表示・将来的な経路案内で使用する。

---

## 1. データモデル

### RoadRouteKind

```cpp
enum class RoadRouteKind : uint8
{
    Expressway,       ///< 高速自動車国道 (例: 中央自動車道)
    NationalRoute,    ///< 一般国道 (例: 国道19号)
    PrefectureRoute,  ///< 都道府県道 (例: 県道100号)
    CityRoute,        ///< 市町村道
    Named,            ///< 通称 (例: 青山通り)
};
```

### RoadRoute

```cpp
struct RoadRoute
{
    int              id         = -1;   ///< 割当 ID (ネットワーク内で一意)
    RoadRouteKind    kind       = RoadRouteKind::Named;
    String           name;               ///< 正式名称 ("国道19号" 等)
    int              number     = 0;    ///< 番号 (1〜400, 0=番号なし=通称)
    Array<int>       edgeIds;           ///< 構成エッジ (起点→終点の順)
    ColorF           color{ 0.5 };       ///< ミニマップ・情報パネル表示色
};
```

### 逆引きインデックス

`RoadEdge` に `Array<int> routeIds;` を持たせ、そのエッジが属する路線の ID 列を保持する。

- **永続化しない**（`RoadRoute.edgeIds` が canonical）
- ロード時に `RoadNetwork::rebuildEdgeRouteIndex()` で再構築

---

## 2. 重複所属

1 つの `RoadEdge` が **複数の RoadRoute に所属可能**。日本の重複国道区間（例: 都内の国道1号と国道15号）を再現する。

- `RoadEdge.routeIds.size()` が所属路線数
- 同一 route 内に同じ edge を複数回入れることは禁止

---

## 3. CRUD API

```cpp
class RoadNetwork
{
public:
    int  addRoute(RoadRouteKind kind, String name, Array<int> edgeIds, int number = 0);
    void removeRoute(int routeId);
    RoadRoute*       getRoute(int id);
    const RoadRoute* getRoute(int id) const;
    const Array<RoadRoute>& routes() const;

    /// 逆引きインデックス再構築（ロード時専用）
    void rebuildEdgeRouteIndex();
};
```

`addRoute()` 実行時:
- 新 id を発行 (tombstone 再利用)
- 各 edgeId に `routeIds << newId` を自動追記

`removeRoute()` 実行時:
- 各 edgeId から `routeIds` から id を除去
- route 自体を id=-1 で tombstone 化

---

## 4. 整合性フック

### エッジ削除時 (`removeEdge(edgeId)`)

このエッジを含む各 route について:

| 条件 | 動作 |
|---|---|
| `pos == 0` (起点) | `edgeIds` から先頭除去、route 短縮 |
| `pos == size-1` (終点) | `edgeIds` から末尾除去、route 短縮 |
| `0 < pos < size-1` (中間) | 2 つの route に自動分割 |

**分割ルール**:
- 元 route は `edgeIds[0..pos-1]` に短縮
- 新 route を作成（同じ `kind`, `name`, `number`, `color`、新 id）、`edgeIds = edgeIds[pos+1..end]`
- 分割後にどちらも空になる場合、route ごと削除

### エッジ分割時 (`splitEdgeAt(edgeId, arc) -> newEdgeId`)

各 route の edgeIds 中の `edgeId` を `[edgeId, newEdgeId]` に置換する。方向は元 route の順序を保つ。

### ノード結合時 (`dissolveNode(nodeId) -> newEdgeId`)

2 エッジが 1 エッジにマージされる場合:
- 両エッジが同じ route に属する → その箇所を単一 newEdgeId に置換
- 片方のみが route に属する → その route の edgeId を newEdgeId に置換、もう片方は無視
- 異なる route に属するが重複しない → 両 route が newEdgeId を含むよう更新（重複所属）

---

## 5. 自動命名

`generateAutoRouteName(RoadRouteKind)` で kind に応じたデフォルト名を生成する。

### 番号付き路線 (National/Prefecture/City)

1〜400 の範囲から **kind 内でユニークな番号** をランダムに割り当てる:

```
int pickUnusedNumber(RoadRouteKind kind):
  used = 既存 route の同 kind の number を集合化
  50 回リトライ: Random(1,400) が未使用なら採用
  フォールバック: 1〜400 を線形走査、最小未使用を採用
  満杯時 0 を返す
```

生成名:
- NationalRoute:   `"国道" + N + "号"`
- PrefectureRoute: `"県道" + N + "号"`
- CityRoute:       `"市道" + N + "号"`

### Expressway

`"○○自動車道"` — 起点/終点の地名から合成（v2 で地名連動、v1 は固定文字列）。

### Named

`"通り"` — 固定（v1）。v2 で地名連動。

### 色

kind ごとのデフォルト色:

| kind | 色 |
|---|---|
| Expressway | 緑 `ColorF{ 0.0, 0.6, 0.0 }` |
| NationalRoute | 赤 `ColorF{ 0.8, 0.1, 0.1 }` |
| PrefectureRoute | 青 `ColorF{ 0.1, 0.35, 0.75 }` |
| CityRoute | 黄 `ColorF{ 0.95, 0.7, 0.2 }` |
| Named | 灰 `ColorF{ 0.55 }` |

---

## 6. 永続化

セーブ形式 **v9** で routes を追記。

### フォーマット

ファイル末尾（RoadObject の後）に追記:

```
uint32 routeCount
for each route:
    int32  id
    uint8  kind
    uint16 nameLen + UTF-8 bytes
    int32  number
    uint32 edgeCount
    int32[edgeCount] edgeIds
    float  r, g, b  // color
```

### 互換性

- v7 / v8 セーブも読み込み可（route 数 0 として扱う）
- `readGlobal()` 末尾で `rebuildEdgeRouteIndex()` を呼ぶ

---

## 7. UI

### 道路選択時の情報パネル (v1)

選択中の `RoadEdge` に対し、`edge.routeIds` を辿って所属 route 名を表示:

```
所属路線:
  ■ 国道19号        (赤色スウォッチ + 名称)
  ■ 中央自動車道    (緑色スウォッチ + 名称)
```

### ミニマップ表示 (v2 以降)

Route の edgeIds に沿って polyline を描画、色は `route.color`。
ラベルは route の中点付近。本仕様 v1 では扱わない。

---

## 8. 設計上の注意

- `number` の範囲 1〜400 は kind 内でユニーク。日本の国道番号 (1〜507) との厳密一致ではなく、ゲーム設定として扱う。
- `Expressway` の番号は原則使わない（`number = 0`）。
- route の方向（起点→終点）は `edgeIds` の順序で表現。個々の edge の A→B 向きとは独立。
- 同じ edge が route 内に複数出現するのは禁止（addRoute で検出、または整合性フックで防ぐ）。
- 将来拡張: route の地域タグ（都道府県ID）、起点終点地名の自動追跡、バイパス/支線の親子関係。
