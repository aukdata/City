# 38地点の再撮影と23地点の景観レビュー

Seed 42、1920×1080。23地点は互いに別の場所で、最短の撮影中心間隔は 114.5 m。生成制約検査は通過。

## 各写真の指摘

| 写真 | 場所 | 残る不自然さ |
|---|---|---|
| [01](../../App/Screenshot/realism_review_20260926_round20/city_render_01_overall.png) | 都市全景 | 郊外まで格子状の街区が均一で、土地利用の境界が急 |
| [03](../../App/Screenshot/realism_review_20260926_round20/city_render_03_close.png) | 市街地近景 | 街区の内部と住宅の間に広い草地が残る |
| [04](../../App/Screenshot/realism_review_20260926_round20/city_render_04_rural_fringe.png) | 田園境界 | 住宅地と農地の境界が直線的で、畑の用途変化が単調 |
| [07](../../App/Screenshot/realism_review_20260926_round20/city_render_07_intersection_zoom_b.png) | 交差点B | 道路の先の区画に未利用の草地が目立つ |
| [08](../../App/Screenshot/realism_review_20260926_round20/city_render_08_intersection_zoom_c.png) | 交差点C | 住宅の配置が同じ間隔で続く |
| [09](../../App/Screenshot/realism_review_20260926_round20/city_render_09_intersection_zoom_d.png) | 交差点D | 建物の間の緑地が所有区画として読み取りにくい |
| [12](../../App/Screenshot/realism_review_20260926_round20/city_render_12_intersection_zoom_e.png) | 交差点E | 商業区画と住宅区画の変化が急 |
| [13](../../App/Screenshot/realism_review_20260926_round20/city_render_13_intersection_zoom_f.png) | 交差点F | 空き区画がまとまって見える |
| [21](../../App/Screenshot/realism_review_20260926_round20/city_render_21_coastal_buffer.png) | 海岸 | 海岸の近くの畑と林が単調に切り替わる |
| [22](../../App/Screenshot/realism_review_20260926_round20/city_render_22_rural_landuse.png) | 田園の土地利用 | 畑の矩形と色の並びが反復的 |
| [25](../../App/Screenshot/realism_review_20260926_round20/city_render_25_satoyama.png) | 里山 | 斜面の植生密度と地形変化が弱い |
| [26](../../App/Screenshot/realism_review_20260926_round20/city_render_26_provincial_town.png) | 地方の町 | 幹線沿い一列に店舗が並び、背後の市街地が疎 |
| [27](../../App/Screenshot/realism_review_20260926_round20/city_render_27_castle_transition.png) | 旧市街から郊外 | 中心部の格子と郊外の境界が明瞭すぎる |
| [28](../../App/Screenshot/realism_review_20260926_round20/city_render_28_fringe_streets.png) | 郊外の街路 | 道路に囲まれた広い空白地が残る |
| [29](../../App/Screenshot/realism_review_20260926_round20/city_render_29_field_access.png) | 農道 | 道の行き先となる農地や施設が少ない |
| [31](../../App/Screenshot/realism_review_20260926_round20/city_render_31_urban_convenience.png) | 都市型コンビニ | 駐車区画線や歩道から店舗への導線が弱い |
| [32](../../App/Screenshot/realism_review_20260926_round20/city_render_32_roadside_convenience.png) | 郊外型コンビニ | 建物の周囲が広い空き地で、利用中の駐車場に見えにくい |
| [33](../../App/Screenshot/realism_review_20260926_round20/city_render_33_urban_fuel.png) | 都市型給油所 | 給油所周辺の敷地と建物の密度が低い |
| [34](../../App/Screenshot/realism_review_20260926_round20/city_render_34_roadside_fuel.png) | 郊外型給油所 | 道路沿いの施設が単独で孤立している |
| [35](../../App/Screenshot/realism_review_20260926_round20/city_render_35_rural_house.png) | 農家 | 一軒家が周囲の農地や集落と結び付いて見えにくい |
| [36](../../App/Screenshot/realism_review_20260926_round20/city_render_36_mountain_river.png) | 山の中の川 | 川面が均一で、瀬や岩など流れの変化が乏しい |
| [37](../../App/Screenshot/realism_review_20260926_round20/city_render_37_mountain_village.png) | 山の集落 | 家は写るが、山麓の集落としては周囲の斜面と家並みが弱い |
| [38](../../App/Screenshot/realism_review_20260926_round20/city_render_38_levee_road.png) | 堤防道路 | 河原と堤防はあるが、堤防道の連続が画面内で分かりにくい |

## この反復で修正した点

- 川の深い切れ込みをなだらかな肩にし、平地の太い川に河原と堤防を追加した。
- 堤防道路を既存道路へ両端接続した4路線として生成した。
- 都心の区画を詰め、農村の家並みと小規模店舗・公園・駐車場の混在を増やした。
- 建物の正面は道路中心線ではなく道路の端までの距離で選び、広い道路を優先した。
- 土地の頂点を追加・移動・削除できる操作を追加し、3点以上と交差防止を検証した。

## 判定

数値上の生成制約と撮影枚数は通過した。上表の景観上の指摘は残っているため、無指摘には達していない。
