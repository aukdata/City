# Inherited hamlets inside expanding cities

2026-10-02. Implementation and validation are in progress. Do not interpret pending numeric checks as visual acceptance.

## Requested shape and reference

The requested new-world structure is older small settlement cores and connecting everyday lanes retained inside a later enlarged city. It also retains genuinely regular planned districts. Tokyo is a contrast/reference, not an exact reconstruction target.

Inspected public maps:
- [GSI Shimokitazawa](https://maps.gsi.go.jp/#16/35.664000/139.668000/&base=std&ls=std&disp=1&vs=c1g1j0h0k0l0u0t0z0r0s0m0f1): coherent small street patches, changing local orientations, bent collectors, T/offset joins and station/rail seams
- [GSI Kinshicho](https://maps.gsi.go.jp/#16/35.695000/139.812000/&base=std&ls=std&disp=1&vs=c1g1j0h0k0l0u0t0z0r0s0m0f1): legitimate regular grids, varying block dimensions, interrupted by rail, station, park and canal structure

The selected game dimensions are design parameters, not measurements from these maps.

## Baseline

`UrbanStructure.AbsorbedHamletTopology` failed as expected for seeds7/42/130. All had zero retained older-lane routes and zero meaningful non-grid road length. The seed42 map visibly showed one orthogonal orientation with some missing edges. National route continuity and building access/overlap validation passed. Results/maps are in `../realism_stage4/UrbanStructure.AbsorbedHamletTopology/`.

## Staged implementation

- Build the modern collector skeleton, then bounded deterministic old cores whose orientation points toward the prior center, then regular local infill
- Keep up to three old cores, with a primary lane, offset rear lanes, offset T branches and useful old-core connecting streets; don't create separate regional settlement nodes
- Reserve successful core areas and near-parallel inherited street envelopes from later local infill; terminate eligible infill with explicit shared-node seam junctions
- Keep meaningful HistoricGrid/PlannedGrid structures regular; apply the layered form to other eligible large-city structures
- Fit actual ground geometry and road-class grade/radius limits; reject water/reserved/steep candidates rather than force crossings or elevated streets
- Perform intersection resolution inside candidate transactions and check every allocated/split descendant before committing. Ordinary infill only snapshots near the older/seam geometry envelope. Failed candidates restore the entire prior graph, including route tables and ID allocators
- Count transaction copies for review; preserve runtime and peak-RSS evidence. Copies are short-lived, never retained per candidate

## Shared topology safeguards

Exact endpoint/interior crossings create real T nodes. Coincident perpendicular or collinear endpoints merge without replacing edge IDs. Ambiguous duplicate-neighbor/host arms and degree-capacity conflicts are refused before mutation. Road profiles, route order, tracked ownership and authored node references are preserved. Genuine grade-separated crossings remain separate.

Destructive sharp-angle cleanup and narrow-face merging may not erase an inherited route. Physical grade/radius validation still applies to the planner and split descendants; preservation is not permission for an invalid road.

## Validation scope

The next shared build runs the complete RoadIntegrity suite, regional connectivity regressions, route replacement, three-seed inherited-hamlet fixture and expanded21 city-type/seed matrix. The hamlet fixture runs production-like sharp-angle, smoothing, curve-constraint, crossing, consolidation and block cleanup before checking distinct old cores, explicit modern-local seam joins, route continuity and parcel access/overlap.

A diagnostic face report includes tiny faces below the normal collection cutoff and long thin faces above the narrow-block cleanup cutoff. It records boundary IDs, area, protected boundaries and sampled maximum road-envelope clearance. Flagged faces are rescanned at0.5m; absence of a free sample is a review flag, not mathematical proof of overlap. Small positive-clearance islands can remain green space.

Independent read-only review checked endpoint metadata, ambiguous joins, descendant validation, full-copy rollback, external-reference safety and seam-envelope coverage. No outstanding static-review blocker remains. Build/runtime/map acceptance is still pending.

## Diagnostic outcomes

- Stage5 made three old cores and49–58 old/modern local junctions in the regional-hub fixture. All occupied-building/frontage/road/other-building overlap counters remained zero, but the feature was rejected for post-cleanup curvature failures and unresolved empty faces
- Diagnostic6 localized the physical failures: each seed had zero invalid inherited curves until the first approximate consolidation, which introduced32/22/30 radius violations. Final route counts also exceeded the builder's maximum because cleanup fragmented routes
- Diagnostic7, with designed geometry excluded from approximate consolidation, retains original route identities and has zero curve violations at every stage for all three seeds. RoadIntegrity30/30, regional/legacy shortcut7/7 and route2/2 passed. Those source changes are checkpointed in62e0819, but were tested in the shared working tree rather than a separate committed-tree build
- The remaining diagnostic7 failures are6/7/7 empty faces at seeds7/42/130. A conservative geometry classifier and detailed candidate/storage/frontage diagnostics are still under validation. There is no blanket exemption for a failed building-placement attempt
- Stage5 generated logs were preserved alongside their JSON. There are47–144 transaction snapshots per eligible city, not one per every grid edge. The three-seed mosaic fixture ran in10.60s with sampled peakRSS1.00GB, compared with its earlier baseline8.33s/1.02GB. These are whole-test observations, not an isolated algorithm performance benchmark

The classifier uses an available-land superset and the same finite, tapered road-clearance quads as placement. Its inscribed footprint disk is slightly shrunk, so a zero result is conservative. Grade-separated or degenerate unsupported geometry remains unknown. Positive/unknown candidate area is not a proof of buildability or impossibility. Curved near-threshold, asymmetric/tapered, degenerate-handle and flat-end-cap cases passed in diagnostic12; these guarantees are independent of the deliberate public-ground design below.


## Later diagnosed fixes and current verification boundary

- Diagnostic13 captured actual plan geometry and finite placement witnesses. It found a valid existing parking site wrongly counted as empty, and a legal 9m house missed by the coarse frontage search. Parking now counts as developed ground separately from roofs/housing. A bounded .5m arc/.25m setback retry placed that house in diagnostic14 without relaxing footprint, terrain, road, neighbor or access predicates
- Continuous fitted pieces share frontage through tangent-continuous two-arm joins. Real-junction exclusions propagate across short pieces; sharp two-arm joins do not become continuous frontage
- Historical connectors now enter an established old-street gateway instead of approaching its midpoint along a duplicate parallel corridor. This removed the long seed42 strip and seed7 corner in diagnostic15
- That change exposed two indirect cleanup paths. A split could reuse a protected third-side edge and overwrite its profile/design flag, and an endpoint merge could redirect an ordinary arm onto an existing protected pair. Both now preflight the complete affected set before mutation. Protected duplicate cleanup also refuses silent route/profile transfer
- Diagnostic20: RoadIntegrity33/33, explicit existing-access seam fixture1/1, regional6, shortcut1 and both ordered/unbridgeable route tests passed. All three hamlet seeds retain physical constraints and route identities, with zero duplicate endpoint pairs. The diagnosed earlier pairs were nearly coincident same-level pieces only0.78–4.25m long, not useful distinct alternatives
- A modern node already attached to an old lane does not need another seam approach. Diagnostic20 removed the redundant seed130 long wedge, increased building counts, and retained the required modern/old connections. Current unresolved faces are three small corners: seed42 areas693.6/510.8m² and seed130471.9m² measured between road centerlines, with substantially smaller actual open ground
- The earlier seed130 exact-coordinate house assertion depended on the superseded road layout. Its successful diagnostic14 evidence is retained. A fixed offset-neighbor frontage fixture now shares the production6/1.5/.5m search schedule and .25m neighbor clearance, verifies genuine coarse/fine misses and refined success, generates real parcels, and applies the full final city validator. Its search window is explicitly one empty storage cell, not a claimed collected street face

## Deliberate small public ground (pending runtime acceptance)

The user-approved outcome allows a small open/landscaped corner where retained useful roads meet, without requiring housing in every face. This is a design choice, not an impossibility proof. The new bounded helper uses existing GardenSoil public-ground rendering (green material122, with existing optional planting); it adds no new assets or renderer behavior and does not promise a tree at every pocket.

Only inherited/modern corners are considered after the unchanged building retries. Actual face polygons are clipped around all nearby road ribbons, railway landscape exclusions, occupied footprints and already-generated private/public land patches. Unsupported geometry is refused. Small-ground design limits are12–250m², at most55m extent, a usable inset, one chunk, dry sampled boundaries/interiors and conservative terrain-grid corner relief. These numbers define the supported small-space treatment, not measured Japanese standards. Larger vacant faces remain reported.

Building angles are finalized before private parcels/public ground. Public patches are counted separately from housing and parking. Diagnostic plan views explicitly distinguish their green fill from a house-placement witness. Tests cover road clipping including laterally offset profiles, larger vacant blocks, thin strips, all-inherited corners, rail exclusions, existing yards and low terrain corners. This batch is not yet accepted until compiled tests and actual rendered evidence are inspected.

## Broader controlled comparison and transport parity

Later evidence supersedes the provisional corner counts above. Diagnostic24 passed all eight focused urban filters, including the three hamlet seeds and the fixed frontage/public-ground fixtures. A subsequent 21-case matrix exposed conservative empty-face diagnostics that the focused fixture did not cover; this is not a full-generation acceptance claim.

A controlled same-source, no-mosaic baseline was run with only the mosaic enable gate temporarily disabled. The exact original header bytes were restored and independently SHA-verified before further builds. Baseline results were 12/21 passing with 17 reported faces, versus 11/21 with 33 faces for that mosaic version. Fifteen centers were identical, two disappeared/reshaped and eighteen were new/reshaped. The extra original historic roof-density assertion remains seed42-specific; all seeds retain the structural/access/overlap assertions. See `nomosaic_comparison.json` and `density_zone_comparison.json`.

The initial necessary-center-area diagnostic omitted rail and station/depot obstacles present in the actual placement index. It now reads the same complete registered ribbon stream as placement, with rail-blocked and rail-adjacent legal-parcel controls. Zero available center area alone supports an unbuildable conclusion; positive area remains unresolved and does not establish a legal square. No public green is placed on rail land. Diagnostic31 passed five focused parity/face/pocket filters and reduced reported faces from33 to14 without altering rendering or building clearance.

## Bounded gateway alternatives

A new village-lane approach checks sustained close departure against the existing connected corridor, including continuous split pieces. The design rule derives spacing from actual road envelopes and the unchanged minimum house clearance. It is a frontage design preference, not a geometric impossibility proof. Independent close parallel roads, useful T junctions and forks opening legal frontage remain supported.

The bounded candidate set includes original backbone nodes and interior projected gateways. Failed attempts are transactionally discarded. Compatibility is ranked before distance, but fitted geometry still passes the full physical/terrain and join checks. Logs distinguish requested/accepted cores, site rejections, attempts and failure categories.

The first gateway version passed both focused departure tests, the previously regressed constrained-linear seed130 case and the three regional-hub hamlet seeds. Its full matrix passed13/21 with every physical/access/overlap counter zero. The later compatibility ranking restored three cores in both targeted large-city cases but left one unresolved face in constrained-linear130 and four in metropolitan130. Their exported image totals also include two and five separately accepted public grounds; image file counts are not vacancy counts. A post-intersection narrow-loop check is being reviewed to address interior crossings that rejoin and enclose a long shallow strip. It will not reject independent crossings or certify every residual face as housing-capable.

Density is reported using both in-core building counts and the sum of assigned occupied-site parcel areas. This latter quantity is explicitly a sum, not a union of all usable city land. Earlier building-count differences of roughly3–5.5% are documented rather than offset by relaxed setbacks or overlap.

## Current visual gate

The original generated ground preview reached resource guards before writing a valid ground image. Lowering submission distance did not prevent whole kilometer-sized chunk mesh/model preparation. A second, working-set-aware guarded attempt also stopped without an OOM event. Neither is visual evidence or a pass.

A Test-only cropped actual-renderer fixture is staged: retain the original short street, copied terrain values, real building geometry/materials, road network and generous boundary margin while withholding distant chunks/assets. Scoped restoration and original/restored field fingerprints are required. This will be labeled a cropped generation fixture, not a full-app performance or whole-city playtest. Production renderer culling and assets are unchanged. The earlier native screenshots still show unresolved repetitive house surfaces and broad weakly articulated courts; no facade realism improvement is claimed.


## Frozen urban source checkpoint

The final focused live-tree set passed64/64 cases, including ResidentialAccess6/6, RoadIntegrity33/33, collector alignment, center/cache persistence, building overlap, protected routes, regional alternatives and the three focused hamlet seeds. The separate21-case quality matrix remains14/21 with16 unresolved transport-constrained faces; this is not relabeled an aggregate pass. `remaining_face_classification.json` identifies the one exact no-mosaic baseline center and the fifteen new/reshaped faces. Sampled failure is not treated as an impossibility certificate.

An urban-only snapshot is being assembled against the existing committed terrain implementation. Experimental river/levee production, tests and registrations are excluded, as are deferred facade experiments. The shared renderer terrain-audit friend and terrain/outlet specification hunks remain outside that snapshot. Independent build and normal-suite results will be recorded before publication; earlier results above were obtained in the shared working tree.
