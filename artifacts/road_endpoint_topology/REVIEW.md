# Exact road junctions and designed-route preservation

2026-10-02. Independently validated shared topology checkpoint. Intracity mosaic and parcel classification remain separate, uncommitted work.

## Changes

- Exact endpoint-to-interior crossings reuse the existing endpoint and split the host with profile, ordered route membership and tracked construction ownership preserved
- Coincident perpendicular/collinear endpoints merge without replacing edge IDs; sign, marking and named-destination node references follow the merge
- Ambiguous duplicate-neighbor/host-arm joins, conflicting authored metadata and node-capacity overflow are refused before mutation
- Grade-separated bridge/tunnel endpoints remain separate
- Destructive sharp-angle replacement does not erase designed or route-bearing geometry
- Approximate overlap consolidation excludes designed geometry; exact intersection joining remains available

## Evidence

Fresh diagnostic7 results:
- `RoadIntegrity`: **30/30**, exit0
- regional connectivity and legacy shortcuts: **7/7**, exit0
- ordered and unbridgeable route-replacement regressions: **2/2**, exit0

Preserved results are under `../realism_diagnostic7/`.

The inherited-hamlet integration test is still failing on its separate empty-face/land-development requirement. Its route-identity and physical-curve checks now pass at seeds7/42/130. Diagnostic6 previously measured zero invalid inherited curves through generation, smoothing, constraints and exact intersection joining, followed by32/22/30 curvature failures at the first approximate consolidation. Diagnostic7 measures zero at every stage, including both consolidation passes.

Independent read-only review checked metadata remapping, duplicate-arm refusal, host-splitting ownership, route continuity, degree limits and endpoint handling. No outstanding finding in this checkpoint.
