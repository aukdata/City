# パネルシステム仕様書

## 概要

画面上の情報表示・編集UIを統一的に管理するパネルシステム。集落一覧、道路情報、ノード情報など、複数のパネルを同時に表示・操作できる。

---

## 1. パネルの構造

```
┌──────────────────────────┐
│ タイトルバー           [×] │  ← ドラッグで移動（movable の場合）
├──────────────────────────┤
│                          │
│  コンテンツ領域          │  ← 任意の描画（文字・ボタン等）
│                          │
│                          │
└──────────────────────────┘
```

### 1.1 プロパティ

| プロパティ | 型 | 説明 |
|---|---|---|
| `id` | String | パネルの一意な識別子 |
| `title` | String | タイトルバーに表示する文字列 |
| `pos` | Vec2 | パネル左上のスクリーン座標 |
| `size` | Vec2 | パネルの幅・高さ（固定、リサイズ不可） |
| `visible` | bool | 表示中かどうか |
| `movable` | bool | タイトルバードラッグで移動可能か |
| `zOrder` | int | 前後関係（大きいほど前面） |
| `scrollable` | bool | コンテンツ領域のスクロールを有効にするか |
| `scrollOffset` | double | 現在のスクロール位置 [px]（0 = 先頭） |
| `contentHeight` | double | コンテンツの実際の高さ [px]（描画側が毎フレーム報告） |

### 1.2 タイトルバー

- パネル上部の固定高さ領域（例: 24px）
- 左側: タイトル文字列
- 右側: 閉じるボタン [×]（クリックで `visible = false`）
- `movable = true` の場合、タイトルバーをドラッグしてパネル位置を移動可能

### 1.3 コンテンツ領域

- タイトルバーの下の領域
- パネルの所有者が自由に描画する（文字、ボタン、リスト等）
- パネルシステムはコンテンツ描画に関与しない（コールバックまたは描画関数で委譲）

---

## 2. 描画

- パネル全体を半透明の黒（例: `ColorF{0, 0, 0, 0.8}`）で塗りつぶす
- タイトルバーはわずかに明るい背景（例: `ColorF{0.15, 0.15, 0.2}`）で区別
- `zOrder` の昇順で描画（小さい zOrder が奥、大きい zOrder が手前）

---

## 3. 複数パネルの管理

### 3.1 同時表示

- 複数のパネルを同時に表示できる
- 各パネルは独立した位置・サイズを持つ

### 3.2 前後関係（Z オーダー）

- 各パネルは `zOrder` を持つ
- **新しく表示されたパネル**は最前面に配置される
- **クリックされたパネル**は最前面に移動する
- 描画は zOrder 昇順（奥→手前）で行う
- 入力判定は zOrder 降順（手前→奥）で行う

### 3.3 Z オーダーの更新

```
パネル表示時:
  zOrder = 現在の最大 zOrder + 1

パネルクリック時:
  zOrder = 現在の最大 zOrder + 1
```

---

## 4. 入力処理

### 4.1 パネル上の入力遮蔽

マウスポインタがいずれかの表示中パネルの矩形内にある場合、**全てのマウス入力は背後の要素（3Dシーン、カメラ操作等）に伝播しない**:

- **クリック**: パネルが消費。背後の選択・編集操作は発生しない
- **ホイール**: パネルのスクロールに使用。背後のカメラズームは発生しない
- **ドラッグ**: パネルのタイトルバー移動に使用。背後のカメラ回転・パンは発生しない

`isMouseOnAnyPanel()` を使い、カメラ操作やゲーム入力の前にパネル上かどうかを判定する。

### 4.2 判定順序

```
1. 表示中パネルを zOrder 降順（手前→奥）でイテレート
2. マウス位置がパネル矩形内にあるか判定
3. 最初にヒットしたパネルがクリックを受け取る
4. いずれのパネルにもヒットしなければ、背後の要素にクリックが伝播する
```

### 4.3 タイトルバー操作

- **閉じるボタン [×]**: クリックで `visible = false`
- **タイトルバードラッグ**（`movable = true` の場合）:
  - タイトルバー上でマウス左ボタン押下 → ドラッグ開始
  - マウス移動量分だけ `pos` を更新
  - マウス左ボタン解放 → ドラッグ終了

