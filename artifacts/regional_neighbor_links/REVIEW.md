# Regional neighbor road connections

## Status

Implementation installed on 2026-10-02. The sole shared build owner completed the combined stage4 compile cleanly and ran fresh focused checks against the unchanged frozen regional source. All eight focused checks passed (six new regional cases, the retained rural shortcut case and settings validation), and all eight passed again in stage5 after the shared topology integration. No desktop or main-game execution was performed by this task.

Static checks completed:
- The six owned implementation/test/settings files have no CR-aware whitespace errors
- C++ sources and headers use UTF-8 BOM and CRLF
- `network.json` keys exactly match required schema properties, and all configured values meet their declared types/ranges

## Change

The existing national backbone remains. The former rural-only, random nearest-neighbor shortcut pass is replaced with deterministic nearby candidates for regional cities, local towns and rural settlements.

- Four terrain-ranked neighbors per settlement within 10 km, selected from a bounded nearest pool
- Reserved nearby town peers and nearest cross-component repair candidates prevent dense surrounding villages from hiding a disconnected town
- Component repair is considered before optional detour reduction
- At most 192 evaluated settlement pairs and 48 terrain alignment attempts per pass; at most three access-pair alternatives for a candidate
- Optional additions require the existing journey to exceed 1.8 times straight-line separation, and then save both at least 100 m and 20 percent of the bounded previous road distance
- Optional links are limited to two per settlement in a pass; component repairs still require finite affordable geometry
- Construction cost must remain at most eight times settlement separation using the shared surface/earthwork/bridge/tunnel model
- A settlement borrows road access only within its own limited footprint; an isolated settlement can receive a genuine center anchor
- Routing enforces both directions, operational road lanes and actual intersection transitions. Generated planned roads can be evaluated when their roadbed is present
- Non-rural alternatives receive deterministic prefectural-route numbers; rural-only links retain local-road profiles
- Proposed networks are copied and committed only after validation. Original edge/node IDs and route identities survive rejected proposals
- Intersection validation covers both the added alignment and any existing crossed-road descendants, using each edge's own grade/curvature limits

The stage does not force a path through infeasible terrain, replace the national route tree, or claim all candidates succeeded. Failed and deferred pairs remain visible in diagnostic counters.

## Regional measurements

`VillageConnections::measure` labels the actual road graph by nearest settlement in road distance. It then contracts same-settlement regions and counts spatially distinct inter-settlement corridors. Boundary crossings closer than 250 m are grouped as a local street fan; separated parallel corridors are retained, including a real alternative between only two towns. Dense loops wholly inside one town do not inflate regional cycle counts.

Measurements include settlement access, compressed components/links/cycles, audited nearby pair count, bidirectionally reachable/unreachable pairs and mean/maximum detour. The improvement pass compares the exact same bounded candidate pairs before and after. “Unreachable” also includes exceeding the finite audit distance bound, as documented in the API; it is not asserted to prove global graph disconnection.

## Focused regression cases passed

- `GenerationRevision.VillageShortcuts`: retained legacy rural detour case, now also verifies the two-town alternate corridor cycle
- `GenerationRevision.RegionalTownLoop`: closes a missing town connection; ignores an internal town grid cycle; preserves a national route; tracks split prefectural edges with recycled storage slots; second pass adds nothing
- `GenerationRevision.RegionalDisconnectedTowns`: joins separate street components and fully isolated original anchors, both directions
- `GenerationRevision.RegionalDirectionAndTurns`: excludes a one-way reverse journey, absent turn and closed lane, then repairs the reverse journey
- `GenerationRevision.RegionalTerrainCostAndFailure`: accepts a cheap valley corridor, rejects the ridge option, retains grade/height constraints and leaves no phantom IDs on rejection
- `GenerationRevision.RegionalCrossingRollback`: rejects a new crossing that would create an illegal short child on an existing road, preserving the original national route
- `GenerationRevision.RegionalTownCandidateCoverage`: nearby disconnected towns remain candidates despite more than four closer villages around each
- `GenerationSettings.RequiredValuesAndValidation`: configuration loading and validation

Verified fresh runtime results (2026-10-02, stage4):

- [Regional tests: 6/6](../realism_stage4/GenerationRevision.Regional/results.json), with [successful process status](../realism_stage4/GenerationRevision.Regional/status.json)
- [Village shortcut: 1/1](../realism_stage4/GenerationRevision.VillageShortcuts/results.json)
- [Settings validation: 1/1](../realism_stage4/GenerationSettings.RequiredValuesAndValidation/results.json)
- [Town-loop measurements](../realism_stage4/GenerationRevision.Regional/regional_town_loop.json): regional links **3 → 4**, regional cycles **0 → 1**, maximum audited detour **7.0000× → 1.2649×**, **6/6** nearby pairs reachable in both directions, **one** accepted addition from **one** alignment trial
- [Terrain measurements](../realism_stage4/GenerationRevision.Regional/regional_terrain_connections.json): components **3 → 2**, **one** affordable valley connection, **two** rejected ridge corridors, **three** alignment trials. Two nearby pairs remain unreachable rather than being forced through unsuitable terrain
- The combined six-case regional process exited 0 with fresh result artifacts. Its test durations were approximately 90–289 ms per case; this is fixture runtime, not a claim about full-map generation performance

The source hashes tested were unchanged from the 13:19 UTC freeze. No focused failure required a code edit after the shared build. A later coordinated change to shared `RoadNetwork_Topology.cpp` adds endpoint-to-interior joins and preserves authored road metadata; the stage5 rerun below verifies this integration. The overall terrain/urban batch was still in progress when these focused results were recorded. These results establish the regional regression fixtures, not a new full-map playtest or a guarantee that every real-world candidate is feasible.

## Post-topology integration rerun

Fresh stage5 checks on 2026-10-02 14:17 UTC passed against source commit `1dc4300` plus the coordinated shared topology changes:

- [Regional: 6/6](../realism_stage5/GenerationRevision.Regional/results.json), [valid process result](../realism_stage5/GenerationRevision.Regional/status.json)
- [VillageShortcuts: 1/1](../realism_stage5/GenerationRevision.VillageShortcuts/results.json)
- [Required settings: 1/1](../realism_stage5/GenerationSettings.RequiredValuesAndValidation/results.json)
- [Town-loop metrics](../realism_stage5/GenerationRevision.Regional/regional_town_loop.json) and [terrain metrics](../realism_stage5/GenerationRevision.Regional/regional_terrain_connections.json) reproduce the stage4 values exactly

All three processes exited 0 with `validResults=true` and fresh artifacts. No regional source edits were needed after the original source freeze. This completes the regional implementation's focused integration validation; broader game/visual acceptance remains with the combined task.

## Scope not changed

`MapGenerator::generateGlobalRoads` still uses its existing backbone generation. Its nontransactional segment helper and fallback reachability bookkeeping were reported for coordinated follow-up; this change does not edit that file.

## Commit coordination

An initial scoped commit attempt was blocked by missing Git identity. The shared build owner supplied the established assistant identity (`dot <dot@localhost>`) for command-local Git use; no persistent or global Git configuration is changed. The scoped commit includes only the eight owned source/settings/test/documentation paths. The pre-existing staged save-consistency review is excluded, and publication remains with the parent.
