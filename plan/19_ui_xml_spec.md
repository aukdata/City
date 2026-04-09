# 19. UI XML スキーマ仕様

## 概要

パネルUIのレイアウトとウィジェット配置をXMLで宣言的に定義し、C++側がそれを読み込んで描画・データバインドする。

**役割分担:**
- **XML** — ウィジェットの種類・配置・レイアウト構造・表示条件
- **C++** — テーマ（色・フォント）・データバインディング・イベント処理・カスタムウィジェットの描画

## ファイル配置

```
App/assets/panels/
  vehicle_info.xml
  edge_info.xml
  node_info.xml
  draw_template.xml
  name_list.xml
  signal_edit.xml
```

1テンプレート = 1ファイル。ファイル名（拡張子除く）がテンプレートIDになる。

### テンプレートとインスタンス

XMLファイルはパネルの **テンプレート** であり、同じテンプレートから **複数のインスタンス** を生成できる。

- **テンプレートID**: ファイル名（例: `vehicle_info`）
- **インスタンスID**: `create()` 時に指定する一意の識別子

```cpp
// テンプレート "vehicle_info" からインスタンス "vehicle_42" を生成
auto& p1 = m_panelManager.create(U"vehicle_info", U"vehicle_42", U"Vehicle #42");
// 同じテンプレートから別のインスタンスを生成
auto& p2 = m_panelManager.create(U"vehicle_info", U"vehicle_87", U"Vehicle #87");
```

インスタンス毎に独立したデータバインド・スクロール状態・位置を持つ。
テンプレートIDとインスタンスIDが同じ場合（1つしか開かないパネル）は省略形を使える:

```cpp
// 省略形: インスタンスID = テンプレートID
auto& edgePanel = m_panelManager.create(U"edge_info", U"Edge Info");
```

---

## XML 構造

```xml
<?xml version="1.0" encoding="UTF-8"?>
<panel size="280,400" movable="true" scrollable="true">
  <!-- ルート要素は必ず1つのレイアウトコンテナ -->
  <vstack gap="2" padding="6">
    ...
  </vstack>
</panel>
```

### `<panel>` — ルート要素

| 属性 | 型 | 必須 | 説明 |
|---|---|---|---|
| `size` | `int,int` | Yes | `幅,高さ` |
| `movable` | `bool` | No | ドラッグ移動可能か（default: `false`） |
| `scrollable` | `bool` | No | スクロール可能か（default: `false`） |

---

## レイアウトモデル

### サイズ決定の基本原則

- **幅: トップダウン** — 親から子へ利用可能幅が伝播する
- **高さ: ボトムアップ** — 子の高さを積み上げて親の高さが決まる

```
panel (width: 280)
  └─ vstack padding="6" → 利用可能幅: 280 - 6*2 = 268
       ├─ label             → 幅 268（親に合わせる）
       ├─ hstack gap="4"    → 幅 268
       │    ├─ label width="60"  → 60 固定
       │    ├─ spin  width="44"  → 44 固定
       │    └─ label stretch     → 残り 268-60-44-4*2 = 156
       └─ button width="120"    → 120 固定（左寄せ）
```

### 幅の決定ルール

| 状況 | 挙動 |
|---|---|
| `width` 指定あり | その値を使う |
| `width` なし + vstack 内 | 親の利用可能幅いっぱい |
| `width` なし + hstack 内 | コンテンツ幅（テキスト幅等） |
| `stretch="true"` + hstack 内 | hstack の残り幅を全て使う。`stretch` が複数ある場合は残り幅を均等分配 |

### 高さの決定ルール

| 状況 | 挙動 |
|---|---|
| `height` 指定あり | その値を使う |
| `height` なし | ウィジェットのデフォルト高さ（17px） |
| vstack | 子の高さ合計 + gap*(n-1) + padding*2 |
| hstack | 子の最大高さ + padding*2 |

---

## レイアウトコンテナ

### `<vstack>` — 縦並び

子要素を上から下に積む。幅は親から継承し、高さは子の合計で決まる。

| 属性 | 型 | default | 説明 |
|---|---|---|---|
| `gap` | `int` | `0` | 子要素間のスペース（px） |
| `padding` | `int` | `0` | 内側の余白（上下左右均一） |
| `align` | `left\|center\|right` | `left` | 子要素の水平揃え |
| `width` | `int` | — | 明示的な幅指定（省略時は親から継承） |
| `visible` | `string` | — | データバインド条件（後述） |

