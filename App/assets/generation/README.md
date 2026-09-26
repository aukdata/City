# 都市生成の設定

このフォルダーの JSON が生成値の正本です。変更後にゲームを再起動し、新しい街を生成してください。再ビルドは不要です。実行中の読み直しや既存セーブの移行は行いません。

`GenerationSettings` が初回に読み込み、以後は同じ不変の値を共有します。毎フレーム、建物ごと、地形のサンプルごとにファイルを読みません。必須項目の欠落、未知の項目、型・範囲の誤り、最小値と最大値の逆転は、項目名付きのエラーになります。

| ファイル | 主な調整対象 |
|---|---|
| terrain.json | 平原の起伏、山脈、海岸の湾・岬、中央の山・湖・湾、ノイズ |
| rivers.json | 集水格子、河川の発生量・幅・河床・河岸 |
| placement.json | 集落の立地適性、中心町と農村の距離、往来費用 |
| settlements.json | 成立史の割合、町割の大きさ、土地利用、街の外側の密度減衰 |
| districtRoads.json | 街区、幹線・生活道路・路地の配置 |
| fringe.json | 都市外縁の生活道路、沿道住宅の伸び方 |
| agriculture.json | ほ場の寸法、耕作道、畦、用排水路、進入口 |
| parcels.json | 敷地の道路離隔、街区面積、建物同士の間隔 |
| development.json | 建物の出現割合、敷地、接道、造成、農地の配置 |
| buildings.json | 建物の占有幅・高さ・収容人口、モデルの選択範囲 |
| vegetation.json | 森林密度、標高・傾斜、樹種の分布、樹木の寸法 |
| streetProfiles.json | 道路種別ごとの車線数・幅・速度、歩道・路肩・標示・電柱間隔 |
| roads.json | 勾配・半径の制限、地上・掘割・高架・トンネルの費用 |
| routing.json | 道路経路の探索・評価の間隔と重み |
| network.json | 地区間道路の探索範囲、接続距離、山道の判定 |
| crossings.json | 橋の水面離隔、取り付け、鉄道と道路の離隔、法面 |
| railway.json | 駅の高さ・直線区間、鉄道路線の経路費用・縦断制限 |
| placeNames.json | 地形別の地名語根の選択重み。語彙本体は既存の地名 TOML |
| pedestrians.json | 住民数、近景・遠景の更新間隔、徒歩探索の予算、駐車在庫、列車定員、描画距離 |
| traffic.json | 車両数、補充速度、建物あたりの交通、時間帯・貨物の割合 |

距離と高さは原則 m、勾配は比率（0.06 = 6%）、速度は km/h、時間は秒です。`Percent` および出現抽選の累積境界は 0–100、それ以外の割合は原則 0–1。名前に `Cells` / `Steps` / `Rows` が付く値は個数です。道路建設費は通貨ではなく相対費用で、標準値は地上1・掘割8・高架64＋高さ[m]×16＋高さの指数罰則・トンネル128＋連続長の指数罰則です。

歩行者は標準10,000人（屋内・乗車中を含む）。500m以内は毎フレーム、遠方の徒歩は4秒ごとに更新します。設定の単位と乗換条件は [歩行者仕様](../../../plan/30_pedestrian_spec.md)を参照してください。

交通の標準上限は12,000台、注視点周辺1,800mには接道建物数に応じて最大2,400台を優先します。全体需要を超えて局所分を別加算する方式ではありません。`maximumVehicles` を上げるだけで必ずその台数が出るわけではなく、人口・用途・時間帯による需要、接道建物、車線の空きが必要です。

建物モデルのファイル、個々のセットバック、信号・道路パーツなどは、それぞれの既存 `assets` 内の OBJ / TOML / JSON を参照します。ハッシュ、配列のインデックス、ベジェ曲線の係数、ワールドのチャンク容量、数値誤差の許容値、メッシュの頂点設計は生成調整値と区別し、コードに残します。

許容型と範囲は `schema/*.schema.json` で確認できます。スキーマは `src/gen/GenerationSettings.def` から生成します。開発時は `python scripts/generation_schema.py --check` と `GenerationSettings` テストで設定の整合性を確認してください。

`roads.json` の `tunnelLengthScale` / `tunnelLengthPenalty` は連続トンネル長の指数罰則、`viaductHeightScale` / `viaductHeightPenalty` は高架高さの指数罰則です。尺度はm、罰則0で追加費用を無効にできます。

