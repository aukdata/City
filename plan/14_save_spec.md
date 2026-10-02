# セーブ・ロード仕様書

将来案としては MessagePack + チャンク分割保存を想定するが、**現行実装（2026-10-02）** は
`JSON + 独自バイナリ` の簡易構成で保存している。

---

## 0. 現行実装（2026-10-02）

### 0.1 ディレクトリ構造

```text
saves/
└── {save_name}/
    ├── meta.json
    ├── global/
    │   ├── economy.json
    │   ├── districts.json
    │   ├── roads.bin
    │   ├── development.bin
    │   ├── construction_clearance.json
    │   └── guide_signs.json
    └── chunks/
        └── {cx}_{cy}/
            ├── terrain.bin
            └── land_patches.bin
```

### 0.2 保存している内容

- `meta.json`
  - `version`, `seed`, `worldChunks`
  - `generation`: 河川・町村・道路・鉄道・建物・農地・自然林・自動車・歩行者の生成選択
  - `gameNow`, `timeScale`
  - `nextNodeId`, `nextEdgeId`
  - `cameraFocusX/Y/Z`, `cameraDistance`, `cameraYaw`, `cameraPitch`
  - `zoneDevelopment`: 編集セルの記録、開発進捗・状態・時刻・再試行時刻。v4は記録のみ復元し、完全復元済みのセルへ編集ペイロードを重ねない
  - `railway`: 線路・駅・車庫のグラフ、路線名・停車駅・運行時間帯・間隔・種別、直前発車と次便の方向
- `global/economy.json`
  - `funds`, `population`, `happiness`
- `global/districts.json`
  - 地区種別、中心座標、半径、スコア、地名、読み
- `global/roads.bin`
  - 道路ノード、道路エッジ、レーン、断面部品、道路標識
  - 道路形式v19はノード位置・ベジェ制御点をdoubleで保存。v13–v18のfloat形式も読めるが、既に失われた精度は復元しない
  - 末尾追記で `RoadObject` と `RoadRoute`
- `global/development.bin`（meta v4、独立スキーマ v1）
  - 全生成済みチャンクの全用途・建物・市街化フラグ。省略セルは明示的な既定値
  - Buildingの全7フィールドと、敷地ID・参照キー・材質・高さ・double頂点座標
  - 固定幅リトルエンディアン。重複座標・不正列挙・非有限値・切詰め・余剰データを拒否
- `global/construction_clearance.json`
  - 撤去済みセルの記録。v4は地形・セルへ再適用しない
- `global/guide_signs.json`
  - **手動編集された案内標識のみ**
  - 自動生成案内標識はロード後に再計算
- `chunks/{cx}_{cy}/terrain.bin`
  - `gridSize(int32)` + `float` ハイトマップ配列

### 0.3 ロード時に再構築している内容

- `NamedDestination` と自動案内標識
- v4の道路は保存したcutoff・ノード種別・接続順序・信号配置・道路標識・施工時間/費用/延長を維持し、レーン接続パス等の派生情報だけを再構築
- v1–v3の自動道路標識・建物・ゾーンは従来の再生成経路を維持
- v4の建物・ゾーン・敷地は `development.bin` から復元し、初期生成・敷地生成・角度移行・施工地形再適用を実行しない
- 鉄道
  - `meta.railway` がある場合は保存したグラフとダイヤを復元し、再生成しない。保存前の形式で鉄道欄がない場合だけ生成する

### 0.4 まだ永続化していない内容

- 車両の現在位置・経路
- 走行中の列車の位置・停車状態（ロード後は保存ダイヤの次便から再開）
- 各種 UI 状態

### 0.5 保存検証と互換性

meta v4の必須メタデータ・地形・開発スナップショットが欠損/破損した場合は再生成で補わず失敗を表示する。書込みは一時スロットを再読込み検証してから公開する。DevelopmentSnapshot単体の失敗時非変更はテスト済みだが、全GameSceneの部分ロードまで汎用トランザクション化したわけではない。

旧保存は従来経路で読むため、以前のロードで失われた生成建物は復旧できない。新形式は古い実行ファイルで開かない。旧バイナリの安全な拒否は保証しない。検証は [保存](../artifacts/save_consistency/REVIEW.md) と [道路復元](../artifacts/road_snapshot/REVIEW.md)。

### 0.6 一覧と削除

タイトルの保存一覧はmeta.jsonを持つセーブを列挙し、ロードと名前指定の削除に対応する。削除前に対象名を確認する。一覧を再取得しても確認対象を番号で取り違えない。失敗理由を画面に表示する。ロード時は生成選択を復元し、無効にした農地・建物や自動交通を勝手に生成しない。詳しくは [33](33_generation_and_underground.md)。

