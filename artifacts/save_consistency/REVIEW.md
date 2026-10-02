# Authoritative development snapshot verification

2026-10-02 UTC. Implementation and focused snapshot verification completed; separate road reconstruction follow-up remains. No remote push.

## Implemented scope

- New saves use metadata v4 plus `global/development.bin` schema v1: deterministic fixed-width little-endian fields, all generated zone/building state, urbanization flags and double-precision land polygon coordinates
- Every Building scalar and reference is preserved, including non-default empty cells, signed zero, historical disconnected frontage IDs and missing pre-existing parcel relationships
- Structural parcel-key corruption is rejected. Missing live frontage after legitimate road deletion is preserved rather than deleting houses or preventing a save
- New-format loading restores this authoritative development state, bypassing regeneration, legacy angle migration, clearance replay and construction replay that would change already fitted terrain
- Zone-manager edited-cell bookkeeping and development timing/progress survive separately from authoritative world restoration
- Codec decoding validates complete chunk sets, bounds, duplicate coordinates/cells, finite values, versions, trailing/truncated data and allocation limits before mutating development state
- Staged publication re-reads development fields and terrain samples; required current metadata and clearance are checked before publication and before new-format loading
- Legacy versions retain their original regeneration route. This cannot recover buildings already lost by earlier loads, and old binaries do not acquire new format protection retroactively

## Current verification state

- Source inspection complete; owned `.cpp`/`.hpp` files verified UTF-8 BOM + CRLF
- Focused regression additions cover exact two-save/two-load codec stability, zoned empty cells, generated baseline with no edit overlay, duplicate building/zone/chunk records, truncation, unknown version, bounds, NaN and infinity, edited bookkeeping and development timing
- Road-deletion regression preserves disconnected building edge IDs through save/load; invalid parcel key cannot overwrite previous valid slot
- Shared Linux CityTests rebuild passed without warnings/errors
- DevelopmentSnapshot 6/6, Zoning 9/9 and SaveTransaction 5/5 passed on the rebuilt executable; per-filter results and logs retained here
- Initial RoadConstruction filter matched no cases and is excluded from coverage; corrected Construction filter passed 14/14 (including the eight Construction.* tests and six related names containing Construction)
- Final City + CityTests rebuild passed without warnings/errors (build-final-integration.log); includes current Storage metadata changes
- Fresh seed42 save/load/save/load/save completed, all three processes exited 0; 37,274 buildings at every stage, zero missing parcel/edge/frontage references
- Every byte of the 13,825,666-byte development snapshot matched over both reloads: 4,096 chunks, 2,305,927 nondefault zone cells and 56,075 land patches
- All terrain, legacy land files, metadata (including time, camera, railway and development state), districts, clearance and economy files were byte-identical
- Only roads.bin changed on first load: 8,916 edges had cutoff changes and 1,099 had automatic-sign changes. Both reloads are road-byte-stable relative to each other, but roadOverlap diagnostics change 0 to 2. This separate reconstruction defect is not fixed by the authoritative development snapshot
- Peak RSS: new 5,747,288 KiB; first reload 5,458,024 KiB; second reload 5,455,256 KiB. These are cloud Linux/llvmpipe measurements, not user-PC requirements
- Runtime evidence: runtime_summary.json, new_snapshot.json, load1_snapshot.json, load2_snapshot.json and load1_road_comparison.json
- Windows build, second full-city seed and full end-to-end edited-city runtime are not claimed; focused edited-world/road-deletion regressions passed
- Full GameScene failure atomicity across roads, terrain and subsystem state is not established by codec atomicity tests

The runner under the workspace build tools uses a fresh `railway_review` slot and refuses to overwrite a pre-existing slot. Copies of every save are retained for exact comparison. Reload phases no longer reset the camera before verification.
