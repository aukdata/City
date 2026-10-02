# Adjustable render distance

User-requested setting, on top of the opt-in low-spec rendering profile.

## Controls and scope

- `/render distance`: query current value, units, valid range and reset syntax
- `/render distance 1500`: add a camera-eye distance limit in meters
- `/render distance 0`: preserve the original frustum/LOD behavior (default)
- Finite range:100–20000m, including decimal values
- Startup: `--render-distance 1500` before `--new` or `--load`; independent of `--low-spec`
- Current-session setting, not stored in city saves

Only building/tree/detail submissions and their shadows are reduced. World
loading, terrain/ground/farm surfaces, roads, vehicles, trains, simulation,
construction geometry and caches keep their prior behavior. This is not a RAM
cap. Bounds are conservative: a large tree/building stays when its near edge
intersects the range. Existing combined batches are retained whole when their
bounds intersect, so some features can remain beyond the numeric cutoff.
Distance is3D from the eye, so increasing camera height can hide scenery below.

Picking and outlines use the same additional cutoff as rendering; construction
and demolition continue using geometric hit boxes. Separate shadow visibility
revision handles distance/eye changes without invalidating terrain or planting.

## Validation

- Independent code review: no confirmed P1/P2 blocker in rendering/picking/cache paths
- Initial integrated City+CityTests build passed
- First execution found an input regression: Siv3D `ParseOpt<double>` accepted
  `500m` as500. Replaced with full-token `std::from_chars` parsing, including
  finite/range checks; runtime distance commands now share the CLI parser
- Invalid suffixes, incomplete exponents, extra tokens, overflow/underflow and
  malformed signs have regression cases; normal decimal/exponent/leading-plus
  values are accepted
- The pre-fix results are preserved in `tests_before_strict_parse/`; they are not
  a full pass. Three of four distance tests passed, including real batched color
  and shadow-depth readbacks, and related tree/command regressions passed
- Post-fix City+CityTests rebuild passed; all11 selected cases passed: RenderDistance4, tree/cache regressions2, command execution1, CityLighting3, RenderQuality1. Results under `tests/`
- Actual0→500→0 comparison passed in normal and low-spec profiles; both input saves stayed byte-identical and submitted counts restored exactly
- Native keyboard query/change500/reset0 worked, with Japanese feedback and matching state; building-pick alignment remains inconclusive as described below

## Repeatable runtime comparison

`chore/benchmark_render_distance.py --profile normal --save <pausedSave>` and then
`--profile low-spec` use the same save, fixed150m camera and paused noon. Each
sequence measures0→500→0 with stable geometry, complete CSV frame coverage and
save-file SHA checks. `--cutoff` changes500 when a different finite boundary is
needed. Startup `--render-distance` is also checked before runtime commands reset
it. Counters include building draw calls, submitted model/tree counts and the
active distance. Reset counts must be reviewed against the first0 phase.

Optional `--ui-marker <path>` leaves the low-spec app open only after all samples
and screenshots, for native targeting/query/change/reset QA. Creating the marker
lets the process exit normally and flush logs before aggregation. UI interaction
is outside sampled frame intervals. Raw screenshots/logs remain local; compact
metrics and permanent-test results form the review evidence.

## Limits

Only Mesa llvmpipe cloud execution is available here. Windows build/GPU speed and
minimum hardware requirements are unverified. Unrestricted3000m warmup was
observed separately during low-spec work; no stable far-view speedup is claimed.

### Concrete GPU evidence

The non-OBJ warehouse fixture removes238 color pixels with a finite cutoff and
restores the original image with0 differing pixels. Static shadow occupancy is
13745→0→13745 pixels for0→300→0, again with0 depth differences on restore.
Equal-focus/equal-extent eye movement produces0→13745→13745→0 shadow pixels:
the repeated unchanged view uses the cached shadow; six changed views trigger
exactly six caster callbacks. Terrain/planting generation counters do not change.

## Bounded runtime result

Same paused saved city, fixed150m camera at noon, source-frozen final binary.
Each range/profile interval has30 sampled frames; all90 per profile are preserved
in `normal_samples.csv` and `low-spec_samples.csv`. Input-save SHA manifests match
between profiles and are unchanged afterward. `comparison.json` retains the
binary hash, matched state and compact metrics.

| Range [m] | Submitted models | Submitted trees | Model draw calls | Normal render median [ms] | Low-spec render median [ms] |
|---|---:|---:|---:|---:|---:|
|0 (original behavior)|1443|14042|97|1147.31|505.87|
|500|94|145|94|1114.02|466.65|
|0 restored|1443|14042|97|1153.90|501.54|

The submitted sets are controlled and fully restored. These numbers are rendering
submissions, not simulated city population or deleted objects. The distance cap
only modestly reduced frame time in this view despite reducing submissions; no
claim of CPU-rendered playability is made. Per the user's latest scope limit,
performance work stops here: no more far warmups, memory work or quality algorithms.

### Native UI check and limitations

Keyboard `/render distance` displayed its Japanese query/help, adding500 changed
the value and feedback, and Escape/reopen followed by reset0 restored the default.
No save was made. A click aimed at a visible house roof selected nearby signal
N12873 (kind4), so building-picking alignment is **not certified**; this does not
establish a low-spec regression. Ctrl+A and short repeated Backspace inputs did
not edit the existing custom command text; Escape/reopen worked. These remain
input/usability observations for the main work, not new optimization scope.

The app exited normally after the bounded UI check. No performance task or
monitor remains running. Raw logs/PNGs stay local under the two profile folders.
