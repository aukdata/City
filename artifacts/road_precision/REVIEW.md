# Double-precision road snapshot

2026-10-02 UTC. Follow-up to exact-road-restoration commit 419a92f. Verification complete for precision/timing preservation; no remote push.

## Evidence and implementation

- With cutoff, signs and attachment ordering restored, v18 save files survive two reloads byte-for-byte. The original two overlap diagnostics remain at world coordinates around 58 km
- ParcelRoadIndex samples full Bezier geometry and does not use cutoffs. The v18 codec narrowed double node/control coordinates to floats; this can move sampled road geometry by millimeters
- RoadBinary v19 writes node position and both Bezier control triples as individual doubles. Other fields retain their format. Header remains 30 bytes
- Version-aware reads and second-pass skips retain versions 13–18's float layouts. Nonfinite input coordinates and truncated triples are rejected
- New data costs 12 extra bytes per node and 24 per edge. Existing v18 files cannot recover precision already lost
- Construction-fixture regression exposed a separate overwrite: readGlobal recomputed its stored 100-second duration as 9.612504272460937 seconds. Snapshot mode now preserves stored plan cost/length/duration/name; index and edge-link reconstruction remain. Legacy mode retains recalculation

## Verification

- Independent codec/skip and plan-statistics reviews completed
- Precision regression covers non-float-representable coordinates near 58 km, exact node/control/Bezier values, a nonempty trailing road object, node/control truncation, nonfinite source rejection and an actual retained v18 construction fixture
- First test pass failed only on the real construction-duration overwrite; fixed without changing the 100-second expected value
- Final City + CityTests build passed; DevelopmentSnapshot 8/8, Construction 14/14, SharedTransport 9/9 passed
- Fresh seed42 v19 generation and two reload/save cycles exited 0, preserving 37,274 buildings and zero missing parcel/frontage/edge references
- Every save file is byte-identical across both reloads, including v19 roads and authoritative development state
- The two overlap diagnostics still change from 0 at generation validation to 2 at load validation. Double-coordinate preservation did NOT resolve that diagnostic discrepancy; the precision-loss hypothesis is not established as its cause
- Overlap building locations: chunk (57,51), cell (17,38), frontage edge 17575; and cell (15,39), frontage edge 17763. Their coordinates are retained in runtime_summary.json
- Runtime evidence: runtime_summary.json; original copies under the isolated precision-roundtrip run are retained

Road-plan viaPoints remain float encoded. Signal connection identity and legacy-save precision recovery are outside this change. The fresh v19 run established that exact coordinate preservation alone does not remove the two overlap diagnostics. Loading and re-saving old v18 coordinates cannot recover precision already lost.
