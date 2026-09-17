# 道路オブジェクト・高架橋仕様書

2026-09-13：都心/郊外の沿道店舗・田舎の民家、道路種別の線形制限、自動交通、信号と標識モデルの現行仕様は [27 沿道の暮らし](27_roadside_life_spec.md) を参照。

## 概要

橋脚などの道路設備を **RoadObject** で管理する。小型標識は本書末尾の現行仕様、大型の方面標識は21の `GuideSign` を使う。以下のPhase 2などの記載は将来の設計資料であり、標識を汎用オブジェクトへ再統合する実装は要求しない。

初期実装として**橋脚（Pier）**を対象とし、高架道路の自動検出と橋脚の自動配置を実現する。

---

## 1. RoadObject データ構造

### 1.1 RoadObject

```cpp
struct RoadObject
{
    int             id = -1;
    int             parentEdgeId;       ///< 親エッジ ID
    float           arcPos;             ///< エッジ上の弧長位置 [m]
    float           lateralOffset = 0;  ///< 横方向オフセット [m]（道路中心基準）
    RoadObjectType  type;               ///< オブジェクト種別
    float           scale = 1.0f;       ///< スケール倍率
    float           yawOffset = 0.0f;   ///< Y軸回転オフセット [rad]

    // 種別ごとの追加パラメータ
    float           heightOverride = -1.0f;  ///< -1 = 地形まで自動延伸（Pier 用）
};
```

### 1.2 RoadObjectType

```cpp
enum class RoadObjectType : uint8
{
    // Phase 1（初期実装）
    Pier,           ///< 橋脚

    // Phase 2（将来拡張）
    StreetLight,    ///< 街灯
    TrafficSign,    ///< 交通標識
    UtilityPole,    ///< 電柱
    StreetTree,     ///< 街路樹
    SoundBarrier,   ///< 遮音壁
    Guardrail,      ///< ガードレール支柱
    TollGate,       ///< 料金所ゲート
};
```

### 1.3 管理場所

`RoadNetwork` に `Array<RoadObject> m_objects` を追加。エッジと同様に ID 管理する。

```cpp
class RoadNetwork
{
    // 既存
    Array<RoadEdge> m_edges;
    Array<RoadNode> m_nodes;

    // 追加
    Array<RoadObject> m_objects;
    int m_nextObjectId = 0;

public:
    int addObject(RoadObject obj);
    void removeObject(int objectId);
    void removeObjectsByEdge(int edgeId);  ///< エッジ削除時に連動
    const Array<RoadObject>& objects() const;
    RoadObject* getObject(int id);
};
```

---

## 2. 高架道路

### 2.1 高架判定

道路の**路面高さ**と**地形高さ**の差（ギャップ）が閾値以上の区間を高架とみなす。

```
gap(s) = roadSurfaceY(s) - terrainY(s)
```

| 条件 | 状態 |
|---|---|
| `gap < 1.0m` | 地上道路（通常） |
| `1.0m ≤ gap < 3.0m` | 盛土区間（Slope パーツで法面表示） |
| `gap ≥ 3.0m` | **高架区間**（Slope 非表示、橋脚を配置） |

### 2.2 路面高さの決定

現在の実装では道路は常に地形追従している（`makeSlice()` で `computeHeight` を使用）。高架道路を実現するために、**RoadEdge のベジェ曲線の Y 座標を路面高さとして使用する**オプションを追加する。

```cpp
struct RoadEdge
{
    // 既存フィールド ...

    bool useElevation = false;  ///< true: ベジェ Y を路面高さとして使用
                                ///< false: 地形追従（従来動作）
};
```

`useElevation = true` の場合:
- `makeSlice()` で `computeHeight` の代わりにベジェ曲線の Y 座標を路面高さとする
- ノードの Y 座標がそのまま道路の高さになる

### 2.3 橋脚の自動配置

`useElevation = true` のエッジに対して、以下のアルゴリズムで橋脚を自動配置する。

```
入力: edge（高架エッジ）
定数: PIER_INTERVAL = 30.0m, PIER_THRESHOLD = 3.0m

1. エッジのベジェ曲線を PIER_INTERVAL 間隔でサンプリング
2. 各サンプル点で gap = bezierY(s) - terrainY(s) を計算
3. gap >= PIER_THRESHOLD の点に Pier オブジェクトを配置
4. heightOverride = -1（地形まで自動延伸）
```

自動配置は以下のタイミングで実行:
- エッジ作成時（`useElevation = true` 設定後）
- エッジのベジェ曲線変更時（制御点ドラッグ後）

自動配置されたオブジェクトはプレイヤーが個別に移動・削除可能。

