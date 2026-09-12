# 鉄道システム仕様書

日本の鉄道を簡略化した路線・ダイヤ・閉塞システムの定義。
道路渋滞と連動する踏切、単線行き違いなどを再現する。

---

## 1. 線路種別

```cpp
enum class TrackGauge : uint8 {
    Narrow,       // 狭軌 1067mm（JR在来線・多くの私鉄）
    Standard,     // 標準軌 1435mm（新幹線・一部私鉄）
    ThreeRail,    // 三線軌条（狭軌・標準軌の双方が走行可能）
};

enum class Electrification : uint8 {
    NonElectric,  // 非電化（ディーゼル・蒸気）
    Electric,     // 電化（直流・交流の区別はゲーム内では省略）
};
```

> 三線軌条は主に標準軌路線が既存狭軌区間に乗り入れる場合に使用。
> ゲームでは「電化 or 非電化」のみ管理し、電圧・周波数の違いは省略する。

---

## 2. 線路データ構造

### TrackNode（線路ノード）

道路の RoadNode に相当。分岐・合流・単純通過の拠点。

```cpp
struct TrackNode {
    int     id;
    Vec3    position;
    bool    isStation;      // 駅かどうか
    bool    isSignalBox;    // 信号所かどうか
    bool    isDepot;        // 車両基地かどうか
    int     facilityId;     // 駅/信号所/基地の ID（-1 = 通常ノード）
};
```

### TrackEdge（線路区間）

```cpp
struct TrackEdge {
    int             id;
    int             nodeA, nodeB;
    Vec3            ctrlA, ctrlB;   // ベジェ制御点（道路と同形式）
    float           length;         // 弧長 [m]
    TrackGauge      gauge;
    Electrification elec;
    int             lineId;         // 所属路線 ID（-1 = 未所属）
    int             planId;         // 建設計画 ID（-1 = 既存）

    // 閉塞区間（後述）
    int             blockId;        // 所属閉塞区間 ID
};
```

---

## 3. 施設

### 3-1. 駅（Station）

```cpp
struct Platform {
    int     index;          // ホーム番号（0始まり）
    TrackGauge gauge;       // 対応軌間
    float   length;         // ホーム有効長 [m]（編成長を超えると停車不可）
    bool    isAvailable;    // 使用可能か（工事中 = false）
};

struct Station {
    int             id;
    String          name;           // 駅名（地名生成ルールに従う）
    Vec3            position;
    Array<Platform> platforms;

    // ダイヤ集計（到着・発車リスト）
    // → Timetable から参照される
};
```

### 3-2. 信号所（SignalBox）

行き違い設備のある駅に準じた施設。旅客の乗降なし。

```cpp
struct SignalBox {
    int   id;
    Vec3  position;
    // 接続する TrackNode が「行き違い可能」として扱われる
    bool  hasCrossover;   // 単線→複線の行き違い設備あり
};
```

### 3-3. 車両基地（Depot）

編成を駐留・整備する施設。編成はここに所属する。

```cpp
struct Depot {
    int          id;
    Vec3         position;
    TrackGauge   gauge;
    Array<int>   formationIds;  // 所属編成 ID リスト
};
```

---

## 4. 路線（RailLine）

```cpp
struct RailLine {
    int             id;
    String          name;           // 例: 「甲諏電鉄本線」
    Array<int>      stationIds;     // 路線上の駅 ID（順序あり）
    Array<int>      edgeIds;        // 構成する TrackEdge ID
    TrackGauge      gauge;
    Electrification elec;

    Color           lineColor;      // 路線図の表示色
};
```

> 1 つの TrackEdge が複数路線を共有可能（乗り入れ・共用区間）。

---

## 5. 車両・編成

### 5-1. 車両種別

```cpp
enum class RailCarType : uint8 {
    MotorCar,      // 動力車（電車・気動車）
    TrailerCar,    // 付随車
    ControlCar,    // 制御車（先頭車両）
};

struct RailCarSpec {
    String      className;      // 形式名（例: "キハ40"）
    RailCarType carType;
    Electrification elec;       // 電化 or 非電化（形式に紐づく）
    TrackGauge  gauge;
    float       length;         // [m]
    float       maxSpeed;       // [km/h]
    float       capacity;       // 定員
};
```

### 5-2. 編成（Formation）

プレイヤーが管理する単位。個々の車両番号は表示のみ。

```cpp
struct RailCar {
    int         id;
    String      carNumber;      // 車両番号（表示用: "キハ40 2012"）
    int         specId;         // RailCarSpec の参照
};

struct Formation {
    int             id;
    String          name;           // 編成名（例: "C01編成"）
    Array<RailCar>  cars;           // 前→後の順
    int             depotId;        // 所属基地

    // 状態
    FormationState  state;
    // Standby / Running / Maintenance

    // 現在位置（Running のとき有効）
    int     currentEdge;
    float   arcPos;         // エッジ上の弧長位置
    float   speed;          // [km/h]
    float   heading;        // 進行方向

    // ダイヤ
    int     scheduleId;     // 運用スケジュール ID（-1 = 運用なし）
    int     scheduleStep;   // 現在のダイヤ進捗
};
```

