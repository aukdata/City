# Drainage/outlet correctness checkpoint

Base21151ebec3525307690602cf49785bb44542700d. World.cpp is byte-identical to that published baseline; the rejected experimental macrovalley is excluded. RiverNetwork changes allow real-elevation finite-map outflow, preserve exact non-grid/control endpoints, cap tributary approach grade against natural terrain, share exact duplicate terminal identity before water/flow grading, and accept already-arrived boundary fallback.

## Default regression scope

The checkpoint retains the four existing river cases and adds six passing topology cases:10/10 in terminal_candidate_rebuilt_run, including the existing22-seed course audit. World source, settings, input files and actual rebuilt binary are recorded. The earlier terminal_candidate_stale_binary_NOT_VALID attempt is not validation; source_build_input_audit.json confines the stale-timestamp problem to that rejected run.

The default regional tests remain unchanged. New prospective landform and inherited-carving quality diagnostics are intentionally opt-in because pure21151eb already fails them. Complete executable source, pinned inputs, installer and exact commands are in diagnostic_fixtures. No assertion was weakened and no known failure is relabeled passed.

## Matched quality evidence and limits

retention_comparison uses identical four-seed fixtures/settings with baseline World and only the River implementation swapped. ActualTU compile/link, fresh distinct binaries, execution hashes, fresh metric files and exact restore are verified. Regional results improve2/4→3/4; largest connected plain and buildable retention improve on all four seeds. Seed130 largest-plain retention improves44.87%→56.95% but remains below65%.

Per-network sampled maximum cuts fall from500.5→110.0m(seed7),901.9→34.3m(42),1192.0→91.3m(130),332.0→23.5m(2026). Sample locations differ with each river network; these are not matched-point or global safety guarantees. Sampled added grades still reach2.95 and1.13 in seeds7/42, and inherited overlap-carving continuity remains red. Final bank shape, wet mesh/query agreement, shore contact and first-person appearance remain unresolved.

regional_map_sanity contains the earlier drainage-only seed42 CPU map without the rejected northern macro corridor. Its metadata identifies the capture scope; it is not GPU or final first-person evidence. Exact-cell Test prototypes, any new water adapter, rejected World shaping and historical lost cap/renderer work are excluded from this checkpoint. No complete-suite or Windows-runtime pass is claimed.
