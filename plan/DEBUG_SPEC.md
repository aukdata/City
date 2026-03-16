# デバッグ機能仕様書

## 概要

開発中の描画・ロジック不具合を素早く特定するためのデバッグ機能群。
リリースビルドでは `#ifdef SIV3D_DEBUG` でコンパイル除外する。

デバッグ描画は `DebugRenderer` クラスに集約し、`GameApp` から毎フレーム呼ぶ。

---

## キーバインド

| キー | 動作 |
|---|---|
| F3 | デバッグモード ON/OFF |
| F3 + N | D-01 ネットワーク可視化 トグル |
| F3 + C | D-02 チャンク境界 トグル |
| F3 + V | D-03 車両デバッグ情報 トグル |
| F3 + G | D-04 ワールド座標グリッド トグル |
| F3 + H | D-05 HUD 詳細 トグル |
| F3 + / | ヘルプオーバーレイ 表示（デバッグモード中のみ） |

### 操作の優先順位
- F3 単独押し（down イベント）: デバッグモードをトグル。OFF になると全オーバーレイも非表示。
- F3 を押しながら他キーを押す: 対応機能のトグル。デバッグモードが OFF でも操作可（ON になる）。
- F3 + / : ヘルプパネルを画面中央に表示。再度 F3+/ で閉じる。

---

## D-01: ネットワーク可視化 (F3+N)

道路グラフが正しく構築されているか確認する。

### ノード表示
| 要素 | 内容 |
|---|---|
| 形状 | `Sphere{ pos, 8.0 }` |
| 色 | Intersection: 黄 / TJunction: 橙 / Endpoint: 白 / IC: 水色 |
| ラベル | ノード ID を `Font::draw3D` で表示 |

### エッジ表示
- ベジェ曲線を 20 分割し、セグメントごとに `Cylinder` で描画（半径 1.0m）
- 色: LocalRoad=水色 / Arterial=緑 / Expressway=赤 / Highway=紫
- 中点に `Cone` で進行方向（Forward/Backward）を示す矢印を描画

### 統計ラベル（画面左上）
```
[D-01 Network]  Nodes: 2  Edges: 1  MeshCache: 1
```

---

## D-02: チャンク境界表示 (F3+C)

アクティブチャンクの範囲とロード状態を把握する。

- アクティブチャンクの境界を y=0.1 の正方形ラインで描画
- 色: Active=薄緑 / Sleeping=薄灰 / Loading=薄黄
- チャンク中心に座標ラベル `(cx, cy)` を表示
- カメラ所属チャンクは枠色を白にして強調

---

## D-03: 車両デバッグ情報 (F3+V)

各車両の状態を 3D 空間上に表示する。

- 車両の真上（+5m）に `Font::draw3D` でラベルを描画
- 表示内容: `#{id} {speed:.1f}m/s E:{edgeId} [{state}]`
- 進行方向を細い `Cylinder`（長さ 6m）で表示
- `leadVehicle != -1` のとき先行車両との接続を破線で描画

---

## D-04: ワールド座標グリッド (F3+G)

スケール感の把握と座標確認用の参照グリッド。

- y=0.02 平面に等間隔のグリッド線を描画
- 小グリッド: 100m 間隔、薄い白（alpha 0.2）
- 大グリッド: 1000m (= CHUNK_SIZE) 間隔、白（alpha 0.5）
- カメラ周辺 ±3000m の範囲のみ描画（遠方はカリング）
- 大グリッド交点に `(x, z)` ラベルを表示

---

## D-05: HUD 詳細表示 (F3+H)

通常 HUD に加えて開発情報を表示する（画面左側）。

```
[DEBUG]
--- Camera ---
Eye:   (512.0, 386.3, 971.6)
Focus: (512.0,   0.0, 512.0)
Yaw: 0.00  Pitch: 40.00  Dist: 600.0m

--- Road ---
Nodes: 2  Edges: 1
RoadMeshCache: 1 entries

--- World ---
ActiveChunks: 25  (5x5)
CameraChunk: (0, 0)

--- Traffic ---
Vehicles: 3  (Moving: 3)
```