`agriculture.json` は農道の分岐間隔・延伸・曲がりと地形変化の費用、不整形区画の大きさ、集落からの密度減衰、田舎の家の間隔を管理します。`homeMaximumDistance` は500mを超えられません。農道断面の幅・側溝は `streetProfiles.json` を参照します。

## 現代都市の構造設定

`urbanStructures.json` は地域中心都市の7類型を定義します。`UrbanStructure::load` が全キー、型・範囲、中心数、建物比率の合計などを検証します。設定は起動中に一度だけ読み込みます。`python scripts/generation_schema.py --check` はこのカタログも検証します。

- `weight` は適地での選択重み。`minimumRelief` / `maximumRelief` は近傍の標高差[m]。`shoreRange` が0より大きい型はその距離[m]以内に水際が必要です。
- `extentX/Z` は街域の半幅[m]。`spacingX/Z` は街路間隔[m]。`cubicWeight` は中央の街区を細かくする度合い。地形制約で半幅は縮小されます。
- `collectorEvery` は集散道路の間隔[街区数]。`staggerEvery` は生活道路の丁字接続周期（0は連続格子）。`ringRatio` は外周連絡幹線の位置/半幅（0はなし）。`crossBoulevard` は長軸以外の中央幹線の有無。
- `centers` のx/zは半幅を1とする地区座標、`radius` は短い方の半幅に対する影響半径。`role` は0業務、1商業、2水際交流。`rail` がtrueなら駅候補。実際に線形制約を満たした候補だけ建設されます。
- `greenAreas` は同じ地区座標で表す公園の矩形。x/zが左上、w/hが幅と奥行き。通過道路は保持し、建物を除外します。
- `backgroundIntensity` は核の外に残る市街地密度。`outerOccupancy` は外縁の建物候補採用率。`coreFrontage` / `outerFrontage` は候補間隔[m]です。
- `urbanFabric.json` の中心街設定は、沿道候補間隔・戸建てと低層集合住宅の比率・建物の周囲に残す敷地余白を調整します。
- `coreHighShare` / `coreOfficeShare` / `coreMidShare` は中心部の高層住宅・業務・中層住宅の配分で、残りは店舗。核から遠ざかるほど高層・業務を減らします。`shoppingOfficeRatio` は商業核の業務配分倍率。
- `innerDetachedShare` / `outerDetachedShare` は核周辺と外縁の戸建て率。残りの住宅に対する中層割合が `housingMidShare` です。
- `coreRadiusRatio` は中心用途の範囲/影響半径。`edgeFadeEnd` / `edgeFadeWidth` は半幅に対する外縁減衰の終端と幅です。

[比較調査](../../../plan/research/2026-09_japanese_city_structures.md)と[仕様](../../../plan/24_japanese_urban_morphology.md)を参照。数値はゲームの調整値で、実在都市の統計値や法的規制値ではありません。

## 都市の拠点施設

`landmarks.json` に高層オフィス、市役所、モール、病院、学校、地下線、ターミナル駅の調整値をまとめます。`footprint_*` / `height_*` はモデルの占有幅・高さ[m]、`*Count*` は都市あたり上限。`mallMinimumRadius` / `mallMaximumRadius` は都市半幅に対する郊外帯、`maximumRelief` は敷地内部の標高差上限[m]です。地下線は `subwayDepth` の土被りと `subwayMaximumDepth` を両方満たす場合に限ります。`towerVariants` は配布済みの1～2種に制限します。[仕様](../../../plan/32_urban_facilities.md)。

## 河川の蛇行

`rivers.json` の集水格子は合流先を決め、完成する河道は格子から移動できます。水位は変形後の流路で再評価します。

| 設定 | 意味 |
|---|---|
| meanderAmplitudeWidths | 蛇行候補の最大移動量/川の全幅。近い河道と集水格子の間隔でも制限する |
| meanderWavelengthWidths | 基準波長/川の全幅。下流へ川幅が変わると波長も連続的に変わる |
| meanderMinimumWavelength | 波長の下限[m] |
| meanderGradeScale | 縦断勾配による蛇行抑制の尺度。最大勾配の制約値ではない |
| meanderSecondaryShare | 異なる尺度の変動の割合。0なら主成分のみ |
| courseBankRiseLimit | 谷底から高い場所を避ける評価尺度と、基準曲線から追加で高い地盤へ出る量の制限[m] |
| courseSearchSamples | 中心から左右それぞれに探索する候補数 |

蛇行は地形に適合する形を作るための近似です。洪水や時間経過による側岸侵食の物理計算ではありません。合流点・河口の共有と下流水位の連続を優先します。
