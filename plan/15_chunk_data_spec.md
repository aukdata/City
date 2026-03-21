# 15. チャンクデータ設計仕様

## 概要

チャンク単位でゲームオブジェクトを管理する。
**ChunkData**（セーブ/ロード用）と**ロード済みリスト**（ランタイム用ワーキングセット）の二層構造を採用する。

---

## 1. 二層構造の責務

| 層 | 役割 | 常時メモリ |
|---|---|---|
| `ChunkData` | チャンク内オブジェクトの永続データ（セーブ/ロード単位） | No（将来はファイル） |
| ロード済みリスト | 全ロード中チャンクの実体をまとめたワーキングセット | Yes |

- **ChunkData** は現在はメモリ上のリストに保管するが、将来的にはファイルとして保存・読み込みする
- ランタイムシステム（RoadRenderer, TrafficManager, PathfindingGraph 等）は**ロード済みリスト**のみを参照する

---

## 2. ChunkData 構造

```cpp
struct ChunkData
{
    Point            chunkCoord;   ///< チャンク座標（ワールド座標への変換用）

    // 道路ネットワーク
    Array<RoadNode>  nodes;        ///< このチャンクが保有するノード（境界ノードは隣接チャンクと重複あり）
    Array<RoadEdge>  edges;        ///< このチャンクが保有するエッジ（nodeA の座標で帰属チャンクを決定）

    // 地区・ゾーン
    Array<MapGenerator::Settlement> districts;

    // ※ 車両は保有しない（セーブ時にスナップショット生成。詳細は §5 参照）
};
```

> **座標系**: ChunkData 内の全座標はワールド座標で保持する（チャンクローカル座標は使わない）

---

## 3. ロード済みリスト

```cpp
// GameScene メンバー（ランタイム用ワーキングセット）
HashTable<int, RoadNode>  m_loadedNodes;   ///< node.id → RoadNode（dedup 済み）
HashTable<int, RoadEdge>  m_loadedEdges;   ///< edge.id → RoadEdge（dedup 済み）
Array<Vehicle>            m_loadedVehicles;
// ... 他オブジェクト
```

`HashTable<int, T>` によって ID をキーとするため、同一 ID のオブジェクトが複数チャンクから登録されても自動的に重複排除される。

---

## 4. ノードの重複保持ルール

境界付近のノードは隣接チャンクの **両方の ChunkData** に保持する。

```
Chunk A     Chunk B
  ●──────────●──────────●
 nodeX      nodeY      nodeZ
           ↑ 境界付近: A の ChunkData にも B の ChunkData にも nodeY を保持
```

### チャンクアンロード時の参照カウント

複数チャンクが同一ノードを保有しうるため、アンロード時は **他のロード中チャンクが同ノードを持っているか** を確認してからロード済みリストを削除する。

```
チャンク A をアンロードする場合:
  for each node in ChunkData_A:
      他のロード中チャンクに同 node.id が存在しない → m_loadedNodes から削除
      存在する → 削除しない
```

実装上は `HashTable<int, int> m_nodeRefCount`（nodeId → 参照チャンク数）で管理するか、
全ロード中チャンクの ChunkData を走査して確認する。

---

## 5. エッジの帰属ルール

エッジは **nodeA のワールド座標が属するチャンク** の ChunkData のみに保持する（重複なし）。

```cpp
Point ownerChunk = worldPosToChunk(nodeA.position);
m_chunkData[regionKey(ownerChunk)].edges << edge;
```

クロスチャンクエッジ（nodeA と nodeB が異なるチャンク）も同様に nodeA チャンクが所有する。

---

## 6. 車両の扱い

車両は毎フレーム移動するため、ChunkData には **常時保持しない**。

- **通常時**: `m_loadedVehicles`（ロード済みリスト）のみで管理
- **セーブ時**: その時点の `m_loadedVehicles` をチャンクごとに分類してセーブデータを生成
- **ロード時**: セーブデータの車両スナップショットから `m_loadedVehicles` を復元

```cpp
// セーブ時のスナップショット生成（例）
for (const auto& v : m_loadedVehicles)
{
    const Point chunk = worldPosToChunk(v.position);
    saveData[regionKey(chunk)].vehicles << v;
}
```

---

## 7. ポスト処理で生成されるノード・エッジの帰属

`resolveIntersections` 等のポスト処理が新たなノード/エッジを生成した場合、
**ワールド座標から `worldPosToChunk()` でチャンクを決定**して ChunkData に追加する。

```cpp
static Point worldPosToChunk(Vec3 pos)
{
    constexpr float kChunkM = 1024.0f;
    const auto fd = [](float v) -> int {
        const int q = static_cast<int>(v / kChunkM);
        return (v < 0.0f && v != static_cast<float>(q) * kChunkM) ? q - 1 : q;
    };
    return Point{ fd(static_cast<float>(pos.x)), fd(static_cast<float>(pos.z)) };
}
```

---

## 8. チャンクロード・アンロードのフロー（概要）

### ロード

```
1. ChunkData を取得（メモリリストまたは将来はファイルから）
2. ChunkData が存在しない → generateChunk() で新規生成
3. ChunkData.nodes を m_loadedNodes に追加（HashTable なので自動dedup）
4. ChunkData.edges を m_loadedEdges に追加
5. 車両スナップショットがあれば m_loadedVehicles に追加
```

### アンロード

```
1. ChunkData.nodes の各ノードについて参照カウントを確認
2. 参照カウントが 0 になるノードを m_loadedNodes から削除
3. ChunkData.edges を m_loadedEdges から削除
4. その場にいる車両を m_loadedVehicles から削除し ChunkData に退避（セーブ用）
5. ChunkData をメモリリストに保持（将来はファイルに書き出し）
```

---

## 9. ChunkData の保管（現フェーズ）

将来のファイル保存に向けた模擬として、`GameScene` に以下を持つ:

```cpp
// 全チャンクデータのリスト（生成済みチャンクは必ずここに入る）
// 将来: ファイルI/O に置き換える
HashTable<int64, ChunkData> m_chunkStore;  ///< chunkKey → ChunkData（永続層）
```

`m_chunkStore` は常時メモリに乗るが、将来のファイル移行を意識して
ランタイムシステムはここを直接参照せず、**ロード済みリストのみを参照**する設計とする。

---

## 10. 関連仕様書

| 仕様書 | 関連箇所 |
|---|---|
| `14_save_spec.md` | ChunkData のファイル保存フォーマット |
| `03_procedural_generation_spec.md` | generateChunk() の生成ロジック |
| `09_vehicle_spec.md` | 車両のチャンク移動・アンロード時退避 |
| `02_technical_spec.md` | RoadNetwork・ワーキングセットのアーキテクチャ |