### `<hstack>` — 横並び

子要素を左から右に並べる。幅は親から継承し、高さは子の最大値で決まる。

| 属性 | 型 | default | 説明 |
|---|---|---|---|
| `gap` | `int` | `0` | 子要素間のスペース（px） |
| `padding` | `int` | `0` | 内側の余白 |
| `align` | `top\|center\|bottom` | `top` | 子要素の垂直揃え |
| `width` | `int` | — | 明示的な幅指定 |
| `visible` | `string` | — | データバインド条件 |

### `<spacer>` — 固定スペース

| 属性 | 型 | default | 説明 |
|---|---|---|---|
| `height` | `int` | `4` | vstack 内での高さ（px） |
| `width` | `int` | `4` | hstack 内での幅（px） |

vstack 内では `height` が使われ `width` は無視される。hstack 内では `width` が使われ `height` は無視される。

### `<separator />` — 水平線

視覚的な区切り線を描画する。vstack 内では親幅いっぱいの水平線、hstack 内では高さいっぱいの垂直線になる。

| 属性 | 型 | default | 説明 |
|---|---|---|---|
| `color` | `string` | `"separator"` | テーマカラー名 |

---

## テーマカラー

色の指定にはテーマカラー名を使う。C++ 側でテーマカラーテーブルを定義し、名前から `ColorF` を解決する。

XML では色名で指定する:
```xml
<label key="title" text="Attachments" color="accent" />
<label key="unit" text="km/h" color="muted" />
```

デフォルトテーマの色名（C++ 側で定義）:

| 名前 | 用途 | 参考値 |
|---|---|---|
| `default` | 通常テキスト・ボタン | `ColorF{0.7}` |
| `bright` | 強調テキスト | `ColorF{1.0}` |
| `muted` | 補助テキスト・単位 | `ColorF{0.5}` |
| `accent` | セクション見出し | `ColorF{1.0, 1.0, 0.4}` |
| `error` | エラー・警告 | `ColorF{1.0, 0.3, 0.3}` |
| `separator` | 区切り線 | `ColorF{0.3}` |

色名はテーマとして C++ 側で自由に追加・変更可能。XML に存在しない色名が使われた場合は `Console` に警告を出し `default` にフォールバックする。

バインドタグで動的に色を切り替えることもできる。`{}` で囲まれた値はバインドタグとして解釈し、囲まれていない値はテーマカラー名として直接解決する:

```xml
<!-- 固定: テーマカラー名をそのまま使う -->
<label key="title" text="Attachments" color="accent" />

<!-- 動的: {} で囲むとバインドタグ。C++側から色名を注入する -->
<label key="type_badge" text="{}" color="{badgeColor}" />
```
```cpp
ui->bind(U"type_badge", badgeText);
ui->setColor(U"type_badge", isUrban ? U"error" : U"accent");  // "badgeColor" に色名を設定
```

この `{}` によるバインド判定は `visible`, `enabled`, `color` 属性で共通のルールとなる。

---

## ウィジェット

全ウィジェット共通属性:

| 属性 | 型 | 説明 |
|---|---|---|
| `key` | `string` | C++側でのデータバインド識別子。データバインドやイベントが必要なウィジェットでは必須。`<spacer>`, `<separator>` 等のレイアウト専用要素では不要 |
| `visible` | `string` | 表示条件のバインドタグ |
| `enabled` | `string` | 操作可否のバインドタグ。`false` 時はグレーアウトして入力を受け付けない |
| `width` | `int` | 明示的な幅指定（省略時: vstack内=親幅, hstack内=コンテンツ幅） |
| `height` | `int` | 明示的な高さ指定（省略時: デフォルト 17px） |
| `stretch` | `bool` | hstack 内で残り幅を全て使うか（default: `false`） |
| `color` | `string` | テーマカラー名またはバインドタグ（default: `"default"`） |
| `tooltip` | `string` | ホバー時ツールチップ |

### `<label>` — 読み取り専用テキスト

```xml
<label key="speed" text="Speed: {:.1f} km/h" />
```

| 属性 | 型 | 説明 |
|---|---|---|
| `text` | `string` | 表示テキスト。`{}` はC++側でフォーマット置換される |
| `bold` | `bool` | 太字フォントを使うか（default: `false`） |

### `<button>` — クリックボタン

