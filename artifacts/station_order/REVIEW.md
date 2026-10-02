# Stable station footprints across generation and reload

2026-10-02 UTC. Targeted follow-up to the save fixes and the bounded diagnosis in `artifacts/road_precision/`.

## Proven cause and fix

Station 17681 (鵜池) had incident rail edges created in order 24932,24933 but stored in recycled infrastructure slots in order 24933,24932. TrainNetwork::synchronize rebuilds cached incident edges in storage order. Generation validated buildings before the final synchronize; load synchronized before validation.

RailwaySite::stationFrame chose its asymmetric station-building footprint from stationPaths().front(). The changed edge order reversed that footprint and produced the two shop-overlap diagnostics at chunk (57,51), cells (17,38) and (15,39). Those shops themselves were preserved exactly.

stationPaths now sorts a copy of incident edge IDs before traversing. No graph arrays, saved geometry or user data are reordered. The canonical key is the stable road edge ID; no separately authored station-facing direction exists in this path.

## Verification

- Independent source review completed; both edited C++ files retain UTF-8 BOM + CRLF
- Permanent regression creates two reusable road slots, then two station rail edges. It proves that synchronize actually reverses the incident order, and verifies exact station frame and ordered platform/building footprint equality before/after. An explicit reverse is also covered
- Combined Linux City + CityTests build succeeded
- DevelopmentSnapshot 9/9, Construction 14/14, SharedTransport 9/9 passed, 32 total
- Fresh seed42 generation and two sequential load/save cycles all exited 0
- Building counts: 37,274 → 37,274 → 37,274
- At every stage: noFrontage=0, missingEdge=0, missingParcel=0, roadOverlap=0, frontageDistance=0, coastal=0, passed=true
- Every saved file is byte-identical across both reloads, including v19 roads, development, terrain, land polygons, metadata, clock/camera/development state and economy
- The independent geometric inference is now supported by the in-engine reversed-order regression and full fresh/load validation; the prior double-coordinate hypothesis did not explain this particular diagnostic issue

Evidence: `runtime_summary.json` and per-filter `results.json`. Full isolated runtime copies remain in the workspace build-tools `station-order-roundtrip` directory. No existing user save was deleted. No push performed.

## Scope and limits

This proves the seed42 paused roundtrip and the focused regressions, not a full test-suite pass or a Windows build. Old formats cannot recover buildings or coordinate precision lost by earlier saves. Full-city second-seed and edited-city runtime coverage remain outside these runs; edited-world, deleted-frontage, construction and corruption behaviors have permanent focused coverage.
