# Village gutter and residential entry access

2026-10-02. Test-first ground-view work. Village covered-gutter production integration passed its six scoped regression gates; entry-path acceptance is still pending.

## Measured baseline

The retained Village street has two2.5m logical lanes,0.35m shoulders,0.32m drainage strips and no sidewalk, for6.34m structural width. Its `roadside_gutter_concrete` asset reused the proper curb OBJ with Y=0..0.15m. The renderer adds this mesh elevation directly, so `height_offset=0` did not make the gutter flush. The visible dark continuous ridge was a real15cm extrusion, not a perspective estimate.

The fixed ground camera is eye(32106.4203385,21.5,33107.4087904),1.5m above the existing flat20m generation fixture, on retained old-village edge675. The150m view submits32 buildings. A Test-only700m-square content crop retains exact generated terrain/buildings/parcels and the full road network;81 active chunks become2 prepared chunks. Original chunk/grid/building/patch fingerprints restore exactly. This is a cropped actual-renderer generation fixture, not a full-app playtest or performance benchmark. The empty horizon, absent traffic and disabled shadow pass are fixture limitations.

## Gutter candidate and evidence

A separate covered-gutter asset retains0.32m width with topY=0 and4mm recessed joints. It has18 detail triangles and6 LOD triangles, no new texture or material pipeline. Its1m authored module is resampled by the existing road renderer, so exact1m visible joint spacing is not promised.

The approved profile scope is Village sides with no sidewalk only. Proper raised curbs, original shared gutter assets, lane/shoulder/structural widths, utility-pole positions and all other profiles stay unchanged. An explicitly added Village sidewalk retains its original gutter/curb treatment on that side.

The original per-directory-symlink Test asset overlay bypassed LOD lookup: canonical source paths resolved outside the physical overlay assets root and the existing safety fallback selected full models for both LOD tiers. That run hit the resource guard and produced no accepted ground image. It was not a gutter geometry failure. A separate physical copy/reflink asset tree with distinct file inodes restored identical canonical LOD selection; no production LOD code or guard changed.

- Covered-gutter mesh/registry test: passed
- Original raised-curb preservation: passed
- Production profile-scope test: intentionally red before integration, then green after approved Village-only wiring
- Canonical physical-overlay LOD preflight: passed
- Same-camera guarded candidate preview: passed, RSS about1.73GiB, no resource guard hit
- Before/after eye and building submission count identical;546 selected old-village gutter parts changed in the Test-only road copy
- Original protected assets/settings/profile hashes unchanged before production installation

Images and reports: `../realism_gutter_physical_preview/UrbanStructure.GroundGutterPreview/`. The candidate image was visually inspected and accepted. The lighter flush drainage strip improves the raised street edge; broad bare frontage still needs a continuous entry treatment. No public-pocket or facade improvement is claimed from this pair.

Normal-path production regression: all six scope/geometry/raised-curb/LOD/street-cross-section/asset filters passed in `../realism_diagnostic43/`. The original raised curb and non-Village profiles remain covered by explicit controls.

## Entry-path stage, not yet accepted

The staged helper targets verified ordinary residential door transforms and the two existing paired-house apron connections. It rejects unsupported model metadata and other road-side profiles. A1.2m strip must lie within its own convex parcel except at most1.5m of the assigned frontage gap; neighboring parcels/buildings and transport masks are excluded exactly. These are bounded game design dimensions, not asserted Japanese standards.

The proposed filled ramp joins the actual covered-gutter elevation with a1cm seam allowance to the rendered step/apron top. Ground/yard ownership determines the underlying surface lift. Exact parcel-boundary crossing samples supplement the full-width terrain samples. Longitudinal/cross-width grade, burial and excessive fill are rejected. A per-chunk spatial index avoids scanning all nearby plots for every path. Side-boundary props and extra planting are deferred until continuity is visually checked.

Pending: compiled ownership/door-transform/ramp/production-contact tests, emitted Float3 winding/area checks, nonzero nearby accepted-path count, exact-camera visual review, and affected renderer regressions. Nothing in this section is a completed visual claim.

## Official reference

[Matsumoto City: privately executed work on city streets](https://www.city.matsumoto.nagano.jp/site/benricho/3256.html), inspected2026-10-02, explicitly distinguishes property entrances, roadside-gutter cover work and sidewalk/curb modifications. This supports modeling accessible covered drainage separately from a proper raised pedestrian curb. It does not specify the game asset dimensions above, nor authorize any real-world construction.

### Entry validation progress

The five focused ownership/door-transform/ramp/production-contact/Float3-coverage cases passed in diagnostic46. The ownership helper originally compared two separately triangulated polygon areas; the rotated local polygon and its hull differed by1.14e−5 despite being convex. Convexity now uses local-double edge turns. The returned rotated strip has double-coordinate area9.59999999993544m²; reconstructing a world-origin Float3 polygon reports9.583282470703125m². Intended ownership geometry is therefore measured in local double coordinates, while emitted Float3 winding, positive area and complete projected edge coverage retain their own explicit tests.

The guarded normal-path entry preview passed at about1.72GiB RSS and emitted19 accepted paths within150m of the unchanged eye. The main image still submits32 buildings and restores the original world fingerprint. Compared with the gutter-only image, only227 pixels differ (225 by more than3 channel levels), so this wide intersection view does not establish a strong visible improvement. A bounded oblique close-up of a recorded emitted path is pending, with its exact building cell/edge identity and emitted endpoint heights. The broader bare-ground/planting limitation is still open.


### Accepted bounded frontage checkpoint

The oblique close-up visibly shows the covered gutter, continuous filled ramp and actual rendered entrance. It uses the same cropped production geometry/materials, with the eye1.5m above the assigned street. The selected source is edge738, building cell(31,32,20,21), parcel8725724280132884, model `residential_014`. The emitted entrance differs from the target transform by0.72mm; the road-end top is about1cm above the nominal covered-gutter top (authored joints can be4mm lower). The original wide view is retained and is not replaced by the close-up as a whole-street comparison.

Versioned [evidence JSON](evidence/) records the exact camera, endpoints/IDs, building identity and restored world fingerprint. PNG capture labels `gutter_before`, `gutter_after`, `entry_wide` and `entry_closeup` are retained locally and are not included in the repository; the permanent Test fixture reproduces them. The final chunk-domain safeguard rejects a strip whose full bounds leave the complete resident transport-mask domain; seam cases are intentionally left unchanged rather than assuming adjacent road masks exist. Diagnostic48 passed all56 focused tests, including ResidentialAccess6/6 and RoadIntegrity33/33. Eight additional urban/core cases also passed, for64/64 final focused live-tree checks. Independent normal-source checkpoint validation is tracked separately.

This is an accepted functional frontage improvement. It does not resolve the broad bare-ground/planting composition, repeated facades, full-city terrain/water appearance, or the14/21 urban quality-matrix limitation. No Windows or full-game performance claim is made.
