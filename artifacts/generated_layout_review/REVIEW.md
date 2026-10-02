# Seed-7 generated layout review

## Evidence and bounded finding

Read-only review of the v20 seed-7 snapshot in `build-tools/latest/signal-generation-seed7-v20/App/road_integrity_snapshot`.

- 32,804 road edges form two components (29,180 and 3,624); the other 546 edges are separate railway networks
- The smaller road component contains city index 3 at (60092,62564), town 12 at (63164,61356), and mountain villages 51/54
- Closest road nodes across those components are 19,331.65 m apart, not an almost-touching missed junction
- Generation logs show finite constrained searches exhausting their limit for 53.85 km and 41.93 km connections to city 3. This is evidence of unconnected regional roads, not proof that a legal terrain alignment exists or that the current specification guarantees one
- The existing placement/terrain audits report zero frontage, parcel, road-overlap, grade, and radius violations. These counters are not a complete visual review

The concrete defect selected is **ordered route metadata**, separate from physical road connectivity. Saved 国道314号 route ID 2 (798 edges) has adjacent-list gaps 36345→15984 and 14968→14966. The first jumps from near (2378,7944) to (21090,18838); the second appends a side branch at node 14015. `route_continuity.json` retains the full edge-list evidence. `settlement_components.json` maps settlement coordinates to nearest connected road nodes.

`DistrictRoads::reattachRoutesFromNeighbors` appends generated arterial neighbors to a route without preserving its ordered position. Clipping also invokes ordinary road-deletion route fragmentation even when the through-road is immediately replaced with a generated town path. These violate the ordered membership/replacement intent of specification 22. Route names and signs use this membership, so the impact is incorrect route designation, not a phantom drivable road.

## Regression status

`Morphology.OrderedRouteReplacement` uses the existing public settlement generator on a flat fixture with forward/reversed national-route membership. Both directions failed before repair: two fragments remained and the original route stopped short of the opposite original approach (four assertions total). Red evidence: `../generated_route_order/red/Morphology.OrderedRouteReplacement/`.

## Repair and final verification

`DistrictRoads.cpp` now snapshots affected route membership after boundary splits and before deleting interior spans. It restores each clipped span using only the already-generated/promoted arterial corridor. Unrelated neighboring branches receive no route label. Existing route ID/name/number/color survive a connected replacement. When a valid bridge is unavailable, surviving continuous fragments retain the original designation, with the first fragment retaining the original ID. Pre-existing disconnected input spans are not treated as authorization to invent regional links.

- Both City and CityTests build successfully, no warnings/errors in the scoped build logs
- Expanded `Morphology` group: **23/23 pass**
- Forward/reverse through-span fixtures pass; opposite-order overlapping routes retain independent IDs, names, numbers, colors, ordered paths, and reverse membership
- Deliberately unbridgeable civic-reservation fixtures pass in both directions: both original outside edges survive in exactly two correctly styled fragments, with no phantom crossing
- Independent read-only source review identified no must-fix issue; its requested fallback/shared coverage was added and passed
- Fresh audit-only seed 7: **exit 0**, saved snapshot successfully before main-world rendering
- Saved routes: **2 adjacency gaps → 0; 2 whole-ordered-walk violations → 0; duplicate/missing edges remain 0**
- All **33,350** road/rail geometry signatures are exactly identical before/after, ignoring IDs: endpoint positions, Bézier controls, sections/lane data, flags/state, length and cutoffs. No added/removed geometry
- `road_ground_audit.json`, `landscape_audit.json`, and `settlement_audit.json` are structurally identical; 46,249 buildings, zero placement failures; the two regional components remain 29,180/3,624 edges
- Basic serialized signal-control audit remains 1,554 signals with zero uncontrolled incoming approaches. The previous production-load movement diagnostic was not rerun, as this repair does not change signal persistence
- Original `railway_review` saves: all 8,199 files hash-identical; main `App/settings.json` remains absent (`original-preservation.json`)
- C++ changes retain UTF-8 BOM + CRLF, and CRLF-aware whitespace checks pass

Final snapshot: `build-tools/latest/route-order-seed7-final/App/road_integrity_snapshot` (relative to the parent workspace). Final road-route evidence is `after/route_summary.json` and `after/route_continuity.json`; geometry comparison is `geometry_comparison.json`; test evidence is `../generated_route_order/green/Morphology/results.json`.

The original saved city is not edited or migrated. Across separate generations the auto-assigned route number can differ; the fixed fixture explicitly checks preserving an existing route's identity during replacement.

## Visual coverage limit

The desktop owner attempted the existing seed-7 snapshot with lightweight quality, 100 m scenery distance, and initial map input. The process was killed by the OS with exit 137 during its initial dense 3D frame before the map became available. No further native retries or performance changes were made. This review therefore establishes saved-topology/geometry correctness and public-generator regressions, **not a completed seed-7 visual/map playthrough**. Other smaller-city UI checks are documented separately by their owners.

## Reproduce

1. Build and run `CityTests --filter Morphology` from `Test/App` using `BUILD_LINUX.md`
2. Prepare a unique isolated runtime with `python3 artifacts/signal_topology/prepare_seed7.py --run-name <unused-name> --audit`, then run its printed command
3. Run `python3 artifacts/generated_layout_review/audit_routes.py <snapshot> <output-directory>` from the repository. This is read-only with respect to the snapshot; it uses the existing saved-network parser, checks serialized signal approaches, and adds ordered-route membership diagnostics

No saved-city migration, network geometry redesign, performance optimization, or Windows validation is included.