```xml
<button key="swap_ab" label="Swap A/B" />
```

| 属性 | 型 | 説明 |
|---|---|---|
| `label` | `string` | ボタン表示テキスト |
| `active_label` | `string` | アクティブ状態のラベル（省略時は `label` と同じ） |

### `<toggle>` — ON/OFF トグル

```xml
<toggle key="is_through" label_on="THROUGH" label_off="through" />
```

| 属性 | 型 | 説明 |
|---|---|---|
| `label_on` | `string` | ON 状態のラベル |
| `label_off` | `string` | OFF 状態のラベル |

### `<spin>` — 数値スピナー（ホイール増減）

```xml
<spin key="speed_limit" step="10" min="10" max="200" format="{:.0f}" />
```

| 属性 | 型 | 説明 |
|---|---|---|
| `step` | `float` | 1ホイールあたりの増減量 |
| `min` | `float` | 最小値 |
| `max` | `float` | 最大値 |
| `format` | `string` | 表示フォーマット（default: `{:.1f}`） |

### `<cycle>` — 列挙サイクル（L/Rクリック）

```xml
<cycle key="road_type" options="Local,Arterial,Express,Highway" />
```

| 属性 | 型 | 説明 |
|---|---|---|
| `options` | `string` | カンマ区切りの選択肢リスト |

### `<section>` — 折りたたみセクション

```xml
<section key="attachments" title="Attachments ({})">
  <vstack gap="2">
    ...
  </vstack>
</section>
```

| 属性 | 型 | 説明 |
|---|---|---|
| `title` | `string` | セクションタイトル（`{}` でフォーマット可） |
| `collapsed` | `bool` | 初期状態で折りたたむか（default: `false`） |

子要素はセクションが展開中のみ描画される。折りたたみ時はヘッダ行（デフォルト高さ 17px）のみが残り、子要素の高さは 0 になる。折りたたみ状態は `PanelInstance` が内部で保持する（`key` 単位）。

### `<list>` — 動的リスト

C++側がアイテム数を供給し、`<template>` 内のレイアウトをアイテム毎に繰り返し描画する。

```xml
<list key="waypoints" source="{routeWaypoints}" clickable="true" hover_highlight="true">
  <template>
    <hstack gap="4" height="17">
      <label key="marker" text="{}" width="12" />
      <label key="info" text="E:{} L:{} {:.0f}m" />
    </hstack>
  </template>
</list>
```

| 属性 | 型 | default | 説明 |
|---|---|---|---|
| `source` | `string` | — | データソースのバインドタグ（C++側でアイテム数を決定） |
| `clickable` | `bool` | `false` | アイテムクリックイベントを発火するか |
| `hover_highlight` | `bool` | `false` | ホバー時にアイテム背景をハイライトするか |

#### `<template>` — アイテムテンプレート

`<list>` の直下に1つだけ配置する。テンプレート内には通常のレイアウト・ウィジェットを自由に配置できる:

- レイアウト: `<vstack>`, `<hstack>`, `<spacer>`
- ウィジェット: `<label>`, `<button>`, `<toggle>`, `<spin>`, `<cycle>`
- カスタム: 任意のカスタムウィジェット

テンプレート内ウィジェットの `key` は、描画時に `"{listKey}[{index}].{itemKey}"` に展開される。

例: `<list key="waypoints">` 内の `<label key="info">` → 実行時キーは `"waypoints[0].info"`, `"waypoints[1].info"`, ...

#### C++ 側 API

```cpp
// アイテム数を設定
ui.setListCount("waypoints", waypoints.size());

// 各アイテムのデータをバインド
for (int i = 0; i < count; ++i) {
    ui.listBind("waypoints", i, "marker", isCurrent ? U">" : U" ");
    ui.listBind("waypoints", i, "info", wp.edgeId, wp.laneIndex, wp.edgeLength);
}

// クリックイベント取得（clickable="true" の場合）
if (auto idx = ui.listClicked("waypoints")) {
    // idx 番目のアイテムがクリックされた
}

// アイテム内ボタンのイベント取得
if (auto idx = ui.listClicked("waypoints", "some_button")) {
    // idx 番目のアイテム内の some_button がクリックされた
}

// アイテム毎の色バインド
ui.listSetColor("waypoints", i, "info", isPast ? "muted" : "bright");
```

#### 複雑なアイテムの例: ノードアタッチメント