---

## 3. 橋脚の描画

### 3.1 メッシュ生成

橋脚は以下の手順で描画する:

1. `arcPos` からベジェ曲線上の位置 P と接線 T を取得
2. 地形高さ `terrainY = computeHeight(P.x, P.z)` を取得
3. 路面高さ `roadY = P.y`（useElevation 時）または `terrainY + terrainLift`
4. 橋脚の上端 = `roadY - 路盤厚（≈0.5m）`、下端 = `terrainY`
5. 接線 T から right ベクトルを計算し、橋脚の向きを決定
6. 直方体または T 型断面のメッシュを生成

### 3.2 橋脚スタイル（将来拡張）

初期実装ではシンプルな直方体コラム（幅 2m × 奥行 1.5m）。

将来:
- `PierStyle::Column` — 円柱（単柱）
- `PierStyle::TBeam` — T 型橋脚（横梁付き）
- `PierStyle::Wall` — 壁式橋脚

### 3.3 描画タイミング

`RoadRenderer::render()` 内でエッジ描画後に、そのエッジに属する RoadObject を描画する。橋脚はキャッシュ可能（エッジメッシュキャッシュと連動して invalidate）。

---

## 4. 高架区間の断面変化

高架区間では断面パーツの表示が変わる:

| パーツ種別 | 地上 | 高架 |
|---|---|---|
| Roadbed | 表示 | 表示 |
| Shoulder | 表示 | 表示 |
| Sidewalk | 表示 | 表示 |
| Curb | 表示 | 表示 |
| **Slope** | **表示** | **非表示** |
| **Wall** | 非表示 | **表示**（高欄として機能） |
| Median | 表示 | 表示 |

→ 同一エッジ内でも区間ごとに gap を判定し、Slope と Wall の表示を切り替える。

---

## 5. セーブ・ロード

RoadObject は `roads.bin` に RoadEdge/RoadNode と同じバイナリ形式で保存する。

```
[Object Header]
  objectCount: int32
[Object Records] × objectCount
  id: int32
  parentEdgeId: int32
  arcPos: float32
  lateralOffset: float32
  type: uint8
  scale: float32
  yawOffset: float32
  heightOverride: float32
```

---

## 6. UI

### 6.1 高架設定

エッジパネルに高架トグルを追加:

```
[✓] Elevated   (useElevation チェックボックス)
```

ON にすると:
1. ベジェ Y 座標が路面高さとして反映される
2. 橋脚が自動配置される
3. ノードの Y 座標をスピナーで調整可能になる

### 6.2 オブジェクト編集（将来）

選択したオブジェクトのプロパティパネル:
- 位置（arcPos スピナー）
- 横オフセット（lateralOffset スピナー）
- 種別表示
- 削除ボタン

---

## 7. エッジ削除時の連動

`RoadNetwork::removeEdge(edgeId)` 実行時に `removeObjectsByEdge(edgeId)` を自動呼び出し。親を失ったオブジェクトは残さない。

---

## 8. 実装フェーズ

### Phase 1（初期）
- RoadObject データ構造 + RoadNetwork への組み込み
- RoadEdge::useElevation フラグ + makeSlice 分岐
- 橋脚自動配置アルゴリズム
- 橋脚メッシュ描画（直方体コラム）
- セーブ・ロード対応

### Phase 2（拡張）
- 橋脚スタイル追加（T 型、壁式）
- 高架区間の Slope/Wall 自動切替
- UI: オブジェクト選択パネル
- OBJ モデルベースのオブジェクト描画

### Phase 3（汎用化）
- StreetLight / TrafficSign / UtilityPole / StreetTree 追加
- パーツに紐づく自動配置ルール（歩道パーツ → 街灯自動配置など）
- LOD 対応

---

## 9. 他仕様書との関係

| 仕様書 | 関係 |
|---|---|
| `16_road_cross_section_spec.md` | §6 橋梁・高架の判定ロジックを本仕様で詳細化 |
| `07_road_lane_spec.md` | RoadEdge への useElevation フィールド追加 |
| `12_visual_spec.md` | §3 道路設備の配置を RoadObject で実現 |
| `14_save_spec.md` | roads.bin に RoadObject セクション追加 |
| `17_road_node_spec.md` | ノード Y 座標の意味が高架時に変わる |

### 2026-09-12: コンクリート電柱

道路付属の電柱は高さ10.2m・下部半径0.18mの12角柱とし、上端を60%へ絞る。2段の腕金、碍子とひだ、柱上変圧器と端子、取付帯、引込管、管理札、昇柱ボルト、弛んだ3本の電線を一体のバッチにまとめる。コンクリート・金属・碍子・電線の材質は分離する。

