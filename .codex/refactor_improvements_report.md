# Refactor Improvements Report (based on `./.codex/refactor.md`)

## Scope (comprehensive pass)
- Targeted files: `src/` + `Test/` C++ files (`.cpp/.hpp`)
- Total files scanned: **115**
- Structural hotspots (>=1000 lines):
  - `src/render/RoadRenderer.cpp` (3326)
  - `src/road/RoadNetwork.cpp` (2542)
  - `src/scene/GameScene_Panels.cpp` (2421)
  - `src/scene/GameScene_Input.cpp` (1549)
  - `src/gen/DistrictRoads.cpp` (1237)
  - `src/scene/GameScene.cpp` (1136)
  - `src/road/GuideSign.cpp` (1108)
  - `src/gen/MapGenerator.cpp` (1039)
- Complexity signal counts (`src`): `if` 2368 / `for` 785 / `while` 30 / `switch` 51
- `[[maybe_unused]]` occurrences: 11
- `-1` sentinel style occurrences (`= -1` or `return -1` in `src`): 203
- Build verification: MSBuild Debug x64 succeeded

## Improvements aligned with checklist

### 1) Structure and responsibility split (highest priority)
- Split gigantic translation units into domain slices:
  - `src/render/RoadRenderer.cpp`
  - `src/road/RoadNetwork.cpp`
  - `src/scene/GameScene_Panels.cpp`
  - `src/scene/GameScene_Input.cpp`
- Current state couples multiple responsibilities in single files (rendering policy + cache policy + geometry generation, etc.).
- Recommended split pattern:
  - `RoadRenderer`: mesh-builders / cache-management / sign-render / lane-markings / debug-draw
  - `RoadNetwork`: topology-edit / geometry-edit / guide-sign / route / road-object
  - `GameScene`: loading / save-load / panel-building / edit-input / world-actions

### 2) Remove sentinel `-1` API style where optionality is semantic
- High density in traffic/network/scene paths:
  - `src/traffic/VehicleManager.cpp`
  - `src/road/RoadNetwork.cpp`
  - `src/gen/DistrictRoads.cpp`
  - `src/scene/GuideSignEditor.cpp`
- Refactor direction:
  - IDs that can be absent should use `Optional<int>` in interfaces.
  - Return value `-1` should be replaced by `Optional<T>` return to force caller handling.
- This directly maps to checklist item "`-1` return -> `Optional<T>`".

### 3) Make loading-state concurrency explicit and safe
- Loading status/title/progress updates are spread across async pipeline and main thread in:
  - `src/scene/GameScene.cpp`
- Refactor direction:
  - Introduce `LoadingState` struct with synchronized update path (message queue or lock-protected swap).
  - Keep worker-thread writes restricted to thread-safe primitives.

### 4) Strengthen binary I/O boundaries and parsing contracts
- `src/save/RoadBinary.cpp` has many unchecked `read(...)` chains.
- Refactor direction:
  - Add small helper wrappers (`readOrFail`, `readArrayOrFail`) and fail-fast on partial read.
  - Isolate version-dependent decode into dedicated functions per version range.

### 5) Reduce include coupling and rebuild cost
- `src/scene/GameScene.hpp` currently includes many heavyweight headers (renderers/world/sim/road/traffic).
- Refactor direction:
  - Prefer forward declarations in header.
  - Move heavy includes to `GameScene.cpp`.
- Similar review should be applied to other high-fan-in headers (`RoadRenderer.hpp`, `VehicleManager.hpp`).

### 6) Normalize duplicated UI/panel construction patterns
- Repetitive widget composition and bar rendering logic appears in:
  - `src/scene/GameScene_Panels.cpp`
  - `src/ui/PanelWidget.hpp`
- Refactor direction:
  - Extract composable panel sections / lane-part bar widgets.
  - Centralize constants and layout primitives.

### 7) Magic-number consolidation
- Numeric constants are still broad across rendering/input/panel files.
- Refactor direction:
  - Move repeated UI sizes, thresholds, and geometry tuning values into named `constexpr` blocks per subsystem.

### 8) `[[maybe_unused]]` cleanup cycle
- 11 remaining occurrences in active code paths.
- Refactor direction:
  - Remove attributes when parameters become used.
  - If permanently unused by design, move to overload split or narrower function signature.

### 9) Consistent early-return flattening in deep branches
- Deep branch-heavy blocks in `VehicleManager`, `RoadRenderer`, `GameScene_*` would benefit from guard-first style.
- Refactor direction:
  - Standardize guard clauses to lower indentation and localize failure paths.

### 10) Testability seams for non-UI core logic
- Most verification is runtime/manual; automated assertion-based unit tests are sparse.
- Refactor direction:
  - Extract pure functions from map/traffic/network logic for deterministic tests.
  - Keep rendering/UI wrappers thin.

## Suggested execution order
1. `GameScene` loading state + save/load boundary hardening (risk reduction)
2. `RoadBinary` read contract refactor (data safety)
3. `RoadNetwork` and `VehicleManager` sentinel/API cleanup
4. `RoadRenderer` and `GameScene_Panels` structural split
5. Include/coupling and style-normalization sweep
