# 都心の雑居ビル

scripts/build_japanese_streets.py で生成するプロジェクト独自モデル。旧エントリのbuild_downtown_assets.pyからも同じ生成器を呼び出す。メートル単位、Y上、正面-Z。日本語看板を持つjapan_street_atlas.pngを共用する。

| モデル | 階数 | 用途 |
|---|---:|---|
| office_005 | 7 | 小規模オフィス・店舗 |
| office_006 | 12 | 都心オフィス |
| shop_007 | 5 | 飲食・店舗の雑居ビル |
| shop_008 | 7 | 店舗・事務所の雑居ビル |
| shop_009 | 2〜3 | 小さな2店舗と上階住宅 |
| shop_010 | 2〜3 | 飲食・商店の連続した間口 |

看板、袖看板、庇・暖簾、室外機、屋上設備、配管、自販機、メニュー板を含む。600m以遠用の簡略モデルを../lodに同時生成する。写真参照の出典はartifacts/urban_revision/photo_references/REFERENCE_NOTES.md。写真自体とフォントファイルはアセットに含まない。
