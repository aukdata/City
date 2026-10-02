# Preserve authoritative road geometry on load

2026-10-02 UTC. Separate follow-up to the development snapshot fix (cc6643e).

## Confirmed issue

Fresh seed42 v4 loading changed 8,916 persisted edge cutoff values and 1,099 automatic sign lists. Maximum observed cutoff change was 1.660798 m. RoadBinary rebuilt edges incrementally through addEdgeRaw, which recalculates junction cutoffs, then both RoadBinary and GameScene regenerated automatic signs.

## Implementation

- Opt-in RoadBinary::readGlobal preserveSnapshot flag, default false for existing/legacy callers
- Validate authoritative cutoffs as finite and nonnegative before graph installation
- Restore stored cutoff values, node classifications, original attachment order and explicit signal placement/absence after raw topology construction
- Decode the previously discarded stored node type
- Refresh only lane-connection Bezier paths after restoring all cutoffs; preserve connection IDs and structure
- Bypass both automatic-sign regeneration calls on the authoritative v4 path
- Existing legacy reconstruction remains available

## Verification

- Independent source review completed
- Four edited C++ files retain UTF-8 BOM + CRLF
- Combined City + CityTests Linux build passed
- DevelopmentSnapshot 7/7, Construction 14/14 and SharedTransport 9/9 passed
- The new four-arm regression proves exact binary stability, deliberately absent signs/signals, node classification, custom cutoffs, derived paths, legacy fallback and malformed-cutoff rejection
- Final attachment-order regression and all 30 focused cases passed after the incremental rebuild
- Two original-v18-baseline reload/save cycles exited 0 and produced zero changed files: roads, development, metadata, economy, terrain and land files are byte-exact
- The same two large-coordinate overlap diagnostics remain in this already-float-encoded v18 world; cutoff/sign/order preservation fixes file drift, not previously lost precision
- Evidence: runtime_summary.json and per-filter results.json

## Separate precision hypothesis

The two observed roadOverlap diagnostics are not necessarily caused by cutoff drift: ParcelRoadIndex samples full Beziers and does not consume cutoffs. RoadBinary v18 narrows live double coordinates to floats, which can move roads by millimeters at large world coordinates. A separate v19 double-coordinate patch is prepared but not applied; fresh generated-world evidence is required before claiming that hypothesis fixes the overlaps. Restoring an existing v18 save cannot recover precision already lost.

This patch does not solve the pre-existing absence of serialized lane-connection identity used by signal phases. It also does not change guide-sign regeneration.