```xml
<list key="attachments" source="{nodeAttachments}">
  <template>
    <vstack gap="2">
      <hstack gap="4">
        <label key="header" text="[{}] edge #{}" />
        <label key="road_info" text="{} {:.0f}km/h" />
      </hstack>
      <hstack gap="4">
        <label key="lat_label" text="lat" />
        <spin key="lateral" step="1" min="-20" max="20" />
        <toggle key="is_through" label_on="THROUGH" label_off="through" />
      </hstack>
      <hstack gap="4">
        <label key="ctrl_label" text="ctrl" />
        <cycle key="control" options="None,Yield,Stop,Signal" />
      </hstack>
      <hstack gap="4">
        <label key="cpy_label" text="cpY" />
        <spin key="cp_y" step="1" min="-100" max="200" />
      </hstack>
    </vstack>
  </template>
</list>
```

---

## カスタムウィジェット（専用エレメント）

複雑すぎてXMLで表現できないUIは、専用のエレメント名で定義する。
C++側に対応する描画関数を実装する。

### `<road_section_editor>` — 道路断面エディタ

Parts + Lanes の断面バー・ドラッグ編集UI。

```xml
<road_section_editor key="sections" />
```

### `<signal_diagram>` — 信号交差点図

交差点の俯瞰ダイアグラム + 信号状態トグル。

```xml
<signal_diagram key="intersection" />
```

### `<signal_phase_list>` — 信号フェーズリスト

フェーズ一覧（ランプ表示・時間編集・追加削除）。

```xml
<signal_phase_list key="phases" />
```

### `<minimap>` — ミニマップ

```xml
<minimap key="map" />
```

カスタムウィジェットは今後必要に応じて追加する。

### カスタムウィジェットのサイズ

カスタムウィジェットは `height` 属性でサイズを指定する。`height` が省略された場合はコールバックが描画後に実際の高さを返す。

```xml
<!-- 高さ固定 -->
<signal_diagram key="intersection" height="380" />

<!-- 高さ可変（コールバックが描画後に高さを返す） -->
<road_section_editor key="sections" />
```

コールバックのシグネチャ:
```cpp
// 戻り値: 実際に使用した高さ（レイアウトに反映される）
using CustomWidgetCallback = std::function<double(PanelInstance& panel, const UINode& node, RectF area)>;
```

`height` 指定ありの場合は `area` にその高さが入り、戻り値は無視される。
`height` 省略の場合は `area.h` に親の残り高さが入り、コールバックの戻り値が実際の高さとして採用される。

---

## データバインド

### テキストバインド

`text` や `title` 属性の `{}` プレースホルダにC++側が値を注入する。

```xml
<label key="speed" text="Speed: {:.1f} km/h" />
```

C++側:
```cpp
// key="speed" に対して float 値をバインド
panel.bind("speed", vehicle.speed * 3.6f);
```

### 表示条件バインド（`visible`）

`visible` 属性にバインドタグを指定する。C++側が真偽値を設定する。

```xml
<button key="set_goal" label="Set Goal" visible="{hasSelectedEdge}" />
```

C++側:
```cpp
panel.setCondition("hasSelectedEdge", m_selectedEdgeId.has_value());
```

複合条件が必要な場合は `&`（AND）、`|`（OR）で結合:
```xml
visible="{hasVehicle}&{isTracking}"
visible="{hasSelectedEdge}|{hasSelectedNode}"
```

否定は `!` プレフィックス:
```xml
visible="{!isTracking}"
```

---

## 使用例: vehicle_info.xml

```xml
<?xml version="1.0" encoding="UTF-8"?>
<panel size="280,400" movable="true" scrollable="true">
  <vstack gap="2" padding="6">
    <label key="type" text="Type: {}" />
    <label key="speed" text="Speed: {:.1f} km/h" />
    <label key="location" text="Location: {}" />

    <hstack gap="8">
      <label key="edge" text="Edge: {}" />
      <label key="lane" text="Lane: {}" />
    </hstack>

    <spacer height="4" />

    <hstack gap="8">
      <label key="goal_edge" text="Goal Edge: {}" />
      <button key="set_goal" label="Set E{}" width="70"
              visible="{hasSelectedEdge}" tooltip="Set selected edge as goal" />
    </hstack>

    <spacer height="4" />

    <button key="track" label="Track" active_label="Tracking ON" width="120" />

    <section key="route" title="Route: {}/{} waypoints">
      <list key="waypoints" source="{routeWaypoints}" clickable="true" hover_highlight="true">
        <template>
          <hstack gap="4" height="17">
            <label key="marker" text="{}" width="12" />
            <label key="info" text="E:{} L:{} {:.0f}m" />
          </hstack>
        </template>
      </list>
    </section>
  </vstack>
</panel>
```