> 編成の全長 = `cars` の `length` 合計。ホーム有効長より長い場合は停車不可。

---

## 6. ダイヤ（Timetable）

プレイヤーが列車ごとにどの駅に何時に停車・発車するかを設定する。

### 6-1. データ構造

```cpp
struct StopEntry {
    int     stationId;
    int     platformIndex;      // 使用ホーム
    GameTime arrive;            // 到着時刻
    GameTime depart;            // 発車時刻
    bool    isOrigin;           // 始発（到着なし）
    bool    isTerminus;         // 終着（発車なし）
};

struct Schedule {
    int             id;
    int             formationId;    // 使用編成
    int             lineId;         // 所属路線
    Array<StopEntry> stops;         // 停車駅リスト（順序あり）
    bool            isDaily;        // 毎日運行か（false = 土休日など）
};
```

### 6-2. ダイヤ編集 UI フロー

```
① [施設建設モード] → [鉄道] → [ダイヤ編集]
② 路線を選択 → 当該路線のスケジュール一覧
③ [新規ダイヤ追加]
   ・使用編成を選択
   ・各駅の着発時刻を入力（スプレッドシート形式）
   ・ホームを選択（複数ホームがある駅のみ）
④ [保存] → 翌ゲーム日から運行開始
```

### 6-3. ダイヤ実行ロジック

```
GameClock.now() が StopEntry.depart に達したとき:
  → 編成を次の StopEntry.stationId へ向けて発車

途中のリアルタイム走行:
  → TrackEdge 上をarcPos で進行
  → 閉塞が取得できない場合は信号所・駅で停車待機

到着時刻に StopEntry.stationId に到達できなかった場合:
  → 遅延として記録（遅延分は以降の stops にも伝播）
  → HUDに「○○線: X分遅延」を表示
```

### 分岐点（ポイント）切替ロジック

複数の TrackEdge が接続する TrackNode（分岐点）では、
ダイヤ（Schedule）の次の StopEntry.stationId をもとに進行方向を決定する。

```cpp
int resolveNextEdge(const Formation& f, const TrackNode& node) {
    // 次の目標駅 ID を Schedule から取得
    int nextStationId = f.schedule.stops[f.scheduleStep + 1].stationId;

    // node に接続する TrackEdge のうち、nextStationId に向かうものを選択
    // → dijkstra（鉄道グラフ版）で最短経路エッジを取得
    return railDijkstra(f.currentEdge, nextStationId).nextEdgeId;
}
```

ポイントはプレイヤーが手動設定する必要はない。ダイヤに従い自動決定する。
ダイヤ未設定の編成（scheduleId == -1）は基地（Depot）へ自動回送する。

---

## 7. 閉塞制御

単線区間での列車衝突を防ぐ行き違い管理を再現する。

### 7-1. 閉塞区間（Block）

```cpp
struct Block {
    int         id;
    Array<int>  edgeIds;        // 構成する TrackEdge
    int         stationA;       // 区間の一端（駅 or 信号所ノード ID）
    int         stationB;       // 区間の他端
    bool        isSingleTrack;  // true = 単線、false = 複線（常に通行可）

    // 占有状態
    int         occupyingFormationId;   // -1 = 空き
};
```

### 7-2. 入線ルール

```cpp
bool Block::canEnter(int formationId, Direction dir) {
    if (!isSingleTrack) return true;    // 複線は常に通行可
    if (occupyingFormationId == -1) return true;    // 空き
    if (occupyingFormationId == formationId) return true; // 自分が占有中
    return false;   // 他の編成が占有中 → 信号所/駅で待機
}
```

### 7-3. 行き違い手順

```
[上り列車が A 駅に進入]
  → Block(A→B).canEnter() = true → 占有
  → A 駅発車 → B 駅に向かう

[下り列車が B 駅に到着]
  → Block(A→B).canEnter() = false → B 駅で停車待機
  → 上り列車が B 駅到着 → Block(A→B).occupyingFormationId = -1
  → 下り列車が入線許可 → B 駅発車

→ 行き違いが成立
```

### 7-4. デッドロック検出

```
2列車が互いの閉塞を占有し合っている場合（閉塞のデッドロック）:
  → 検出時、古い（入線時刻が早い）方を優先
  → 新しい方を直前の駅に後退させ、閉塞を解放
  → HUDに警告: 「○○線: 行き違いに失敗しました」
```

---

## 8. 踏切（LevelCrossing）

道路と線路の平面交差。列車通過時に道路を閉鎖し、渋滞要因となる。

### 8-1. データ構造

```cpp
struct LevelCrossing {
    int     id;
    Vec3    position;
    int     trackEdgeId;    // 交差する TrackEdge
    int     roadEdgeId;     // 交差する RoadEdge
    float   trackArcPos;    // TrackEdge 上の位置
    float   roadArcPos;     // RoadEdge 上の位置

    // 状態
    CrossingState state;
    // Open / Warning / Closed
    GameTime      closedUntil;  // 遮断解除予定時刻
};
```