以下の章は中長期の理想設計として残す。

---

## 1. ディレクトリ構造

```
saves/
├── {save_name}/           ← 手動セーブ（任意数・任意名）
│   ├── meta.msgpack
│   ├── global/
│   │   ├── economy.msgpack
│   │   ├── placenames.msgpack
│   │   ├── railway.msgpack
│   │   └── road_plans.msgpack
│   └── chunks/
│       ├── 0_0/
│       │   ├── terrain.msgpack
│       │   ├── roads.msgpack
│       │   ├── buildings.msgpack
│       │   ├── zones.msgpack
│       │   └── vehicles.msgpack
│       ├── 0_1/
│       │   └── ...
│       └── ...
│
└── autosave/              ← オートセーブ（3世代ローテーション）
    ├── slot0/
    │   ├── meta.msgpack
    │   ├── global/
    │   └── chunks/
    ├── slot1/
    └── slot2/
```

---

## 2. 各ファイルの内容

### meta.msgpack（セーブルート直下）

セーブ全体のメタ情報。ロード画面に表示する情報もここから読む。

```
{
  version: uint32,           // セーブフォーマットバージョン
  gameName: string,          // セーブ名（プレイヤーが設定）
  mapSeed: uint64,           // マップ生成シード値
  terrainType: uint8,        // 地形タイプ（4種）
  gameMode: uint8,           // 通常 / サンドボックス
  savedAt: uint64,           // 保存日時（Unixタイム）
  playTime: uint64,          // 累計プレイ時間 [秒]
  gameYear: int32,
  gameMonth: uint8,
  gameDay: uint8,
  gameHour: float,
  timeScale: uint8,          // 最後の時間速度（×1/×2/×4）
  population: uint32,        // サムネイル用（表示専用・正値は economy.msgpack）
  funds: int64,              // サムネイル用（表示専用・正値は economy.msgpack）
  visitedChunks: [{x, y}],   // 訪問済みチャンク座標リスト
}
```

### global/economy.msgpack

> `funds` はここが正値。meta.msgpack の `funds` はロード画面表示専用。

```
{
  funds: int64,                  // 資金 [円]（正値・float32 では精度不足のため int64）
  monthlyIncome: int64,          // 月次収入
  monthlyExpense: int64,         // 月次支出
  incomeHistory: [int64 × 24],   // 過去24ヶ月の収支
  happiness: float,
  congestionPoints: uint32,
  avgCommuteTime: float,         // 平均通勤時間 [ゲーム分]
  // 交付金制度（税率なし。交付額は人口規模から自動計算）
  // → 保存すべきプレイヤー設定パラメータなし
}
```

### global/placenames.msgpack

```
{
  settlementNames: [{id, name}],
  facilityNames:   [{id, name}],
  roadNames:       [{id, name}],
  azaMap:          [{id, name, bounds}],
}
```

### global/railway.msgpack

TrackEdge はチャンクをまたぐため、線路ネットワーク全体をグローバルで保存する。

```
{
  trackNodes:  [TrackNode],   // 全線路ノード
  trackEdges:  [TrackEdge],   // 全線路エッジ（gauge・elec・planId含む）
  lines:       [RailLine],
  formations:  [Formation],
  schedules:   [Schedule],
  depots:      [Depot],
  stations:    [Station],
  signalBoxes: [SignalBox],
  blocks:      [Block],
  levelCrossings: [LevelCrossing],  // 踏切（道路×線路交差）
}
```

### global/road_plans.msgpack

仕様上は `road_plans.msgpack` を想定するが、現行実装（2026-04）は `global/roads.bin` の末尾追記領域に `RoadRoute` と `RoadPlan` を保存する。

```
{
  nodes:   [RoadNode],      // 全ノード
  edges:   [RoadEdge],      // 全エッジ（Lane配列・TempOps・PlannedChange含む）
  plans:   [RoadPlan],      // 全計画（未着工・工事中・完成）
  signals: [TrafficLight],
}
```

### chunks/{cx}_{cy}/terrain.msgpack

> N = 64（チャンク 1024m ÷ 16m/セル）。ゾーン格子と同解像度。

```
{
  cx: int32, cy: int32,
  heightmap:   [float × 64×64],   // ハイトマップ格子値 [m]
  surfaceType: [uint8 × 64×64],   // 地表種別
  dirty: bool,                    // 地形変更ありフラグ（掘割・造成・埋立）
  // トンネルのレンダリングメッシュ穴あけはロード時に
  // road_plans.msgpack の RoadEdge 情報から再実行するため保存不要
}
```

