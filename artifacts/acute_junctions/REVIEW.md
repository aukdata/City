# 鋭角交差の描画修正

## 原因

- `RoadNetwork::updateNodeCutoffs` は、鋭角に開く2本の道路の口を離すための延長カットオフを、交差点の全接続道路へ適用していた。直進側の道路まで数十 m 交差点扱いになり、区画線のない広いアスファルトと無地の歩道が続いていた。
- 鋭角の内側では、両側の歩道・縁石が楔の中で重なりうる。

## 修正

- 延長カットオフは鋭角を成す2本だけに適用し、他の腕は通常のカットオフを保つ。
- `JunctionGeometry` の各断面に、楔の二等分線までの到達距離 `limit` を持たせ、歩道・縁石帯と地形切り抜きをそこで止める。両側の帯が二等分線で接し、一つの島になる。

## 検証（Linux / llvmpipe）

- 新規 `RoadRendering.AcuteJunctionReview`：24°Y字、25°合流（生活道路）、30°斜め交差、25°合流（生活道路同士）、40°急カーブを俯瞰・斜めで撮影し `Test/App/Screenshot/acute_junction_*.png` に保存。直進側のカットオフが通常値であることを確認。PASS。
- `--filter Road`：111件すべて PASS（RoadIntegrity 33件、RoadRendering 3件を含む）。