## 使用例: draw_template.xml

```xml
<?xml version="1.0" encoding="UTF-8"?>
<panel size="374,500" movable="true" scrollable="true">
  <vstack gap="2" padding="6">
    <hstack gap="4">
      <label key="type_label" text="Type" />
      <cycle key="road_type" options="Local,Arterial,Express,Highway" />
    </hstack>

    <hstack gap="4">
      <label key="speed_label" text="Speed" />
      <spin key="speed_limit" step="10" min="10" max="200" format="{:.0f}" />
      <label key="speed_unit" text="km/h" />
      <label key="total_width" text="W:{:.1f}m" />
    </hstack>

    <spacer height="4" />

    <road_section_editor key="sections" />
  </vstack>
</panel>
```

## 使用例: signal_edit.xml

```xml
<?xml version="1.0" encoding="UTF-8"?>
<panel size="700,550" movable="true" scrollable="true">
  <hstack>
    <!-- 左ペイン: フェーズ一覧 -->
    <vstack width="180" padding="6">
      <signal_phase_list key="phases" />
      <spacer height="4" />
      <label key="cycle_total" text="Cycle: {:.0f}s" bold="true" />
    </vstack>

    <!-- 右ペイン: 交差点図 -->
    <signal_diagram key="intersection" />
  </hstack>
</panel>
```

---

## C++ 実装方針

### 内部構造

```
PanelManager
├─ templates: HashMap<String, UINodeTree>     // XMLから読み込んだテンプレート
└─ instances: HashMap<String, PanelInstance>   // 表示中のインスタンス

UINodeTree（テンプレート）
├─ panelSize, movable, scrollable
└─ root: UINode（ウィジェットツリー）

UINode
├─ type: vstack / hstack / label / button / ...
├─ attributes: key, text, visible, width, ...
└─ children: Array<UINode>

PanelInstance（インスタンス）
├─ templateId: String
├─ state: PanelState（位置・スクロール・zOrder等）
├─ bindings: HashMap<String, Variant>         // データバインド値
├─ conditions: HashMap<String, bool>          // 表示条件
├─ events: HashMap<String, bool>              // フレーム内イベント（クリック等）
└─ computed: Array<ComputedLayout>            // レイアウト計算結果
```

### 初期化・読み込み

```cpp
void GameScene::initScene()
{
    // assets/panels/ 内の全 .xml をテンプレートとして一括読み込み
    m_panelManager.loadPanels(U"assets/panels/");

    // テンプレートからインスタンスを生成（メンバ変数に保持）
    m_edgePanel    = &m_panelManager.create(U"edge_info", U"Edge Info");
    m_nodePanel    = &m_panelManager.create(U"node_info", U"Node Info");
    m_namePanel    = &m_panelManager.create(U"name_list", U"Name List");
    m_drawTplPanel = &m_panelManager.create(U"draw_template", U"Draw Template");
}
```

`XMLReader` で各ファイルをパースし、`UINodeTree` を構築してテンプレートとして保持する。

### PanelManager API

```cpp
class PanelManager
{
public:
    /// テンプレート一括読み込み
    void loadPanels(FilePathView dir);

    /// インスタンス生成: create(テンプレートID, インスタンスID, 初期タイトル)
    PanelInstance& create(StringView templateId, StringView instanceId, StringView title);

    /// 省略形: インスタンスID = テンプレートID
    PanelInstance& create(StringView templateId, StringView title);

    /// インスタンス破棄
    void destroy(StringView instanceId);

    /// カスタムウィジェットの描画コールバック登録
    void registerCustomWidget(StringView elementName, CustomWidgetCallback callback);

    /// 全インスタンスの入力処理・描画（zOrder順）
    bool handleInput();
    void drawAll();
};
```

### PanelInstance API

