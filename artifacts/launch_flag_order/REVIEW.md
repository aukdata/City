# Startup option order regression

Date: 2026-10-02

## Cause and bounded change

`GameApp::run()` stopped parsing arguments at either `--new` or `--load <saveName>`.
Rendering and diagnostic flags after the startup target were silently ignored.

The existing loop is now shared by the game and permanent tests through
`src/GameLaunchOptions.hpp`. Before the fix, extraction was mechanically checked
to be token-equivalent to the previous loop after only variable/reference renames.
That unchanged implementation produced the red results below.

The behavioral fix changes only the two startup-target branches:

- The first `--new` or complete `--load <saveName>` keeps ownership of the target,
  preserving the old first-target-wins behavior if conflicting targets are supplied
- Parsing continues after either selector, so later rendering and diagnostic flags apply
- `--load` consumes its save-name operand even when an earlier target was selected
- Preference loading, argument validation, post-parse capture setup, and scene startup
  retain their existing behavior

No save schema, simulation, renderer, or settings-persistence logic changed here.

## Permanent regression coverage

`Test/LaunchOptionsTests.hpp` is registered through `Test/CityHudTests.cpp` and calls
the same parsing loop used by `GameApp`; it does not duplicate parser logic.

- `LaunchOptions.LoadFlagOrder`: low-spec, distance, playtest, seed, and uncapped
  flags before versus after a load target
- `LaunchOptions.NewFlagOrder`: low-spec, distance, seed, and a valued playtest
  command-file option before versus after a new-game target; stale save cleared
- `LaunchOptions.FirstTargetWins`: conflicting/repeated targets, later ordinary
  flags, and consuming an ignored load operand rather than interpreting it as a flag
- `LaunchOptions.PreservesPreferences`: title/load defaults, persisted quality,
  distance and volume, and invalid/missing distance values without losing later flags

## Verification

The native-build owner performed both test executions and the combined application
and test build. This source-editing worker did not start an application or test process.

- [Red results](../shadow_cli/red/LaunchOptions/results.json): 4 tests failed with
  11 expected assertions, all attributable to ignored trailing flags
- [Green results](../shadow_cli/green/LaunchOptions/results.json): 4/4 passed
- `cmake --build build-linux --target City CityTests`: combined green build exited 0;
  build evidence is `build-tools/latest/build-shadow-cli-green.log` in the workspace
- UTF-8 BOM, CRLF, and trailing-whitespace validation passed for all four affected
  C++ files; `git -c core.whitespace=cr-at-eol diff --check` passed
- Native end-to-end trailing-flag confirmation passed using
  `--load rebuild_v20 --low-spec --render-distance 100`: the
  [captured state](../shadow_cli/cli-native-state.json) reports light rendering,
  an 853×512 3D target, and `renderDistanceMeters=100`; the
  [native debug log](../shadow_cli/cli-native-debug.log) confirms the intended save
  loaded with `msaa=false` and `shadows=false`. The native-build owner reported
  clean exit 0. This exercises the formerly ignored flags after `--load`.

Updated startup instructions in `BUILD_LINUX.md`, `.codex/build.md`,
`plan/28_commands.md`, and `plan/00_current_implementation.md`.
