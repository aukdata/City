# Opt-in river/landform quality diagnostics

These complete executable fixtures preserve failures already present in the published baseline. They are not default regression gates for the limited drainage/outlet checkpoint, are not relabeled passed, and have unchanged assertions. The default checkpoint verifies six new river-topology cases plus four existing river regressions.

Use a clean disposable worktree at the drainage checkpoint, leaving normal development files alone:

```sh
git worktree add ../City-drainage-diagnostics HEAD
cd ../City-drainage-diagnostics
python3 artifacts/drainage_only_rebuild/diagnostic_fixtures/install_diagnostics.py
```

After the normal Linux SDK, assets and native-display setup, exact build/test invocations are:

```sh
cmake --build build-linux --target CityTests --parallel 1
cd Test/App
./CityTests --filter Rivers.BaselineCarving.OverlappingBanksStayContinuous
./CityTests --filter Terrain.Foundation.PostRiverUsableRegion.
```

A fresh worktree needs the repository's usual CMake configuration first. Preserve TestResults after each command, since summaries are overwritten. On the existing Windows toolchain, the corresponding normal wrapper is `python chore/run_check.py build --target Test` and `python chore/run_check.py test --filter <exact filter>` from the repository root; Windows runtime was not validated here.

Known results: inherited carving continuity is red. Four-seed regional retention is2/4 on pure21151eb and3/4 on corrected drainage. Seed130 retains56.95% of the largest sampled connected plain, below the unchanged65% target, despite improving from44.87%. Full evidence is in ../retention_comparison and ../terminal_candidate_rebuilt_run. River-relative bank sample locations differ between networks; improvements in sampled maxima do not prove pointwise/global bank safety.

The same opt-in fixture also exposes Terrain.FoundationMap.Seed42 and Terrain.FoundationProbe.Seed42 for CPU-only map/capture work. These are not GPU/first-person acceptance. The rejected macrovalley implementation and all exact-kernel prototypes are absent from the default checkpoint.