```cpp
class PanelInstance
{
public:
    // ── 表示制御 ──
    void show(Vec2 pos);           // 表示（位置指定）
    void hide();                   // 非表示
    bool isVisible() const;

    // ── メタデータ ──
    void setTitle(StringView title);

    // ── データバインド ──
    template <class... Args>
    void bind(StringView key, Args&&... args);        // テキスト {} に値を注入
    void setCondition(StringView name, bool value);    // visible/enabled 条件
    void setActive(StringView key, bool active);       // button/toggle の active 状態

    // ── リストデータ ──
    void setListCount(StringView key, int count);
    template <class... Args>
    void listBind(StringView key, int index, StringView itemKey, Args&&... args);

    // ── リスト色バインド ──
    void listSetColor(StringView key, int index, StringView itemKey, StringView colorName);

    // ── イベント取得 ──
    bool clicked(StringView key) const;
    bool changed(StringView key) const;
    template <class T>
    Optional<T> value(StringView key) const;           // cycle/spin/toggle の現在値（キー不在時 none）

    Optional<int> listClicked(StringView key) const;                           // アイテムクリック
    Optional<int> listClicked(StringView key, StringView itemKey) const;       // アイテム内ボタン

    // ── 色バインド ──
    void setColor(StringView key, StringView colorName);

    // ── 描画 ──
    void draw();
};
```

### 使用例: 車両パネル（動的に複数生成）

```cpp
// 車両選択時 — まだ無ければ生成
void GameScene::onVehicleSelected(int vehicleId)
{
    const String instanceId = U"vehicle_{}"_fmt(vehicleId);
    if (!m_vehiclePanels.contains(vehicleId))
    {
        auto& panel = m_panelManager.create(U"vehicle_info", instanceId,
                                            U"Vehicle #{}"_fmt(vehicleId));
        m_vehiclePanels[vehicleId] = &panel;
    }
    m_vehiclePanels[vehicleId]->show(Vec2{100, 200});
}

// 毎フレーム更新
void GameScene::updateVehiclePanel(int vehicleId, const Vehicle& veh)
{
    auto* ui = m_vehiclePanels[vehicleId];

    ui->bind(U"type", typeNames[static_cast<int>(veh.type)]);
    ui->bind(U"speed", veh.speed * 3.6f);
    ui->bind(U"location", locNames[static_cast<int>(veh.location)]);
    ui->setCondition(U"hasSelectedEdge", m_selectedEdgeId.has_value());
    ui->setActive(U"track", m_trackingVehicle);

    // イベント処理
    if (ui->clicked(U"track"))
        m_trackingVehicle = !m_trackingVehicle;

    ui->draw();
}

// パネルを閉じた時
void GameScene::onVehiclePanelClosed(int vehicleId)
{
    m_panelManager.destroy(U"vehicle_{}"_fmt(vehicleId));
    m_vehiclePanels.erase(vehicleId);
}
```

### 使用例: 単一パネル（エッジ情報）

```cpp
// メンバ変数
PanelInstance* m_edgePanel = nullptr;

// initScene() で生成済み。表示切替と更新のみ。
void GameScene::drawEdgePanel()
{
    if (!m_selectedEdgeId)
    {
        m_edgePanel->hide();
        return;
    }

    RoadEdge* edge = m_network.getEdge(*m_selectedEdgeId);
    if (!edge) { m_edgePanel->hide(); return; }

    m_edgePanel->setTitle(U"Edge #{}"_fmt(*m_selectedEdgeId));
    m_edgePanel->show(Vec2{800, 100});

    m_edgePanel->bind(U"info", edge->nodeA, edge->nodeB, edge->length);
    m_edgePanel->setCondition(U"hasSelectedEdge", true);

    if (m_edgePanel->clicked(U"swap_ab"))
    {
        std::swap(edge->nodeA, edge->nodeB);
        // ...
    }

    m_edgePanel->draw();
}
```

### 描画パイプライン

`PanelInstance::draw()` の内部処理:

1. バインド値・条件を元にウィジェットツリーを走査
2. `visible` が false の要素をスキップ
3. `enabled` が false の要素はグレーアウト描画 + 入力無視
4. vstack/hstack のサイズ決定ルールに従いレイアウト計算
5. 各ウィジェットタイプに対応する描画関数を呼ぶ
6. カスタムウィジェットは登録済みの描画コールバックを呼ぶ
7. イベント（クリック・ホイール等）を記録

### イベントのライフサイクル

`clicked()` / `changed()` が返すイベントは **`draw()` 内でセットされ、次の `draw()` 冒頭でクリアされる**。

