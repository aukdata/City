# 実写比較（長野道・山間国道・地方都市・渓谷の道）2026-10-31

## 撮影方法

`./City --capture-photos --capture-folder photos --low-spec --window-size 960 600 --seed 42 --new`

- 15視点を自動で探し、FOV 50°、曇天0.85、紅葉0.8、13時で `App/Screenshot/photos/` に保存した。地形と道路設備の非同期生成が6フレーム続けて空になってから保存する（上限160フレーム）。
- 環境: Linux、llvmpipe（ソフトウェア描画）。1視点あたり約20〜60秒。

## 結果（seed 42）

| 視点 | 結果 | 写真との比較 |
|---|---|---|
| 00 expressway_ahead | 撮影 | 曇天、W形ガードレール、路肩の舗装、出口予告の緑の板、遠景の山並み |
| 01 expressway_exit | 撮影 | IC手前の本線照明（両車線）と出口案内 |
| 02 expressway_powerline | 本線を横切る送電線なし（撮影なし） | 送電線そのものは8路線・346基を生成。前回の撮影では本線をまたぐ架空線を確認した |
| 03 expressway_noise_barrier | 撮影 | 透光板の遮音壁と照明 |
| 04 expressway_embankment | 撮影 | 法面の外から撮る構図。盛土の斜面が画面の大半を占め、写真ほど道路が見えない |
| 05 expressway_information_board | 撮影 | 黒地の情報板が本線の先に見える。960×600では文字は判読できない |
| 06 expressway_spacing_check | 撮影 | 車間確認の緑の板 |
| 07/08 mountain_tunnel_* | 一般道のトンネルなし | seed 42 の道路網には山中の一般道トンネルがない |
| 09 mountain_bridge | 不採用 | 駅構内の高架道路を選んだため比較から外した |
| 10 mountain_road | 撮影 | 尾根型ノイズの山腹、紅葉、電柱8本線 |
| 11 valley_road / 12 town_arterial | 該当なし | 両岸が12m以上高い谷の2車線道路、4車線以上の幹線がない |
| 13 gorge_road | 撮影 | 川沿いの道は選べたが平野部。国道41号のような渓谷の道は seed 42 にない |
| 14 river_meander | 撮影 | 平野の本流と支流が蛇行する。蛇行前はほぼ直線だった |

比較画像の控えは project files の `photo-match/`。

## テスト

`Test/App` に `shaders` が無いと、樹木シェーダーを読む `MountainRoad.DesignGradePavementVisibility` が失敗する。`App/shaders` へのリンクを置くと通る（環境側の問題）。
結果は PR 本文に記載する。

## 既知の差と次の作業

- 渓谷の道（国道41号の飛騨川沿い）: 吹付法面・法枠は高い切土に付く。ただし seed 42 には山中の川沿い道路がなく、撮影で確認できていない。山地の川に沿う国道を生成する規則と、対岸の鉄道・落石防護網・覆道は未実装。
- 高速道路の両側に並ぶ樹木列が写真の田園より多い。
- 遠景の地域地形は128m格子で、地平線付近が明るく見える。
