# 配送・貨物車モデル

2026-09-13。小型配送車 `vehicles/delivery_truck` と3軸貨物車 `vehicles/cargo_truck` を追加。

`build_neighborhood_vehicles.py` のオリジナル形状を、既存のBlender処理で面取り・UV展開し、車体塗装・ゴム・ガラス・金属の色と局所AOを1枚の1K画像へ焼き込む。外部の写真テクスチャは使用していない。

配送車6,382三角形、貨物車9,143三角形。各モデルは1材質。メートル単位、Y上、+X前方、タイヤ下端Y=0。近距離のSmallTruck / LargeTruckに対応し、遠景は既存の簡易形状。

再生成：`python scripts/build_neighborhood_vehicles.py` の後、Blenderで `scripts/finish_realistic_town_blender.py -- --work neighborhood_realism --vehicle-paint`。編集元と検査値は `artifacts/neighborhood_realism/` に保存。Siv3Dのモデル読込・接地原点・材質・テクスチャを常設Testで検査する。
