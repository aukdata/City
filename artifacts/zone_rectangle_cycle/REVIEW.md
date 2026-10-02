# Zoning rectangle release recovery

2026-10-02. Starting revision: `846ccbb`. This change fixes a stale rectangle capture after a release that the terrain/UI input path did not receive. The defect was reproduced by replaying the extracted, behavior-preserving production input transitions. **Native sustained-Shift rectangle interaction remains unverified**; the native attempt was inconclusive and is not evidence of an application modifier bug.

## Defect and bounded fix

Previously, `GameScene::handleZonePaint()` returned before clearing `m_rectStart` when the cursor missed terrain or a panel owned the pointer. Other scene gates skipped the handler over the HUD/header, text input, or minimap. With Shift still held, releasing there retained the anchor. Returning to the world could leave a stale preview; a subsequent press originating over UI and release over the world could paint a rectangle from the old anchor.

`src/ui/ZonePaintInput.hpp` now contains the brush/rectangle transitions used by both the actual scene and replay tests. The dispatcher runs after every ordinary scene input frame, including blocked frames. A valid world release paints first. Once the left button is no longer held, any remaining capture is cleared. There is no speculative painting on a missing release edge.

The existing brush behavior, same-frame Shift click, and holding a rectangle across UI and returning to the world before release are preserved. Esc/tool changes already clear the anchor; no undo feature or new zoning semantics were introduced. The RoadPlan dispatcher remains outermost, so its existing skipped-release cleanup still runs when the minimap blocks the nested zoning/input dispatch.

Production paths: `GameScene.cpp` dispatch; `GameScene_Input.cpp::handleZonePaint`; `ZonePaintInput.hpp::update/dispatchFrame`; `ZoneManager.cpp::paintZone/paintZoneRect`. The preview continues to read `m_rectStart` in `GameScene_Render.cpp`.

## Red reproduction and permanent coverage

The initial extraction retained the original transitions, with no post-dispatch cleanup. The clean CityTests build then ran `Zoning.RectangleInput`:

- 6 tests: 3 passed, 3 failed
- Blocked release: 6 failed assertions across panel/no-ground, disabled minimap dispatch, and early HUD/header/text return
- UI press followed by world release: 3 failed phantom-paint assertions
- Missing release edge: 1 failed stale-capture assertion
- Valid rectangle/brush, held UI crossing, and Shift-release/explicit-reset controls passed

The full compact red result is retained in [red/Zoning.RectangleInput/results.json](red/Zoning.RectangleInput/results.json). This is deterministic replay of production transitions, not a separate test implementation of the paint logic.

The final suite also includes `Zoning.RectangleInput.WorldPaintEraseSnapshotRoundTrip`. It uses the shared input dispatcher with the real `ZoneManager`, paints a 3×3 inclusive rectangle across a chunk boundary, erases its center through the same rectangle path, and writes/reads/verifies the real authoritative `DevelopmentSnapshot`. A later agriculture edit is overwritten by reload. All nine expected zones and the eight remaining pending plots are checked. No serialization helper or alternate save format was introduced.

The explicit-reset replay models the state reset already performed by Esc/tool transitions; it is not a full native keyboard/Esc test.

## Native observations

The two native sessions used the unchanged starting binary and a disposable `rectangle_review` world. Neither session saved its trial edits, and both exited with code 0.

- The attempted Shift-modified drag was sampled as the ordinary radius-1 brush: nine cells changed, creating nine paused pending plots. This did not demonstrate sustained Shift input or rectangle success/cancellation. Modifier delivery and frame lifetime remain an automation limitation to investigate separately; no application modifier defect is claimed. The unsaved edits were discarded. [initial_preservation.json](initial_preservation.json) records all 8,199 disposable-source files unchanged after that session.
- A second session used radius 0 to paint LowResidential at world center `(23208, 26552)`, global cell `(1450, 1659)`. While paused: one Checking plot, progress 0, no building. After 10.6 simulation seconds and pausing again: one NeedsSpace plot (`state=3`), progress 0, completed 0, still no building. The native palette displayed `敷地不足 1`. This verifies the actual blocked-site feedback, not successful building growth.
- NeedsSpace means an eligible nearby surface road was found, but one of the required setback/end clearances or road/building/railway overlap checks prevented placement. The diagnostic state does not identify which precise geometric condition failed. No extra wait or forced building was used.

[Native summary](native_summary.json) contains the compact target-cell/development fields, changed-cell list, source-state hashes, and preservation result. Full state dumps and debug logs remain local and are not included in the commit.

## Final validation

Both final targets, `City` and `CityTests`, built successfully with exit code 0 and no compiler warning/error lines in the build log. Focused checks all exited 0:

- `Zoning`: 16/16 passed, including all seven `Zoning.RectangleInput` cases
- `RoadPlan`: 14/14 passed, protecting the nested dispatcher and prior skipped-release behavior
- `MapTransport.PanelOcclusion`: 3/3 passed
- Total: 33/33 passed

[Test summary](test_summary.json) retains case names, results, red failure messages, result/log hashes, and exit codes. These are focused checks; no full-suite pass is claimed. The rebuilt binary also completed a native tool/brush smoke: Z then Esc returned to `editMode=0`, palette hidden, no pending plots; reopening and ordinary painting worked; a separately selected radius-0 Unzoned brush changed the target cell from zone 2 to 0 and reduced pending plots from 18 to 17; Esc again closed the tool/palette. The simulation stayed paused at development time 26.8 throughout.

The earlier batched brush-selection click and drag in that smoke were sampled before the radius change, so the paint stage used radius 1 and affected 18 cells. That stage is ordinary-brush coverage, not a successful one-cell paint. Separating the selection and subsequent erase verified radius 0. No application defect is inferred from the batched-control limitation. Compact fields are included in [native_summary.json](native_summary.json). The final runtime exited 0, and [final_preservation.json](final_preservation.json) confirms all 8,199 save files remained unchanged. All native trial edits were discarded.

## Scope and remaining limits

- No sustained-Shift native rectangle success or stale-state reproduction is claimed
- No native successful-growth completion is claimed for the blocked site
- No save-format, growth rules, UI layout, zoning undo, or occupied-building demolition behavior changed
- `Test/ZoneDevelopmentTests.cpp` was already BOM+LF; the touched file was normalized to the repository-required BOM+CRLF. Other modified C++ files preserve BOM+CRLF, and both new headers use that format
- The unrelated staged `artifacts/save_consistency_design/REVIEW.md` is excluded from this change
