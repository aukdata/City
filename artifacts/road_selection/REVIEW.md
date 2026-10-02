# Short-road selection review

Date: 2026-10-02 (UTC)

## Reproduction and cause

A completed 38.333 m road could not be selected by clicking its body. Ground selection searched for a node within 20 m first and only searched for a road edge when no node was found. The midpoint of this road is only 19.1665 m from either endpoint, so the entire center region was claimed by a node. A nearby unrelated node could produce the same obstruction.

The real mouse baseline is preserved in `../road_edit_cycle/before-pick-fix.log`. In the parent playtest, clicking desktop (522, 676), client (518, 548), selected node 19300 rather than short edge 41608.

## Change

- Always collect the existing 15 m road-edge candidate.
- Prefer a nearby node when its projected native-screen position is within 12 px of the cursor.
- Otherwise prefer the actual edge candidate.
- Retain the forgiving 20 m node selection if no edge competes.
- Keep the existing elevated-road override and building/sign/signal depth arbitration from commit `9df723f` unchanged.

Production logic is shared through `src/scene/RoadSelection.hpp` and called by `GameScene::handleSelectionClick()`. The helper does not duplicate network hit testing. The existing nearest-node and nearest-edge searches still supply the candidates.

## Regression evidence

The permanent `RoadSelection` tests in `Test/UrbanUsabilityTests.cpp` use a real 38.333 m RoadNetwork edge and real BasicCamera3D projections at camera offsets of 100 m and 1000 m.

Coverage includes:

1. The center of the short road, within the original 20 m node radius, selects the edge outside the 12 px handle.
2. Endpoint clicks at 11 px retain node priority, while 13 px clicks select the competing edge at both zoom levels.
3. An off-road click 16 m from a node and beyond the edge radius still selects the node.
4. An edge without a node is selected; no candidates produce no selection.

Before changing the helper from old node-first priority, the red run produced 2 tests / 1 failure, with exactly four failed assertions: short-road body and outside-handle picks at both zoom levels. The fallback test passed. Evidence: `../road_edit_cycle/red/RoadSelection/results.json`.

After the 12 px arbitration change, the unchanged fixtures passed 2/2, with exit code 0. Evidence: `../road_edit_cycle/green/RoadSelection/results.json` and `exit.txt`.

The existing building-depth and native-resolution picking regressions also passed 3/3. Evidence: `../road_edit_cycle/green/WorldSelection./results.json`.

The parent ran the combined City and CityTests CMake build successfully before the green fixtures. Build evidence is in the workspace's `build-tools/latest/build-road-edit-green.log` and `road-edit-green.exit`. The live mouse selection and deletion/reload checks are recorded below.

All three changed C++ files retain UTF-8 BOM + CRLF. A scoped `git -c core.whitespace=cr-at-eol diff --check` passes.

## Live mouse verification

The parent repeated the original interaction against the fixed build:

- The exact road-body click at desktop (522, 676), client (518, 548), now selected edge 41608 (`selectionKind=1`, `selectionId=41608`). Evidence: `../road_edit_cycle/fixed_body_pick.json` and `fixed_body_pick.log`.
- After clearing selection with Escape, the endpoint click at desktop (402, 610) selected node 31891 (`selectionKind=2`, `selectionId=31891`). Evidence: `../road_edit_cycle/fixed_node_pick.json`.
- The accessible edge inspector's Delete action removed the road surface and streetlights. The parent then saved through the menu, reloaded, and confirmed the deleted road remained absent. Evidence: `../road_edit_cycle/deleted_reloaded_confirmed.json`.

These checks verify the fixed arbitration in the real mouse path as well as preserving direct endpoint selection and access to the edge's editing controls.
