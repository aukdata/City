# Original town building assets — provenance

> Historical first-pack record. The current shipped seven models were subsequently rebaked with CC0 photographic materials. See [REALISTIC_TOWN_ASSETS.md](REALISTIC_TOWN_ASSETS.md) for their current provenance. The no-download statements below apply only to the original procedural version.

- Created: 2026-09-11
- Creator: Codex, for this City repository at the user's request.
- Scope: `App/assets/buildings/commercial/{shop_001,shop_002,shop_003,factory_001,factory_002,public_001,public_002}.{obj,mtl,toml}`, `town_atlas.png`, `model_manifest.json`.
- Source of geometry: original parameterized geometry in `scripts/generate_detailed_buildings.py`.
- Source of textures: original deterministic pixel patterns and hand-defined 5×7 letter shapes in the same script. No downloaded image, scanned material, brand logo, or copied architectural model is included.
- Source of editing project: those OBJ assets imported by `scripts/review_buildings_blender.py`, with the atlas packed into `artifacts/building_models/town_buildings.blend`.
- External asset downloads: **none**.
- Third-party asset license obligations for this pack: **none introduced**. Use of these project-created assets follows the repository owner's distribution policy; this record does not relicense unrelated files.
- Tools: Python, NumPy, Pillow, Blender 4.5. Tool executables and font files are not distributed with the game assets. Labels on preview sheets are review annotations, not game textures.
- Existing third-party terrain/road textures are outside this pack and retain their records in `THIRD_PARTY_ASSETS.md`.
