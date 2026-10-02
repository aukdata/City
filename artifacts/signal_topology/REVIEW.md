# Generated-city signal controls and persistence

2026-10-02, dot cloud Linux. Final status: both targets build cleanly, 24 focused tests pass, and a freshly generated v20 seed-7 city passes the production-load signal diagnostic: **1,554 signals, 18,932 movements, zero unserved movements, zero stale green references**. Two empty programs correctly use runtime default phases.

## Defects demonstrated before repair

- Untouched fresh seed 42 had 1,453 signal-bearing nodes; 576 had an open/provisional incoming road lane on a `None`-controlled attachment, including 569 junctions with at least three arms. Example node 835 at X=25751.0478, Z=23064.7777: edges 775/2868 were `Signal`, later edge 23198 was `None`. The fourth-arm regression failed six control and 60 green-coverage assertions.
- Initial repair review found manually authored None/Yield/Stop controls would be overwritten by rebuilds. A separate regression failed all 12 preservation assertions, including outgoing-only→incoming-only conversion.
- A production readback of the newly generated v19 seed-7 snapshot exposed 6,435 unserved movements and 6,439 stale green references. Signal programs were saved but their logical movement identities/counter were not.
- The old movement-key packing discarded high edge-ID bits. Edges differing by 65,536 reduced 24 movements to 20 unique IDs. The save/load fixture restored counter 42 instead of 10000 and lost exact custom movement references.

The seed-42 baseline is the untouched `station-order-roundtrip/new/save`, not the manually edited mouse-road fixture. Full affected-node evidence is in `seed42_before_signal_violations.json`.

## Repair

- Existing signals initialize genuinely new road approaches. Prior source **and destination** edge identities distinguish existing outward-only roads; authored controls remain unchanged.
- Surviving green IDs, phase ordering and custom timing remain. Deleted movement references are removed; genuinely new movements receive separate default groups. Intentionally unserved movements are not indiscriminately assigned green. Deletion down to a single arm or zero arms remains saveable and retains intentional all-red phases.
- Full four-integer movement keys replace truncated bit packing. ID exhaustion is explicit rather than signed overflow.
- Global road format **v20** requires an `LCID` identity trailer. Every live node, including non-signal/zero-movement nodes, stores its exact next counter and ordered `(ID, fromEdge, fromLane, toEdge, toLane)` records.
- Loading reconstructs the complete topology in saved attachment order, validates one-to-one full-key matches, and restores IDs/order/counter plus authored signal state in both load modes. Derived paths remain reconstructed from current geometry.
- Source identity validation happens before opening/truncating the destination. The reader rejects missing/truncated/unknown trailers, duplicate IDs/keys/nodes, unrelated-node substitution, invalid counters/references, movement-set mismatch and extra trailing data.
- Existing append-style load semantics remain: a failed load may have installed graph data; callers must discard failed loads. This is not a failure-atomic loader redesign.

Independent read-only review identified and checked the dead-end, exact-node-membership and required-tail corrections. C++ files retain UTF-8 BOM + CRLF; the edited connection source's pre-existing LF was repaired to the required convention.

## Final verification

| Check | Result |
|---|---|
| City + CityTests build | Success, no warnings/errors |
| Signal growth, authored controls, identity roundtrip, dead-end save, high edge IDs | 5/5 |
| Minor-road signal policy | 1/1 |
| Development snapshot | 9/9 |
| Shared road/rail transport | 9/9 |
| Whole generated v20 snapshot through production loader | 1/1; 18,932/18,932 movements served, 0 stale references |
| Original `railway_review` preservation | All 8,199 files SHA-identical; main settings file remains absent |

The roundtrip regression includes custom phase/green-ID ordering, repeated membership, 37/11-second timings, an intentional 19-second all-red phase, a deliberately unserved movement, reversed asymmetric five-arm attachments, retired ID gaps, an empty node's counter, both load modes, exact byte re-save, and post-load allocation. Malformed/truncated trailers, duplicate keys/IDs, foreign-node substitution, missing empty-graph tails and rejected-write preservation are covered. Existing legacy fixture tests pass.

Evidence: `persistence_green/*/results.json`, `snapshot_diagnostic_v20/{results.json,diagnostic.json}`, `seed7_v20_summary.json`, `seed7_v20/verification.json`, `original-preservation-v20.json`. Earlier red results are retained under `red/`, `red_authored/`, `persistence_red/`, and `snapshot_diagnostic/`.

## Fresh seed-7 generation and scope

The final native audit generated and saved the city, then exited 0 before main-world rendering. Its saved header is v20.

- 46,249 buildings; frontage, missing edge/parcel, road-overlap, distance and coastal counters all zero
- 15,556 irregular fields, zero isolated fields, maximum rural-home distance 499.3769 m
- Grade and radius violations zero; four town centers have no elevated/tunnel streets
- 33,350 edges, 31,967 nodes, 1,554 signals; zero uncontrolled incoming signal approaches
- Two regional road components (29,180/3,624 edges), containing 12,276/639 farm-access edges; five separate rail components

Finite constrained route-search failures remain. This review does not claim universal regional connectivity, redesign road aesthetics, or optimize performance.

The first isolated seed-7 interactive attempt completed generation validation but was killed with exit 137 during its first dense 3D frames, before saving. That failed runtime is preserved. Subsequent audit-only runs used the existing snapshot-before-render path and completed normally; final v20 OOM counters did not increase. **A full dense seed-7 3D visual/playthrough pass is not claimed.** Windows and the entire test suite were not run in this work.

v13–v19 files remain readable and are not rewritten on load, but their missing historical movement identities cannot be recovered by this repair. In particular the demonstrated v19 signal mismatch is not silently migrated or reset to default timings. Exact signal persistence is verified for newly written v20 snapshots. Full v19-to-v20 byte identity is neither expected nor claimed.

## Reproduce without touching existing saves

1. Build using `BUILD_LINUX.md`; run relevant Test filters from `Test/App`.
2. From the repository, prepare a unique runtime: `python3 artifacts/signal_topology/prepare_seed7.py --run-name <unused-name> --audit`.
3. Run the printed command from its isolated `App`: `./City --audit-road-integrity --seed 7 --low-spec --render-distance 100 --new`.
4. Audit `<isolated App>/road_integrity_snapshot` using `artifacts/signal_topology/audit_network.py <snapshot> <output-folder>`.
5. From `Test/App`, run `./CityTests --filter Comprehensive.GeneratedSignalSnapshotDiagnostics --generated-signal-snapshot <snapshot>/global/roads.bin`.

The last diagnostic is opt-in and expects a **fresh generated, unauthored** city. It must not treat a user's intentionally unserved authored movements as generation failures. It checks actual reconstructed movement IDs through `RoadBinary::readGlobal(..., true)`, not only serialized phase counts.
