# 公共施設・交通モデル 第3弾

2026-09-11。15種類のオリジナル形状をBlender 4.5で面取り・UV展開し、実写素材と局所AOを焼き込んだOBJとして追加。

| 用途 | モデル | 主な造形 |
|---|---|---|
| 駐車場 | parking_001 / 002 | コインパーキング／ソーラーカーポート、区画線、車止め、精算機、駐車車両 |
| 警察署 | public_005 | 多層庁舎、受付、窓枠、外部設備 |
| 交番 | public_006 | 小規模詰所、入口庇、赤色灯、看板 |
| 消防署 | public_007 | 車庫シャッター、執務室、屋上設備 |
| 市役所 | public_008 | 庁舎、正面玄関、庇、旗竿 |
| 駅 | station_001 / 002 | 地方駅／郊外駅、ホーム、屋根、跨線橋 |
| 自動車 | sedan / kei_wagon | セダン／軽ワゴン、タイヤ、窓、灯火、ミラー |
| 緊急車両 | patrol_car / fire_engine | パトカー／消防車、赤色灯、消防用機材・はしご |
| バス | city_bus | 路線バス、側窓、扉、車輪 |
| 電車 | commuter_001 / 002 | 通勤電車2種、扉、台車、パンタグラフ、屋上機器 |

建物は `buildings/commercial/`、道路車両は `vehicles/`、駅・電車は `railway/`。各モデルはOBJ・MTL・1枚の拡散色PNGで構成する。テクスチャは1K、駅・電車は2K。合計103,185三角形。個別の寸法・三角形数は [civic_transport_manifest.json](civic_transport_manifest.json)。

## ゲームへの組み込み

- 公共施設の選択候補を8種、駐車場を2種に拡張。既存のセル座標による選択を使用。公共施設の描画高さ上限15m、駐車場4.5m。駐車場の古い簡易装飾との二重描画を回避。
- 近距離の乗用車・軽自動車・バス・緊急車両に専用モデルを使用。緊急車両の外観はIDの偶奇で選ぶ。遠景は従来の簡易表示。
- 駅ノードに駅舎・ホームを表示。接続線路の接線方向へ向け、IDの偶奇で2種を選択。普通・急行の電車表示を専用モデルに変更。
- 車両はY上・+X前方、電車はY上・+Z前方、施設はY上・-Z正面。メートル単位、接地位置Y=0。線路中心は駅モデルのX=0、Z方向。電車はレール上面分0.17m持ち上げて表示。

これは外観の拡張。警察・消防の出動、個別の行政サービス、駐車場の入出庫、電車の編成・ドアアニメーションは追加していない。駅は剛体モデルなので急曲線・急勾配への追従はしない。建物の選択候補数が変わるため、既存セルの外観が変わる場合がある。

## 編集・検証

編集用: `artifacts/civic_transport/civic_transport.blend`（画像パック済み）。プレビュー: 同ディレクトリの `contact_front.jpg` / `contact_back.jpg`。Blenderプレビューは生成街の実ゲームレビューとは別。

再生成: `scripts/build_civic_transport.py` → Blenderで `scripts/finish_realistic_town_blender.py -- --work civic_transport` → Blenderで `scripts/preview_civic_transport.py` → `scripts/verify_realistic_town.py --work civic_transport`。

ライセンスと素材の加工内容は [CIVIC_TRANSPORT_ASSETS.md](licenses/CIVIC_TRANSPORT_ASSETS.md)。