### 4.4 スクロール

- `scrollable = true` のパネル上でマウスホイールを操作すると、コンテンツ領域がスクロールする
- `scrollOffset` はクランプされる: `0 ≤ scrollOffset ≤ max(0, contentHeight - viewHeight)`
- `viewHeight` = パネルの高さ - タイトルバーの高さ

---

## 5. パネルの種類（初期実装）

| パネル ID | タイトル | movable | 用途 |
|---|---|---|---|
| `edge_info` | RoadEdge #N | true | 道路エッジの情報表示・編集 |
| `node_info` | RoadNode #N | true | 道路ノードの情報表示・編集 |
| `name_list` | 地名一覧 | false | 集落の一覧表示（画面右上固定） |

---

## 6. API 設計

### 6.1 PanelManager クラス

```cpp
class PanelManager
{
public:
    /// @brief パネルを登録する（初回のみ）
    void registerPanel(StringView id, Vec2 size, bool movable);

    /// @brief パネルを表示する（zOrder を最前面に設定）
    void show(StringView id, StringView title, Vec2 pos);

    /// @brief パネルを閉じる
    void hide(StringView id);

    /// @brief パネルが表示中かどうか
    bool isVisible(StringView id) const;

    /// @brief 入力処理（ドラッグ・閉じるボタン・クリック遮蔽）
    /// @return いずれかのパネルがクリックを消費したら true
    bool handleInput();

    /// @brief 全パネルの背景・タイトルバーを描画（zOrder 順）
    /// @details コンテンツ描画はこの後に呼び出し元が行う
    void drawBackgrounds();

    /// @brief 指定パネルのコンテンツ領域の左上座標を返す
    /// @return コンテンツ領域の左上 (pos.x, pos.y + titleBarHeight)
    Vec2 contentPos(StringView id) const;

    /// @brief スクロール対応のコンテンツ描画を開始する
    /// @details ScopedViewport2D + Transformer2D を設定し、
    ///          コンテンツ描画側はスクロール位置を意識せずに
    ///          (0, 0) から描画するだけでよい。
    ///          戻り値のオブジェクトがスコープを抜けると自動解除。
    /// @return RAII ガードオブジェクト（スコープ終了で viewport 解除）
    Optional<ScopedContentArea> beginContent(StringView id);

    /// @brief コンテンツの実際の高さを報告する（スクロール範囲の計算に使用）
    void reportContentHeight(StringView id, double height);

    /// @brief マウスがいずれかのパネル上にあるかどうか
    bool isMouseOnAnyPanel() const;
};
```

### 6.2 使用例

```cpp
// 初期化時
m_panelManager.registerPanel(U"edge_info", Vec2{300, 500}, true);
m_panelManager.registerPanel(U"node_info", Vec2{300, 400}, true);
m_panelManager.registerPanel(U"name_list", Vec2{250, 600}, false);

// 道路クリック時
m_panelManager.show(U"edge_info", U"RoadEdge #{}"_fmt(edgeId), Vec2{900, 50});

// 毎フレーム
bool consumed = m_panelManager.handleInput();
if (!consumed && MouseL.down()) { /* 3Dシーンへのクリック処理 */ }

m_panelManager.drawBackgrounds();

// コンテンツ描画（スクロール対応）
if (m_panelManager.isVisible(U"edge_info"))
{
    // beginContent() 内で ScopedViewport2D + Transformer2D が設定される
    // コンテンツ側は (0, 0) から描画するだけ（スクロール位置を意識しない）
    if (auto area = m_panelManager.beginContent(U"edge_info"))
    {
        int y = 0;
        font(U"Speed: 60 km/h").draw(Vec2{4, y}, Palette::White); y += 18;
        font(U"Lanes: 4").draw(Vec2{4, y}, Palette::White); y += 18;
        // ... コンテンツが長くなっても自動でスクロール対応
        m_panelManager.reportContentHeight(U"edge_info", y);
    }
    // area がスコープを抜けると viewport/transform が自動解除
}
```

---