### 2026-09-12: 道路工事の段階描画と撤去

実装済みの着工・自動撤去・通行制御・実写材質・橋脚の段階形状・撤去履歴の保存は `23_road_construction_spec.md` を参照。

## 小型標識の現行仕様（2026-09-13）

| 表示 | 根拠・配置 |
|---|---|
| 最高速度 | 道路の制限速度をそのまま表示。25を20へ丸めるような表示はしない |
| 一時停止・徐行 | 接続部のStop・Yield制御に合わせる |
| 進入禁止・一方通行 | 進入可能な車線方向に合わせる |
| 指定方向外進行禁止 | 実際の車線接続から直進・左折・右折・転回の許可方向を描く |
| 急曲線・急勾配・幅員減少 | 曲線、5%以上の設計勾配、先の道路が2m以上狭まる接続を予告 |
| 国道・都道府県道番号 | 所属路線の種別と番号。国道はおにぎり、県道は六角形 |
| 道路の通称名 | 所属路線の名前。交差点付近または長区間の沿道に表示 |
| カントリーサイン | `DistrictHierarchy` の市町村界を通過する位置。進む先の市町村名を左右の進入方向ごとに表示 |

番号と名前の根拠は既存の道路・路線・住所データ。標識のためだけの路線名・区域名や新しい規制管理システムは追加しない。国道と県道の同じ番号は別の表示として管理し、路線名の変更も反映する。進行方向の左路肩に配置し、広い板は路面へ張り出さない余裕を取る。曲線・勾配の警戒標識は板自体も菱形にする。

描画実装は `RoadRenderer_Signs.cpp` にまとめ、道路本体・交差点・信号機の描画から分離する。所有者とキャッシュの無効化は既存の `RoadRenderer` に残す。案内標識の通常表示と選択表示は位置・板形状・支柱の生成を共用する。小型標識の合成待ちには種別・数値・文字列だけを持たせ、配置行列は含めない。

小型標識は `RoadSign` に統合する。可変内容のテクスチャは種別・数値・文字列をキーとした一つのキャッシュで共有し、実際に画面へ出た種類だけを合成する。描画要求の次フレームに準備するため、初出時は1フレーム待つことがある。支柱モデルの重複読み込みを廃止して共通のポール生成を使う。道路名・市町村名の板は2.4×0.8m、768×256px。

複数方面・矢印・距離を編集する大型の `GuideSign` は [21](21_guide_sign_spec.md) に分離したままにする。この二系統だけを使い、市町村標識専用の第三の配置・保存システムは作らない。

実物の分類・配置対象は [国土交通省の案内標識の種類](https://www.mlit.go.jp/road/sign/2.html)、[道路標識一覧](https://www.mlit.go.jp/road/sign/sign/douro/ichiran.pdf) を参照。検証は `TransportPlanningTests.cpp` と `--capture-rail-signs`。

## 標識の表裏と曲がり方向（2026-09-15）

小型標識と大型案内標識は local −Z を表面に統一する。UVは右へUが増加、下へVが増加。静的OBJは前面だけを抽出し、面の向き・法線・UVを正規化してから模様を貼る。裏面と20mmの縁は別の無地金属メッシュとし、裏から文字・矢印を表示しない。支柱より接近車側へ板を出し、柱による文字の遮蔽を避ける。

道路の接線と対象車線から接近車へ正面を向ける。`reference/zushu_16_1-16.md` の5-1（道路に直角）と、`reference/06_案内標識_126_警戒標識_201-205.png` の202を参照。屈曲警告は value=1 が左、2が右。同じ道路でも逆向きに走れば左右が逆になる。矢印の先端は曲線の曲がる方向へ向ける。

検証: `MapSigns.*` のカメラ四方向・前後両方向・表裏のGPU読み戻しと、`Planning.GuideSignSelectionMatchesRenderedBoard` の本番描画を使う。

## 高架の橋脚配置

長さ8m以上、地盤から路盤下まで3m以上の高架区間へ、最大30m間隔で橋脚を配置する。短い区間にも中央の1基を用意し、区間長が間隔の倍数に近くても配置が0基にならない。道路に属する橋脚だけを再生成し、他の設備を削除しない。閾値は `assets/generation/roads.json`。

## 細い道路の自動信号（2026-09-17）

生活道路（合計2車線以下）と農道・畦道だけの交差点には、自動で信号を設置しない。四差路で車線接続数が8を超えても無信号とする。幹線・有料道路、または合計3車線以上の一般道が接続する場合に限り、従来の接続数による自動設置を行う。プレイヤーが明示的に置いた信号は保持する。