```
フレーム N:
  bind() ...          ← データ設定
  draw()              ← イベント記録（このフレームのクリック等）
  clicked("btn")      ← フレーム N のイベントを取得 ✓

フレーム N+1:
  bind() ...
  draw()              ← フレーム N のイベントをクリア → 新しいイベントを記録
  clicked("btn")      ← フレーム N+1 のイベントを取得 ✓
```

`draw()` の前に `clicked()` を呼んだ場合は前フレームのイベントが残っている。これは仕様として許容するが、通常は `draw()` の後に呼ぶことを推奨する。

### フォーマットエラーの安全性

`bind()` で渡された値と XML の `text` フォーマット指定（`{:.1f}` 等）の型が不一致の場合、`fmt` の実行時エラーでクラッシュする恐れがある。これを防ぐため、フォーマット処理は `try-catch` で囲み、失敗時はフォーマット文字列をそのまま表示する。

```cpp
// 内部実装イメージ
try {
    result = Fmt(formatStr)(args...);
} catch (...) {
    Console << U"[UI] Format error: key='{}' format='{}'"_fmt(key, formatStr);
    result = formatStr;  // フォーマット文字列をそのまま表示
}
```

### ウィジェット固有の一時状態

折りたたみ状態、ドラッグ状態、選択インデックス等の「ウィジェット固有の一時状態」は `PanelInstance` 内の `widgetStates: HashMap<String, Variant>` に `key` 単位で保持する。

- `<section>` の折りたたみ → `widgetStates[key] = bool`
- `<cycle>` / `<spin>` の現在値 → `widgetStates[key] = int / float`
- カスタムウィジェットの内部状態 → コールバック内で `panel.setState(key, value)` / `panel.getState<T>(key)` でアクセス

### カスタムウィジェット間の状態共有

同一パネル内の複数カスタムウィジェット（例: `<signal_phase_list>` と `<signal_diagram>`）が状態を共有する場合は、`PanelInstance` 経由で間接的にやり取りする。

```cpp
// signal_phase_list のコールバック内
panel.setState(U"selectedPhase", selectedPhaseIndex);

// signal_diagram のコールバック内
int phase = panel.getState<int>(U"selectedPhase").value_or(0);
```

### カスタムウィジェットの登録

```cpp
// 初期化時にカスタムウィジェットの描画コールバックを登録
m_panelManager.registerCustomWidget(U"road_section_editor",
    [this](PanelInstance& panel, const UINode& node, RectF area) {
        // C++ で自由に描画
        drawRoadSections(...);
    });

m_panelManager.registerCustomWidget(U"signal_diagram",
    [this](PanelInstance& panel, const UINode& node, RectF area) {
        drawSignalDiagram(...);
    });
```

### エラーハンドリング

**方針: エラーはログに出して、該当要素をスキップする。無理なリカバリーはしない。**

エラーは全て `Console` に出力する。パネル全体が壊れている場合はそのパネルを描画しない。個別ウィジェットのエラーはそのウィジェットだけスキップする。

| エラー | 挙動 |
|---|---|
| XMLファイルが見つからない / パース失敗 | `Console << U"[UI] Failed to load: {}"` 。パネル自体を無効化 |
| 未知のエレメント名 | `Console << U"[UI] Unknown element: <{}>"` 。その要素をスキップ |
| 必須属性の欠落（`key` 等） | `Console << U"[UI] Missing required attr '{}' in <{}>"` 。その要素をスキップ |
| `width` と `stretch` の同時指定 | `Console << U"[UI] width and stretch conflict in '{}'"` 。`width` を優先、`stretch` を無視 |
| `<list>` に `<template>` がない | `Console << U"[UI] <list key='{}'> has no <template>"` 。リストをスキップ |
| `<template>` が複数ある | `Console << U"[UI] <list key='{}'> has multiple <template>"` 。最初のものを使用 |
| 属性値の型エラー（数値に文字列等） | `Console << U"[UI] Invalid value '{}' for attr '{}'"` 。デフォルト値を使用 |
| `bind()` のフォーマット型不一致 | `Console << U"[UI] Format error: key='{}'"` 。フォーマット文字列をそのまま表示 |
| `value<T>()` のキー不在・型不一致 | `none` を返す（`Optional<T>` を使用） |
| バインドキーが未設定のまま描画 | テキストの `{}` をそのまま表示（フォーマット未置換） |