---

## D-06: オンスクリーンログ (デバッグモード中)

任意コードから1行テキストを画面に流せるデバッグログ。

```cpp
DebugLog::print(U"spawnVehicle: edge={}", edgeId);
```

- 最大 20 行を画面右下に表示（新しいものが下）
- 各行は追加から 3 秒後に自動フェードアウト
- `DebugLog::clear()` で全消去
- リリースビルドではマクロが空になる

---

## ヘルプオーバーレイ (F3+/)

デバッグモード中に F3+/ で画面中央に表示。再度押すと閉じる。

```
┌─────────────────────────────┐
│  DEBUG MODE  (F3 to toggle) │
├─────────────────────────────┤
│ F3+N  Network visualizer    │
│ F3+C  Chunk boundaries      │
│ F3+V  Vehicle debug info    │
│ F3+G  World grid            │
│ F3+H  Detail HUD            │
│ F3+/  This help             │
└─────────────────────────────┘
```

---

## 実装方針

### DebugRenderer クラス

```cpp
class DebugRenderer
{
public:
    void render(const RoadNetwork& network,
                const Array<Vehicle>& vehicles,
                const World& world,
                const GameCamera& camera);

    /// @brief F3 系キー入力を処理する（毎フレーム呼ぶ）
    void handleInput();

    bool isDebugMode() const { return m_debugMode; }

private:
    bool m_debugMode     = false;
    bool m_showNetwork   = false;  ///< F3+N
    bool m_showChunks    = false;  ///< F3+C
    bool m_showVehicles  = false;  ///< F3+V
    bool m_showGrid      = false;  ///< F3+G
    bool m_showDetailHUD = false;  ///< F3+H
    bool m_showHelp      = false;  ///< F3+/

    Font m_font{ FontMethod::MSDF, 16 };

    void renderNetwork(const RoadNetwork& network);
    void renderChunks(const World& world);
    void renderVehicleInfo(const Array<Vehicle>& vehicles);
    void renderGrid(const GameCamera& camera);
    void renderDetailHUD(const RoadNetwork& network,
                         const World& world,
                         const GameCamera& camera,
                         const Array<Vehicle>& vehicles);
    void renderHelp();
};
```

### handleInput() の実装方針

```cpp
void DebugRenderer::handleInput()
{
    if (KeyF3.down() && !KeyN.pressed() && !KeyC.pressed() /* ... */)
        m_debugMode = !m_debugMode;

    if (KeyF3.pressed())
    {
        if (KeyN.down()) m_showNetwork   = !m_showNetwork;
        if (KeyC.down()) m_showChunks    = !m_showChunks;
        if (KeyV.down()) m_showVehicles  = !m_showVehicles;
        if (KeyG.down()) m_showGrid      = !m_showGrid;
        if (KeyH.down()) m_showDetailHUD = !m_showDetailHUD;
        if (KeySlash.down()) m_showHelp  = !m_showHelp;
    }
}
```

### GameApp への組み込み

```cpp
// GameApp.hpp に追加
DebugRenderer m_debugRenderer;

// GameApp::update() 末尾に追加（入力処理を update 内で行う）
m_debugRenderer.handleInput();

// GameApp::render() 末尾に追加
m_debugRenderer.render(m_network, m_traffic.vehicles(), m_world, m_camera);
```

### ビルド切り替え

```cpp
// stdafx.h または プリプロセッサ定義で制御
#ifdef SIV3D_DEBUG
#  define DBG_LOG(...) DebugLog::print(__VA_ARGS__)
#else
#  define DBG_LOG(...) ((void)0)
#endif
```

---

## 優先実装順

1. **D-01 (ネットワーク可視化)** — 道路描画不具合の直接的な診断に使う
2. **D-05 (HUD 詳細)** — カメラ位置・エッジ数の数値確認
3. **D-06 (ログ)** — スポーン・遷移のトレース
4. **D-04 (グリッド)** — スケール感の確認
5. **D-02 (チャンク境界)** — チャンクロードの確認
6. **D-03 (車両情報)** — 交通シミュレーション実装後
