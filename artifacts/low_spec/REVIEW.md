# Low-spec rendering review

## Scope

User-requested low-spec fallback, followed by explicit adjustable drawing distance.
The first implementation is opt-in `--low-spec`. Normal rendering remains the
default. Distance controls are being prepared separately; they are not part of
the first compiled profile.

- 3D raster target: 853×512 at the usual 1280×768 scene (two-thirds each axis)
- Single-sample target instead of MSAA; no cast-shadow render passes or PCF samples
- Native-resolution HUD, map, selection mask/outline, camera and picking coordinates
- Same world content, simulation, terrain/road geometry and daytime lighting/fog
- Low-spec mode has softer 3D edges and no cast shadows; no automatic hardware detection
- No save schema/data change, no push

## Evidence motivating the change

The historical `../linux_port/timing_summary.json` llvmpipe run measured full-city
median render 11220.46 ms, of which shadow work was 7074.17 ms. Close 150m median
render was 1246.24 ms and forward GPU time 1202.17 ms. These historical runs are
motivation, **not** a matched A/B speedup measurement. `Scene::DeltaTime` clamps at
100ms, so `renderMs`/GPU timings must be used instead.

The code drew full shadow maps at 2048² and used full-resolution MSAA for all
hardware. The low-spec profile bypasses only those explicit costs. Large world
and geometry-cache memory remain; the previous roughly6GB RSS is not solved or
capped by this profile.

## Validation status

- Final integrated City and CityTests single-job build: passed, no warnings/errors
  (`build-tools/latest/build-final-integration.log`, outside repo, 2026-10-02)
- C++ source BOM+CRLF and whitespace checks: passed
- `python3 -m py_compile chore/benchmark_low_spec.py`: passed
- Independent read-only review: no remaining rendering blocker; inspected SDK base
  render-target clearing, viewport behavior and unchanged-size presentation path
- GPU tests: CityLighting3/3 and RenderQuality1/1 passed on final integrated binary; results under `tests/`
- Matched paused150m runtime A/B: passed; see `comparison.json` and the two `_sample.csv` files

New permanent tests use the production `RenderQuality::targetSize/present` helpers
and real shader GPU readback to check native one-pixel overlay sharpness, zero
static/dynamic shadow callbacks, and unchanged non-occluded daylight/fog pixels.
Existing shadow/material and moving-shadow tests cover the normal profile.

## Repeatable comparison

Run sequentially, from a source-frozen, compiled tree after other City processes
have exited. Use an existing **paused** save (`timeScale=0`). The runner refuses
an active City or an existing output directory. For example:

```
python3 chore/benchmark_low_spec.py --profile normal --save railway_review
python3 chore/benchmark_low_spec.py --profile low-spec --save railway_review
```

The runner uses the same save, native window, camera focus, yaw=-0.7, pitch=0.72,
noon and paused simulation. It measures both distance150m and3000m, excludes fixed
warmup and then requires30 rendered frames without terrain/tree uploads. It
rejects additional uploads during the sample. This is a completion-log heuristic,
so matched building counts and scene state must also be reviewed. It preserves
save SHA256 manifests, per-frame counters, RSS, logs and local screenshots. Logs
are copied only after City exits and flushes; all sample frames must exist exactly
once. Original saved files are checked unchanged after each run.

## Remaining coverage / limits

- llvmpipe software rendering is not the user's Windows GPU; no minimum-hardware
  performance guarantee or Windows build verification
- The first tests do not prove real-game clicking/projection alignment or dusk/night
  appearance; runtime inspection must supplement them
- Render-target savings do not bound terrain, building and road caches or simulation RSS

## Matched result (2026-10-02)

Same `railway_review` save, final integrated binary, camera focus/eye, noon, paused
clock, population233146 and funds30. Each profile has30 stable sampled frames,
97 building draw calls on every frame, and zero road-cache edge/node builds.
All input-save file hashes match across runs and remain unchanged afterward.

| Metric | Normal | Low-spec |
|---|---:|---:|
| Median render ms |1134.919|501.528|
| P95 render ms |1489.804|532.526|
| Median forward GPU ms |1120.219|483.897|
| Shadow render work in stable sample |0 (cached)|0 (disabled)|
| Near-view resident KB |5467544|5359100|

Median frame-render time fell55.8% (2.26× ratio) on llvmpipe. This is still roughly
2 rendered frames/second in software, not a claim of smooth playback on this
cloud machine or a Windows GPU. Near RSS dropped only about106MiB: geometry/world
memory remains about5.1GiB. Normal RSS is the last sample at the near phase end
(excludes far-view work); the low-spec value is its near-only run peak.

Both screenshots were inspected: identical road/building layout, no apparent
global offset, readable native HUD and minimap. The low profile visibly loses
roof detail and has jaggier curb/building edges, the intended resolution/MSAA
tradeoff. Live low-resolution mouse picking is deferred to distance-setting QA.

The normal3000m view reached21.44–22.99seconds/frame during asynchronous geometry
warmup (median shadow16.75seconds, forward GPU4.89seconds). It was deliberately
closed normally to keep the near A/B and proceed to draw-distance work; the
benchmark runner then reported its expected interrupted warmup. This is a cold
observation only, not a stable far-view benchmark or comparative speedup. Use
`--near-only` for the validated near scenario. Full raw logs, save manifests and
PNGs remain local under `normal/` and `low-spec/`; compact evidence is committed.
