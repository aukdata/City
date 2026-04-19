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

## 9. PanelBuilder（即時モード自動レイアウト）

### 9.1 概要

`PanelBuilder` は、パネル内ウィジェットの y 座標管理を自動化する即時モードレイアウトヘルパー。毎フレームスタック上に構築し、描画と入力処理を同時に行う。

ファイル: `src/ui/PanelLayout.hpp`, `src/ui/PanelLayout.cpp`

### 9.2 設計思想

- **毎フレーム構築**: `beginContent()` スコープ内でスタック上に `PanelBuilder` を作り、ウィジェットを順に呼ぶ
- **即時モード**: 各ウィジェット関数が描画と入力処理を同時に行い、結果を即座に返す
- **y 座標の自動管理**: `y += kLH` の手動管理が不要。要素間のスペースも gap で自動挿入
- **横並びはラムダ**: `row(gap, [&]{ ... })` でラムダ内のウィジェットが横に並ぶ

### 9.3 使用フロー

```cpp
auto area = m_panelManager.beginContent(U"edge_info");
if (!area) return;

PanelBuilder ui(contentWidth);

ui.label(U"A:{}  B:{}  {:.0f}m"_fmt(edge->nodeA, edge->nodeB, edge->length), ColorF{1.0});

if (ui.button(U"Swap A/B", false, 62, U"Swap nodeA/B"))
{
    std::swap(edge->nodeA, edge->nodeB);
}

ui.row(4, [&] {
    ui.label(U"Type", ColorF{0.6});
    ui.cycle(edge->roadType, rtNames, 4, 60);
});

ui.row(4, [&] {
    ui.label(U"Speed", ColorF{0.6});
    if (ui.numberInput(edge->speedLimit, 10.f, 10.f, 200.f, U"{:.0f}", 44))
        dirty = true;
    ui.label(U"km/h", ColorF{0.5});
});

if (ui.section(U"Parts", partsCollapsed))
{
    // 折りたたみ内のコンテンツ
}

ui.flush();
m_panelManager.reportContentHeight(U"edge_info", ui.height());
```

### 9.4 ウィジェット一覧

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `label(text, color, bold)` | void | 読み取り専用テキスト |
| `button(label, active, width, tooltip)` | bool | クリックされたら true |
| `toggle(labelOn, labelOff, value&, width, tooltip)` | bool | 変化したら true |
| `numberInput(value&, step, lo, hi, fmt, width)` | bool | 変化したら true。ホイールで増減 / クリックでテキスト編集（TextEditState は内部で自動管理） |
| `textInput(state&, width, maxChars)` | bool | 変化したら true。日本語 IME 対応 |
| `cycle(value&, names, count, width, tooltip)` | bool | 変化したら true。値を直接書き換え |
| `section(title, collapsed&, color)` | bool | 開いていれば true |
| `spacer(height)` | void | 固定スペース |
| `separator()` | void | 区切り線 |
| `row(gap, lambda)` | void | ラムダ内のウィジェットを横並びに |
| `flush()` | void | ツールチップ描画（最後に呼ぶ） |
| `height()` | int | コンテンツ全体の高さ |

### 9.5 レイアウトモデル

- 縦方向: ウィジェットを上から下に積む。要素間に `gap`（デフォルト 2px）を自動挿入
- 横方向: `row()` 内ではウィジェットを左から右に並べる。各ウィジェットの幅は `width` 引数か、label はテキスト幅
- 幅: コンストラクタで受け取る `width` から `padding` を引いた値がデフォルト幅
- 高さ: 各ウィジェットは 17px（kLineH）。spacer は指定値、separator は 5px

### 9.6 PanelManager との統合

`beginContent()` のスコープ内でスタック上に構築する。メンバ変数不要。

```cpp
auto area = m_panelManager.beginContent(U"panel_id");
if (!area) return;

PanelBuilder ui(static_cast<int>(m_panelManager.getSize(U"panel_id").x));
// ... ウィジェット ...
ui.flush();
m_panelManager.reportContentHeight(U"panel_id", ui.height());
```

---

## 10. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `06_ui_spec.md` | UI レイアウトの一部としてパネルシステムを使用 |
| `17_road_node_spec.md` | ノード情報パネルの表示内容 |
| `07_road_lane_spec.md` | エッジ情報パネルの車線表示・編集 |
| `11_placename_spec.md` | 地名一覧パネルの表示内容 |
