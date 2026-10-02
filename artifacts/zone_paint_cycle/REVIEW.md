# Native one-cell zone paint / erase / save review

2026-10-02. Runtime source: `4d3358a`. No gameplay defect was found in this bounded cycle. No C++ or UI behavior was changed; the only specification change corrects the current save-format description in [05_zoning_spec.md](../../plan/05_zoning_spec.md).

## Scope and native results

The original `lane_ui_review` save was preserved. The app operated on the disposable `zone_review` copy, with simulation paused throughout. A single empty cell was selected at world center `(23208, 26552)`, global cell `(1450, 1659)`, chunk `(22, 25)`, local cell `(42, 59)`.

| Stage | Cell zone | Pending plots | Building | Result |
|---|---:|---:|---:|---|
| Before | Agriculture (6) | 0 | None (0) | Original empty cell |
| Native one-cell paint | LowResidential (2) | 1 | None (0) | Only the selected cell changed |
| Native erase | Unzoned (0) | 0 | None (0) | Pending development canceled |
| Save and actual reload | Unzoned (0) | 0 | None (0) | Erasure persisted |
| Native agriculture repaint | Agriculture (6) | 1 | None (0) | Original visible zone restored |
| Close zoning tool | Agriculture (6) | 1 | None (0) | `editMode=0`, `zonePalette=false` |

Evidence: [native_summary.json](native_summary.json). Brush radius was explicitly changed from its default 1 to 0 before each one-cell operation. Both app sessions exited with code 0. The local logs contain atomic-save success at log timestamps `[49.4628s]` and `[43.6247s]`; those are timestamps, not measured save durations.

The restored save is **not a byte-identical full save**. Explicitly repainting the empty agriculture cell correctly creates one pending agriculture plot and an edited-cell record in `meta.json`. The original metadata had no pending plot. Both saved states retain `timeScale=0`, `completed=0`, and the same development clock `26.80000000000011`; the restored plot has progress 0.

## Authoritative snapshot comparison

The existing `build-tools/latest/inspect-development-snapshot.py` and the production schema in `src/save/DevelopmentSnapshot.hpp/.cpp` were checked before adding [compare_development.py](compare_development.py). The comparator reads the files only, validates headers/counts/truncation/EOF, treats omitted zone cells as Unzoned, and compares building and full parcel-record bytes exactly. Floating-point fields and polygon coordinates are not rounded or regenerated.

- [saved_erased_comparison.json](saved_erased_comparison.json): exactly one zone changed, `(1450,1659): 6 -> 0`; all 4096 chunk identities and urbanization flags match. All 37,274 building records, 56,075 land patches, and 267,554 polygon vertices are bit-exact unchanged. The file shrank by exactly one 3-byte zone record, from 13,825,657 to 13,825,654 bytes.
- [saved_restored_comparison.json](saved_restored_comparison.json): the entire authoritative `global/development.bin` is byte-identical to the original. SHA-256: `f8a91e9d9d7a68e0a4d09c08d76ee534859eae2d019320ae1f1efd13e9bcbad9`.
- [comparator_checks.json](comparator_checks.json): syntax check plus five checks passed: identity, expected one-cell erase, restored identity, rejection of the erased file as identical, and rejection of truncation.

Reproduce from the repository root while the local raw snapshots remain available:

```sh
python artifacts/zone_paint_cycle/compare_development.py \
  ../build-tools/latest/road-lane-cycle/App/saves/lane_ui_review/global/development.bin \
  artifacts/zone_paint_cycle/saved_erased/global/development.bin \
  --expect-zone 1450,1659,6,0
python artifacts/zone_paint_cycle/compare_development.py \
  ../build-tools/latest/road-lane-cycle/App/saves/lane_ui_review/global/development.bin \
  artifacts/zone_paint_cycle/saved_restored/global/development.bin \
  --expect-identical
```

Full save copies, full state dumps, and debug logs remain local and are excluded from the evidence commit. The compact JSON summaries and comparator are retained.

## Existing control contract and coverage limits

- Z opens/closes zoning; 0 selects Unzoned; 2 selects LowResidential; 6 selects Agriculture. Space controls simulation pause. In zoning mode, 0 does not pause the game.
- The palette's `1区画` brush is radius 0. The default small brush is radius 1 and affects nine cells; the large brush affects 21 cells.
- Left hold paints. Shift+left drag supports inclusive-cell rectangles, but rectangle painting was not exercised in this cycle.
- **Zoning undo/redo is not supported.** Existing undo/redo controls belong to road planning. No zoning undo feature was added or claimed tested.
- Paint/erase does not automatically demolish existing buildings or remove parcels. This empty-cell cycle verified that all saved buildings and parcels remained unchanged; it did not directly exercise painting an occupied cell.
- Time remained paused. Building growth, progress after resuming, wider brushes, and terrain/frontage suitability were not tested here.
- Existing `Zoning.*`, `DevelopmentSnapshot.*`, and `Input.SaveChord.*` tests cover related contracts, but were not rerun for this documentation/evidence-only change. No C++ build was needed or claimed.

Sources: `GameScene_Input.cpp::handleZonePaint`, `GameScene_Zoning.cpp`, `ZonePalette.cpp`, `ZoneDevelopment.cpp::paintCell`, `GameScene_Storage.cpp::writeGameSnapshot/loadGame`, and `DevelopmentSnapshot.hpp/.cpp`.

The specification edit replaces only the stale save paragraph: version 4 restores authoritative full zone/building/parcel state from `global/development.bin`; `meta.json::zoneDevelopment` restores bookkeeping and matching empty pending plots without overwriting that world. UTF-8 BOM and CRLF were preserved, and the scoped diff passed whitespace validation with CRLF recognized.