### chunks/{cx}_{cy}/buildings.msgpack

Building 構造体の正式定義は `05_zoning_spec.md` を参照。

```
{
  buildings: [
    {
      id:          int32,
      type:        uint8,    // BuildingType enum（05参照）
      position:    {x, y, z},
      rotation:    float,    // ヨー角 [ラジアン]
      builtYear:   int32,    // 建設ゲーム内年
      zoneType:    uint8,    // 建設時のゾーン種別
    }
  ]
}
```

### chunks/{cx}_{cy}/zones.msgpack

```
{
  cells: [uint8 × M×M],   // ゾーン種別（16m×16mセル）
  urbanArea: [bool × M×M] // 市街化区域フラグ
}
```

### chunks/{cx}_{cy}/vehicles.msgpack

車両はチャンクをまたいで移動するため、**現在位置が属するチャンクに保存**する。

```
{
  vehicles: [
    {
      id:            int32,
      type:          uint8,
      hazardous:     bool,
      goalEdgeId:    int32,        // 目的地エッジ（再探索時に必要）
      routeNodeIds:  [int32],
      routeProgress: int32,
      currentEdge:   int32,
      currentLane:   int32,
      arcPos:        float,
      speed:         float,
      heading:       float,
      position:      {x, y, z},
      state:         uint8,
      sirenActive:   bool,
      scheduleId:    int32,        // バス・緊急車両の運用スケジュール（-1=なし）
      departedAt:    double,       // 出発時刻 [GameTime]（平均通勤時間計算用）
    }
  ]
}
```

---

## 3. オートセーブ

```
頻度: リアル時間 5分ごと
方式: slot0 → slot1 → slot2 → slot0 ... の3世代ローテーション

autosave/
  slot0/  ← 最新
  slot1/  ← 1世代前
  slot2/  ← 2世代前

保存時:
  slot2 を削除
  slot1 → slot2 にリネーム
  slot0 → slot1 にリネーム
  新規セーブを slot0 に書き込む

UIでの表示:
  meta.msgpack の savedAt（Unixタイム）と現在のリアル時刻を比較して表示
  「オートセーブ (5分前)」「オートセーブ (10分前)」「オートセーブ (15分前)」
```

---

## 4. 手動セーブ

```
保存名: プレイヤーが任意に設定（デフォルト: "セーブ {YYYY年MM月}"）
文字数制限: 最大20文字
上書き: 同名フォルダが存在する場合は確認ダイアログを表示
削除: ロード画面から削除可能（確認ダイアログあり）
```

---

## 5. チャンクの差分保存

### 「訪問済み」の定義

```cpp
// Chunk 構造体に追加
bool visited;  // 一度でもアクティブ範囲に入ったか

// visited = true になるタイミング（いずれか一つでも満たせば）:
//   1. チャンクがアクティブ範囲（カメラ周辺 5×5）に入った
//   2. 道路・建物・地形変更が当該チャンクに及んだ
//      （カメラ外での長距離道路建設も含む）
```

### 保存ルール

```
保存対象:
  visited == true のチャンク → chunks/{cx}_{cy}/ に保存
  visited == false のチャンク → 保存しない（ロード時にシードから再生成）

terrain.msgpack の省略:
  visited == true かつ terrain.dirty == false
  → terrain.msgpack を省略可能（シード再生成と同一結果のため）

  terrain.dirty == true（地形変更あり）:
  → カメラ未訪問であっても必ず保存する
  → 道路建設時に影響チャンクを即座に visited = true かつ dirty = true にセット

チャンクがアンロードされるとき（カメラが遠ざかりアクティブ範囲外になったとき）:
  変更があれば chunks/{cx}_{cy}/ を更新して書き出し
  → ゲーム中に随時書き込むため、クラッシュ時のデータ損失を最小化

トンネルのメッシュ穴あけ:
  terrain.msgpack には保存しない
  → ロード時に road_plans.msgpack の RoadEdge（トンネル区間）から
    ブーリアン穴あけを再実行して復元する
```

### ファイルサイズ目安

```
terrain.msgpack（64×64、float+uint8 × 2配列）: 約 40KB/チャンク
buildings.msgpack（建物 100棟想定）:           約 5KB/チャンク
zones.msgpack（64×64 × 2配列）:               約 10KB/チャンク
vehicles.msgpack（車両 20台想定）:             約 3KB/チャンク
road_plans.msgpack（全体）:                   約 1〜5MB

ディスク容量の警告: 残り 500MB 未満のとき「ディスク容量が少なくなっています」を表示
手動セーブの最大件数は設けない（容量警告で代替）
```