### 8-2. 踏切の動作

```
列車が踏切まで残り T_warn 秒の距離に到達:
  → state = Warning（警報機・点滅灯が鳴動）
  → T_close 秒後: state = Closed（遮断機下降）
  → 踏切のある RoadEdge の TempOp を適用:
      全車線を Closed に（両方向通行遮断）

列車が踏切を通過:
  → T_open 秒後: state = Open（遮断機上昇）
  → TempOp を解除 → 道路復旧

const float T_warn  = 30.0f;  // 秒（ゲーム内時間）
const float T_close = 10.0f;  // 警報開始から遮断機下降まで
const float T_open  =  5.0f;  // 通過後から遮断機上昇まで
```

### 8-3. 踏切と渋滞

```
・踏切を含む道路区間は実効容量が低下する
  → 道路ネットワーク上で踏切の閉鎖頻度が高い区間は
     congestion 値が高めに評価される

・経路探索への影響:
  → 踏切コストを Transition コストに加算
  costLevelCrossing(crossing) {
      float closureRatePerHour = crossing.dailyTrains * (T_close + T_open) / 3600.0f;
      return closureRatePerHour * kLevelCrossingPenalty;
  }
  const float kLevelCrossingPenalty = 30.0f;  // 秒

・踏切渋滞が慢性化するとプレイヤーは:
  → 立体交差化（高架 or 地下）を建設して解消
  → 踏切廃止 → 迂回路整備
```

---

## 9. 路線建設

### 9-1. 建設フロー（道路計画と同様）

```
① [施設建設モード] → [鉄道] → [路線計画]
② 軌間・電化を選択
③ マップ上でルートをベジェ曲線で描画
④ 駅・信号所・踏切位置を指定
   ・踏切: 道路との交差点を自動検出 → 候補を表示
   ・駅: ホーム数・ホーム長を設定
⑤ 計画確定 → 概算費用・工期を表示
⑥ [着工] → 工事開始（道路と同様に段階的供用）
```

### 9-2. 建設コスト（概算）

| 施設 | コスト |
|-----|--------|
| 単線 線路 | 1億円/km |
| 複線 線路 | 2億円/km |
| 電化設備 | +0.5億円/km |
| 地方駅（1面1線） | 0.5億円 |
| 地方駅（2面3線） | 2億円 |
| 信号所 | 0.3億円 |
| 車両基地 | 5億円 |
| 踏切立体交差化 | 15〜30億円/箇所 |

### 9-3. 車両の購入

```cpp
struct RailCarPurchaseOption {
    int         specId;
    String      displayName;    // 例: "近郊型電車（4両）"
    int         carCount;       // 編成両数
    float       cost;           // 億円
    Electrification elec;
    TrackGauge  gauge;
    float       maxSpeed;
    int         capacity;       // 総定員
};
```

> 購入後、指定した車両基地に配備される。
> 車両番号（例: "クモハ103-1"）はゲームが自動付番し、右パネルの詳細表示で確認可能。

---

## 10. 収支

鉄道は道路と独立した収支を持つ。

```
月次収入 = Σ(路線の利用者数 × 平均運賃)

利用者数の計算:
  → ゾーンの人口 × 駅勢圏係数 × 競合交通機関（バス・車）との分担率
  → 駅勢圏 = 駅から徒歩 800m 以内（ゲーム内距離）

月次支出:
  線路維持費: 0.05億円/km/月
  電化維持費: +0.02億円/km/月
  車両維持費: 0.005億円/両/月
  駅運営費: 0.02億円/駅/月
  運行コスト: 0.001億円/km 走行/編成

鉄道収支が赤字でも道路予算で補填可能（市の総合収支で管理）
```

---

## 11. 他仕様書との連携

| 仕様書 | 連携内容 |
|--------|----------|
| 07_road_lane_spec | 踏切の TempOp 適用・解除 |
| 08_pathfinding_spec | 踏切コストを RoadEdge コストに反映 |
| 04_gameplay_detail_spec | 鉄道収支・イベント（ダイヤ遅延・脱線） |
| 06_ui_spec | 施設建設モード（鉄道カテゴリ）・ダイヤ編集 UI |
| 05_zoning_spec | 駅勢圏内の地価上昇・建物成長加速 |

## 駅・電車の詳細モデル（2026-09-11）

駅ノードはID偶奇でstation_001/002を表示し、接続する有効な最初の線路の接線にホーム長手方向を合わせる。線路中心はモデルX=0・長手Z方向。駅モデルは剛体で、曲線や勾配には変形追従しない。普通・急行にはcommuter_001/002を使用し、+Z前方をheadingに合わせ、車輪接地をレール上面0.17mへ置く。1エージェントにつき1車体の表示で、編成・ドア開閉は追加しない。その他の種別は既存簡易表示。モデルはassets/railway/から初回に読み込みキャッシュする。
