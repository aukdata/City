# Realistic town pack — sources and licenses

Created for City on 2026-09-11. Powered by [Poly Haven](https://polyhaven.com).

## Downloaded texture assets

All three downloaded albedo textures are released under **CC0 1.0** by Poly Haven. See [Poly Haven's asset license](https://polyhaven.com/license) and [CC0 legal text](https://creativecommons.org/publicdomain/zero/1.0/legalcode).

| Asset | Author | Source | Local original |
|---|---|---|---|
| Grey Plaster 02 | Rob Tuytel | https://polyhaven.com/a/grey_plaster_02 | `App/assets/third_party/polyhaven/grey_plaster_02/grey_plaster_02_diff_1k.jpg` |
| Wood Planks Grey | Rob Tuytel | https://polyhaven.com/a/wood_planks_grey | `App/assets/third_party/polyhaven/wood_planks_grey/wood_planks_grey_diff_1k.jpg` |
| Brick Wall 11 | Rob Tuytel | https://polyhaven.com/a/brick_wall_11 | `App/assets/third_party/polyhaven/brick_wall_11/brick_wall_11_diff_1k.jpg` |

The original JPG files are stored unchanged. Each asset folder contains `SOURCE.json` recording author, page URL, exact download URL, license, retrieval date, and SHA-256. A combined record is [REALISTIC_MATERIALS.json](REALISTIC_MATERIALS.json). The downloader also checks the MD5 provided by the official API. This script is a local authoring tool, not an API connection in the shipped game.

## Reused asset

`App/assets/third_party/polyhaven/concrete/concrete_diff_1k.jpg` (Concrete, Rob Tuytel, Poly Haven, CC0 1.0) was already present in the repository. Its original provenance remains in [THIRD_PARTY_ASSETS.md](THIRD_PARTY_ASSETS.md), with source https://polyhaven.com/a/concrete.

## Derivatives distributed with the models

The photographic colors are tiled in metre-space, adjusted for the painted finish, combined with original procedural materials/sign lettering, and baked with local geometry ambient occlusion into `*_diffuse.png`. The resulting files are placed alongside each listed OBJ in `App/assets/buildings/residential/` or `commercial/`. Exact scope: [realistic_manifest.json](../buildings/realistic_manifest.json).

The models, original procedural colors, geometry, and sign letter designs were created for this repository. No third-party building model, logo, font file, or architectural drawing was copied. Model geometry follows the repository owner's distribution policy. This record does not relicense unrelated repository files.

`artifacts/realistic_town/realistic_town.blend` contains the same meshes and packed image resources for editing. Preview lighting, cameras, and gallery floor are authoring aids and are not exported to the game. Python, NumPy, Pillow, and Blender executables are not distributed as part of the assets.

## Relation to the first pack

[ORIGINAL_TOWN_ASSETS.md](ORIGINAL_TOWN_ASSETS.md) describes the original first-pack procedural source and historical preview. Its statement that no external textures were used applies to that earlier version. The current game versions of those seven models have now been rebaked with the CC0 materials described here.

## 2026-09-12: 旧モデルの詳細化

residential_001〜010 と office_001〜002 を、このパックの住宅・集合住宅・事務所の寸法バリエーションへ更新。元モデルのUV、ベイク済みPNG、素材の権利表記を引き継ぐ。新規の外部素材は使っていない。生成元とスケールは `scripts/refine_legacy_buildings.py` に記録し、元データは `artifacts/city_realism/legacy_asset_baseline/` に保存。
