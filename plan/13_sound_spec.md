# サウンド仕様書

シンプルなアンビエント＋イベント駆動型の2層構成。
カメラ周辺の状況に応じてサウンドレイヤーをミックスして再生する。

---

## 1. 構成

```
┌─────────────────────────────────────┐
│  アンビエント層（常時ループ）         │  ← カメラ周辺の環境音
│  イベント層（状況に応じて発火）       │  ← 緊急車両・工事・踏切など
│  BGM層（オプション）                 │  ← 環境に合わせたBGM
└─────────────────────────────────────┘
```

---

## 2. アンビエント層

カメラ位置周辺の状態をスコアリングし、複数のサウンドをボリューム調整してミックスする。

### サウンド一覧

| サウンドID | 内容 | ループ |
|-----------|------|-------|
| `amb_traffic` | 車の走行音・クラクション（遠め） | ○ |
| `amb_crowd` | 人の賑わい・話し声 | ○ |
| `amb_birds` | 鳥のさえずり | ○ |
| `amb_wind` | 風・草の揺れ | ○ |
| `amb_river` | 川のせせらぎ | ○ |
| `amb_sea` | 波音 | ○ |
| `amb_rain` | 雨音 | ○ |
| `amb_construction` | 工事音（重機・ハンマー・削岩機） | ○ |
| `amb_train` | 列車走行音・レールのきしみ | ○ |
| `amb_station` | 駅のホーム音（アナウンス・乗客） | ○ |

### ボリューム決定ロジック

```cpp
struct AmbientScore {
    float traffic;      // 0〜1
    float crowd;        // 0〜1
    float birds;        // 0〜1
    float wind;         // 0〜1
    float river;        // 0〜1
    float sea;          // 0〜1
    float construction; // 0〜1
    float train;        // 0〜1
    float station;      // 0〜1
};

AmbientScore calcScore(const Camera& cam) {
    Vec3 pos = cam.position;
    float r = 150.0f;  // 参照半径 [m]

    AmbientScore s;

    // 車両密度 → 走行音
    int vehicleCount = countVehiclesNear(pos, r);
    s.traffic = clamp(vehicleCount / 30.0f, 0.0f, 1.0f);

    // 商業・住居ゾーンの密度 → 賑わい
    float commercialDensity = getZoneDensity(pos, r, ZoneType::Commercial);
    float residentialDensity = getZoneDensity(pos, r, ZoneType::Residential);
    s.crowd = clamp((commercialDensity * 1.2f + residentialDensity * 0.6f), 0.0f, 1.0f);

    // 緑地・農業・低密度ゾーン → 鳥・風
    float naturalDensity = getZoneDensity(pos, r, ZoneType::Agricultural)
                         + getZoneDensity(pos, r, ZoneType::Park);
    s.birds = clamp(naturalDensity * 1.5f - s.traffic * 0.8f, 0.0f, 1.0f);
    s.wind  = clamp(naturalDensity - s.crowd * 0.5f, 0.0f, 1.0f);

    // 河川・海の近傍
    s.river = clamp(distanceToRiver(pos) < 80.0f ? 1.0f - distanceToRiver(pos) / 80.0f : 0.0f, 0.0f, 1.0f);
    s.sea   = clamp(distanceToSea(pos) < 200.0f ? 1.0f - distanceToSea(pos) / 200.0f : 0.0f, 0.0f, 1.0f);

    // 工事中エッジが近傍にある → 工事音
    s.construction = clamp(countConstructionEdgesNear(pos, 100.0f) / 3.0f, 0.0f, 1.0f);

    // 線路が近傍にある → 列車音
    s.train   = clamp(distanceToTrack(pos) < 120.0f ? 1.0f - distanceToTrack(pos) / 120.0f : 0.0f, 0.0f, 1.0f);
    s.station = clamp(distanceToStation(pos) < 80.0f ? 1.0f - distanceToStation(pos) / 80.0f : 0.0f, 0.0f, 1.0f);

    return s;
}
```

### 天候・時間帯補正

```
天候:
  雨: amb_rain を加算、amb_birds を 0 に、amb_crowd を 0.5× に
  台風: amb_wind を 1.0 固定、その他のアンビエントを 0.3× に

時間帯:
  深夜（22:00〜5:00）: amb_crowd = 0、amb_birds = 0、amb_traffic を 0.2× に
  早朝（5:00〜7:00）:  amb_birds を 1.5× に（さえずりが強い）
  朝ラッシュ（7:00〜9:00）: amb_traffic を 1.5× に
```

---

## 3. イベント層

特定の状況で単発 or 短ループのサウンドを3D空間上の位置から再生する。

| サウンドID | トリガー | 再生位置 | 減衰距離 |
|-----------|---------|---------|---------|
| `evt_siren_police` | 緊急車両（警察）が sirenActive | 車両位置 | 400m |
| `evt_siren_fire` | 緊急車両（消防）が sirenActive | 車両位置 | 400m |
| `evt_siren_ambulance` | 緊急車両（救急）が sirenActive | 車両位置 | 400m |
| `evt_crossing_bell` | 踏切 state = Warning / Closed | 踏切位置 | 100m |
| `evt_train_horn` | 列車が踏切に接近 | 列車位置 | 250m |
| `evt_train_pass` | 列車が高速通過（80km/h 以上） | 線路位置 | 200m |
| `evt_construction_bang` | 工事進捗が更新されたとき | 工事位置 | 150m |
| `evt_notification` | HUD通知が表示されたとき | UI（2D） | — |

### 3D 音源の減衰

```cpp
float calcVolume(Vec3 sourcePos, Vec3 camPos, float maxDist) {
    float dist = distance(sourcePos, camPos);
    if (dist >= maxDist) return 0.0f;
    return 1.0f - (dist / maxDist);  // 線形減衰（シンプル優先）
}
```

---

## 4. BGM層（オプション）

ゲームプレイを邪魔しない控えめな環境音楽。ON/OFF 設定可能。

| 状況 | BGM の雰囲気 |
|-----|-------------|
| 通常（昼・市街地） | 穏やかな和風ポップ |
| 通常（昼・農村） | のどかなアコースティック |
| 夜 | 静かなピアノ・環境音楽 |
| 渋滞深刻（渋滞ポイント 10箇所以上） | やや緊張感のあるBGM |
| イベント発生中 | BGMを一時フェードアウト |

> BGM はループ素材を用意するだけでよい。クロスフェード時間: 3秒。

---

## 5. 一人称視点時のサウンド

```
車両追従中:
  エンジン音（VehicleType に応じた音色）をフォアグラウンドで再生
  速度に応じてピッチ変化
  外部アンビエント音は 0.3× に抑える

徒歩（一人称・車両乗車なし）:
  足音（アスファルト or 砂利 or 草）を歩行に合わせて再生
  アンビエント音はフル音量
```

---

## 6. 設定項目

```
マスターボリューム:  0〜100（デフォルト 80）
BGMボリューム:      0〜100（デフォルト 50）
効果音ボリューム:   0〜100（デフォルト 80）
アンビエント音量:   0〜100（デフォルト 70）
BGM ON/OFF:        トグル
```