## 7. 実装上の注意

- パネルの描画は 2D コンテキストで行う（3D シーン描画後）
- `handleInput()` は描画前に呼ぶ（入力状態を確定してから描画）
- パネルが画面外に出ないようクランプは任意（初期実装では省略可）
- コンテンツの描画はパネルシステムの外で行う（PanelManager はフレーム・クリック遮蔽・スクロールを担当）

---

## 8. スクロール

### 8.1 概要

コンテンツがパネルの表示領域を超える場合、自動的にスクロール可能になる。コンテンツ描画側はスクロール位置を意識する必要がない。

### 8.2 仕組み

`beginContent()` が以下を設定する:

1. **ScopedViewport2D**: コンテンツ領域にクリッピング（領域外の描画を遮断）
2. **Transformer2D**: 描画座標を `(0, -scrollOffset)` だけオフセット

これにより、コンテンツ描画側は常に `(0, 0)` から描画するだけでよい。スクロール位置はパネルシステムが管理する。

```
コンテンツ全体（contentHeight = 800px）
┌──────────────────────┐ ← y=0（描画起点）
│  項目1                │
│  項目2                │
│  項目3                │
├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤ ← scrollOffset（ここから表示）
│  項目4  ←見えている  │
│  項目5  ←見えている  │
│  項目6  ←見えている  │
├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤ ← scrollOffset + viewHeight
│  項目7                │
│  ...                 │
└──────────────────────┘ ← y=contentHeight
```

### 8.3 スクロールバー

- コンテンツがパネル高さを超える場合、右端にスクロールバーを表示
- スクロールバーはドラッグでも操作可能（将来拡張）

### 8.4 コンテンツ側の責務

コンテンツ描画側が行うこと:
1. `beginContent()` を呼ぶ（RAII ガード取得）
2. `(0, 0)` から通常通り描画（スクロール位置は意識しない）
3. 描画が終わったら `reportContentHeight()` で実際の高さを報告

パネルシステムが行うこと:
- ビューポートのクリッピング
- 座標のオフセット
- マウスホイールによるスクロール
- スクロールバーの描画
- `scrollOffset` のクランプ

---

## 9. PanelLayout（宣言的UIレイアウト）

### 9.1 概要

`PanelLayout` は、パネル内のウィジェット配置を宣言的に構築するシステム。既存の即時モード描画（`PanelWidget` による手動 y 座標管理）を置き換え、VStack/HStack による自動レイアウトを提供する。

ファイル: `src/ui/PanelLayout.hpp`, `src/ui/PanelLayout.cpp`

### 9.2 設計思想

- **初回構築・毎フレーム更新**: UIツリーは一度だけ構築し、毎フレーム `update()` → `draw()` で描画
- **自動レイアウト**: VStack/HStack が子要素の位置を自動計算。手動の `y += kLH` が不要
- **データバインド**: ラムダ（`std::function<String()>` 等）または参照（`float&`, `bool&`）で値を渡す
- **イベント分離**: `update()` で入力処理、`draw()` で描画、`clicked()` / `changed()` でイベント取得

### 9.3 使用フロー

```cpp
// ── 初回構築（initScene 等） ──
PanelLayout layout;
layout.label(U"speed", U"Speed: {}",
    [this] { return U"{:.1f} km/h"_fmt(speed); }, ColorF{1.0});
layout.button(U"track", U"Track", m_tracking, 120);
layout.beginHStack(8);
    layout.label(U"edge", U"Edge: {}",
        [this] { return U"{}"_fmt(edgeId); });
    layout.label(U"lane", U"Lane: {}",
        [this] { return U"{}"_fmt(laneId); });
layout.end();

// ── 毎フレーム ──
layout.setVisible(U"set_goal", m_selectedEdgeId.has_value());
layout.update(contentWidth);   // dirty ならレイアウト再計算 → 入力処理
layout.draw();                 // 描画のみ

if (layout.clicked(U"track")) { m_tracking = !m_tracking; }
int totalH = layout.contentHeight();
```

### 9.4 ウィジェット一覧

#### レイアウトコンテナ

