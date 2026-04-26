# Code Review Report (comprehensive pass)

## Scope
- Reviewed: `src/` + `Test/` (`.cpp/.hpp`, 115 files)
- Built: `City.sln` Debug x64 (success)
- Runtime test execution: `Intermediate/Test/Debug/Test(debug).exe` could not run in current WSL environment

## Findings (ordered by severity)

### High 1: Load failure path can incorrectly proceed to "new game finalize" logic
- Location:
  - `src/scene/GameScene.cpp:73-76` (`m_loadGameResult = loadGame();`)
  - `src/scene/GameScene.cpp:353-392` (if false, executes generic post-pipeline path)
  - `src/scene/GameScene.cpp:619-620` (`loadGame()` returns false on missing/invalid meta)
  - `src/scene/GameScene.hpp:56` (`m_loadGameResult` default false)
- Problem:
  - `m_loadGameResult == false` means both "new game path" and "load failed".
  - If load actually fails, code still runs `applyZonesGlobal()`, `placeInitialBuildings()`, and starts simulation as if initialization succeeded.
- Risk:
  - Corrupt/incomplete world state becoming playable instead of explicit error handling.

### High 2: Binary decode trusts unbounded counts from file, allowing pathological allocation/work
- Location:
  - `src/save/RoadBinary.cpp:210-212` (`nodeCount/edgeCount` read)
  - `src/save/RoadBinary.cpp:297-299` (`laneCnt` -> `e.lanes.resize(laneCnt)`)
  - `src/save/RoadBinary.cpp:327-331` (`partCnt` -> reserve/loop)
- Problem:
  - Count fields from disk are used directly without sanity caps.
- Risk:
  - Corrupted or hostile save can trigger excessive memory allocation and long parsing loops.

### Medium 1: Data race on loading UI strings across threads
- Location:
  - Writer side: `src/scene/GameScene.cpp:127`, `171`, `204`, `216`, `534`, `665`, `704` etc.
  - Reader side: `src/scene/GameScene.cpp:430`, `438`
- Problem:
  - `m_loadingTitle` / `m_loadingStatus` are `String` (non-atomic) updated in async pipeline while main thread renders them.
- Risk:
  - Undefined behavior under concurrent read/write.

### Medium 2: Partial-read handling is weak in terrain bulk load path
- Location:
  - `src/scene/GameScene.cpp:563` (`r.read(gridSize)` without result check)
  - `src/scene/GameScene.cpp:570` (bulk `read` return value unused)
- Problem:
  - Truncated terrain file may be treated as loaded; invalid buffer may be used for min/max and chunk install.
- Risk:
  - Silent data corruption or unstable terrain state instead of clean fallback.

### Medium 3: Async completion exception path is not guarded
- Location:
  - `src/scene/GameScene.cpp:351` (`m_generationFuture.get()`)
- Problem:
  - If async pipeline throws, no `try/catch` around `get()` in update loop.
- Risk:
  - Unhandled exception may abort scene/game without recovery messaging.

## Refactor-risk notes (non-blocking but important)
- Very large files increase regression risk during change:
  - `src/render/RoadRenderer.cpp` (3326)
  - `src/road/RoadNetwork.cpp` (2542)
  - `src/scene/GameScene_Panels.cpp` (2421)
  - `src/scene/GameScene_Input.cpp` (1549)
- Sentinel ID style (`-1`) remains widespread (`src` total 203 hits), making invalid-state handling easy to miss.
- `[[maybe_unused]]` is still present at 11 points; periodic cleanup will keep interfaces honest.

## Test/verification gaps
- No assertion-driven unit-test pattern (`TEST/REQUIRE/CHECK`) was detected in scanned C++ sources.
- Current confidence relies heavily on compile success and runtime/manual validation.