---

## 6. セーブ・ロードの処理フロー

### セーブ

```
① 一時停止（時間を止める）
② 一時フォルダ（saves/.tmp_{name}/）に書き出し開始
③ meta.msgpack・global/ を書き出し
④ アクティブチャンクを順に書き出し
   （非同期で書き込み、完了まで進捗バーを表示）
⑤ 全ファイル書き出し完了 → 一時フォルダを正式フォルダにリネーム
   （途中失敗時は一時フォルダを削除してロールバック）
⑥ 完了通知 → 再開

ウィンドウ終了操作を検知したとき:
  非同期セーブが進行中なら完了まで待機（最大10秒）
  10秒を超えた場合は「セーブが完了しませんでした」を表示して終了
```

### ロード

```
① タイトル or ゲーム内メニューからセーブを選択
② meta.msgpack を読み込み → マップシード・地形タイプを取得
③ global/ を読み込み（道路・経済・鉄道・地名）
④ カメラ初期位置周辺のチャンクを読み込み
   ・chunks/{cx}_{cy}/ が存在する → ファイルから復元
   ・存在しない → シードから再生成
⑤ 車両を各チャンクの vehicles.msgpack から復元
   routeNodeIds が現在のグラフに存在しない場合は再探索をキューに積む
⑥ ゲーム開始
```

---

## 7. バージョン管理・互換性

```
meta.version でフォーマットバージョンを管理する。

ロード時:
  meta.version == CURRENT_VERSION → そのまま読み込み
  meta.version < CURRENT_VERSION  → マイグレーション処理を実行
  meta.version > CURRENT_VERSION  → 「このセーブは新しいバージョンで作成されました」と警告

マイグレーション:
  バージョンごとの migrate_vX_to_vY() 関数を実装
  古いフィールドの補完・新フィールドのデフォルト値設定
```

---

## 8. エラー処理

```
書き込み失敗（ディスク容量不足など）:
  → 部分書き込みを残さずロールバック（書き込みは一時フォルダに行い、完了後にリネーム）
  → HUDに「セーブに失敗しました」を表示

読み込み失敗（ファイル破損・欠損）:
  → 対象チャンクをシードから再生成して代替
  → 代替した場合は「チャンクデータが破損したため、このエリアの地形変更が失われました」を警告表示
  → global/ の破損は「セーブデータが破損しています」と警告してタイトルへ
```

### 2026-09-12: 道路工事の段階描画と撤去

実装済みの着工・自動撤去・通行制御・実写材質・橋脚の段階形状・撤去履歴の保存は `23_road_construction_spec.md` を参照。

### 2026-09-12: 旧道路ジオメトリの移行

meta.json に `roadGeometryVersion: 1` を保存する。キーが存在しない旧セーブはロード後、建物配置・道路参照復元前に共有道路区間を統合する。処理は読み込んだネットワーク上で実行し、ロードだけでは元セーブを書き換えない。ユーザーによる保存後は再移行を省略する。削除された旧道路への建物参照は有効な接道候補から復元する。

### 2026-09-12: 都市の形成計画

`global/districts.json` に `morphologyVersion: 1` と計画の起源・農村形態、街区軸、範囲、旧中心、駅、公共・産業用地、沿道型フラグ、散村の居住点を保存する。読み込み時も同じ計画を復元するため、ロード後に土地利用の判定が別の街へ変化しない。`SettlementKind` は規模（RegionalCity / LocalTown / RuralSettlement）を表し、歴史的起源は独立した値とする。新しい計画の往復保存を検証する。旧セーブの景観互換性は対象外。

## 城下町外縁・耕作道の現行保存形式（2026-09-13）

`global/districts.json` の `morphologyVersion=2` では、地区ごとの `fringe_i` 件数と `fringeAX_i_j / fringeAZ_i_j / fringeBX_i_j / fringeBZ_i_j` に、外縁街路を地区座標で保存する。旧版でキーがない場合は空配列とする。道路そのものは既存の道路保存系で復元する。

田畑の不整形ポリゴンは `chunks/x_y/land_patches.bin` に保存する。新規生成の農道は道路として保存し、道路の高さフラグのbit3に `farmAccess` を記録する。旧 `FarmTrack` / `IrrigationDitch` の列挙値は残すが、新規農道は土地パッチで生成しない。既存セーブの移行処理は行わない。

当面の開発優先度は新規生成の景観と街のつながり。既存セーブの互換性・移行の拡充を優先しない（ユーザー指定、2026-09-13）。