| メソッド | 説明 |
|---|---|
| `beginVStack(gap, padding)` / `end()` | 縦並び。子を上から下に積む |
| `beginHStack(gap)` / `end()` | 横並び。子を左から右に並べる |
| `spacer(height)` | 固定スペース |
| `separator()` | 区切り線 |

#### データウィジェット

| メソッド | データバインド |
|---|---|
| `label(key, text, source, color, bold)` | ラムダ `std::function<String()>` |
| `label(key, text, ref, color, bold)` | 参照（String& / float& / int&） |
| `label(key, text, color, bold)` | 固定テキスト |
| `button(key, label, active, width, tooltip)` | ラムダ or bool& でアクティブ状態 |
| `toggle(key, labelOn, labelOff, ref, width, tooltip)` | bool& に直接読み書き |
| `spin(key, ref, step, lo, hi, format, width)` | float& に直接読み書き |
| `cycle(key, ref, options, width, tooltip)` | int& に直接読み書き（enum は reinterpret_cast） |

#### 特殊ウィジェット

| メソッド | 説明 |
|---|---|
| `beginSection(key, title, collapsed, color)` / `endSection()` | 折りたたみセクション |
| `custom(key, onDraw, height)` | C++ コールバックによるカスタム描画 |

### 9.5 レイアウトモデル

**幅（トップダウン）:**
- ルート: `update(availableWidth)` で渡された幅
- vstack 内の子: 親幅 - padding*2
- hstack 内の子: `width` 指定があればその値、なければコンテンツ幅を推定

**高さ（ボトムアップ）:**
- 通常ウィジェット: 17px
- spacer: 指定値、separator: 5px
- vstack: 子の高さ合計 + gap*(n-1) + padding*2
- hstack: 子の最大高さ
- section: ヘッダ(17px) + 展開時は子の高さ合計
- custom: height 指定値、または onDraw の戻り値

**dirty フラグ:**
- 初期値 `true`（初回 `update()` で自動計算）
- `setVisible()` やセクション折りたたみで `true` に設定
- `update()` 冒頭で dirty なら再計算

### 9.6 イベント

| メソッド | 説明 |
|---|---|
| `clicked(key)` | ボタンがクリックされたか |
| `changed(key)` | toggle/spin/cycle の値が変化したか |
| `setVisible(key, vis)` | ウィジェットの表示/非表示を設定 |

イベントは `update()` 内で記録され、次の `update()` 冒頭でクリアされる。

### 9.7 PanelManager との統合

PanelLayout は PanelManager の `beginContent()` スコープ内で使用する。PanelManager 側の変更は不要。

```cpp
auto area = m_panelManager.beginContent(U"vehicle_info");
if (!area) return;

m_vehicleLayout.update(contentWidth);
m_vehicleLayout.draw();
m_panelManager.reportContentHeight(U"vehicle_info", m_vehicleLayout.contentHeight());
```

既存の即時モード描画との混在も可能。PanelLayout の描画後に `y += layout.contentHeight()` で座標を進めれば、後続の即時モード描画と共存できる。

### 9.8 内部実装

全ウィジェット型を `std::variant`（`UIVariant`）で統一。コンテナ型は `Array<std::unique_ptr<UIElement>>` で子を持つ再帰構造。構築時は `beginVStack()` / `end()` のスタックで入れ子を管理。

`std::function` のキャプチャが `[this]`（8バイト）のみであれば MSVC の Small Buffer Optimization によりヒープ確保は発生しない。

### 9.9 段階的移行

各パネルを個別に PanelLayout に移行できる。移行順序の制約はない。

1. パネル毎に `PanelLayout` メンバ変数を追加
2. `drawXxxPanel()` 内で構築（初回のみ）
3. 旧コードを `update()` + `draw()` + イベント処理に置換
4. 複雑な部分は `custom()` で旧コードを包む

---

## 10. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `06_ui_spec.md` | UI レイアウトの一部としてパネルシステムを使用 |
| `17_road_node_spec.md` | ノード情報パネルの表示内容 |
| `07_road_lane_spec.md` | エッジ情報パネルの車線表示・編集 |
| `11_placename_spec.md` | 地名一覧パネルの表示内容 |
