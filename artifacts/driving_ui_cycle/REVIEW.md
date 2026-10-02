# Walking/driving interruption cycle and save-notice overlap

2026-10-02 UTC. A bounded native controls review on a disposable seed-42, v20 city derived from the previously verified grown-house snapshot. The original snapshot was preserved. Settings were opened and cancelled, never applied. No performance assessment or engine-lifetime change is included.

## Native controls checked

The integration owner performed the keyboard/mouse cycle in the actual application; the stage JSON below records the resulting game state.

- F entered walking (`cameraMode=2`) with eye height exactly 1.5m above the resolved surface. A W step moved approximately 0.15m while preserving that clearance: [walking](walking.json), [walked](walked.json).
- C entered driving on road edge 26508 (`cameraMode=4`) and changed the initially paused time speed from 0 to 1. The driving eye stayed 1.2m above the car surface: [entered](entered.json).
- W accelerated the car; the immediate sample was 0.2870875m/s, with eventual total travel 0.217026494m before the interruption: [throttled](throttled.json), [map open](map_open.json).
- M opened the full-screen map and stopped the car. W while the map was open left position, distance, and zero speed unchanged. Esc closed the map and returned to the same car, without opening pause: [map input blocked](map_throttle_blocked.json), [map closed](map_closed.json).
- Esc opened pause. Opening Settings, pressing W, and cancelling Settings retained the exact stopped-car position and distance. Cancellation returned to pause: [paused](paused.json), [settings open](settings_open.json), [settings input blocked](settings_throttle_blocked.json), [settings cancelled](settings_cancelled.json).
- After closing pause, C exited to walking and restored the pre-driving paused time speed. F recovered overview without moving the horizontal focus: [exited walking](exited_walking.json), [overview recovered](recovered_overview.json).
- The process exited cleanly: [exit code](native-controls.exit), [debug log](native-controls-debug.log).

This cycle does not claim live coverage of reverse recovery, all steering combinations, map-jump relocation, or every road surface. Existing focused tests cover related supported behavior separately.

## Confirmed display defect

A successful save notice was drawn after the driving HUD at the ordinary lower-left location. At 1280px wide, its opaque background covered the first driving instruction row containing C to exit and Esc for the menu. At 800px wide, the same placement covered the pause/reverse-recovery status panel instead.

Ordinary observation initially missed the five-real-second success notice. The integration owner then used the existing `rail_save_review` plus `capture=1` diagnostic to invoke the real save function and capture the same rendered frame, after native C entry. No new capture hook or longer notice lifetime was introduced. The live baseline confirmed the overlapping instruction row: [local baseline image](live_notice_before.png), [baseline state](live_notice_before.json).

`CityHud.SaveStatusDrivingComposition` renders the actual `DrivingHud`, reads a baseline, then appends the actual `SaveStatusNotice` in production order. Its clock is fixed at 101 seconds after showing at 100, so the notice cannot expire before the measurement. It checks both geometry and actual overwritten HUD/text pixels, not a mock renderer.

- Coverage: 800×600, 1280×800, and 1920×1080; success and long error notices; P-paused and road-edge-blocked car states. P-paused is distinct from an open pause menu.
- Red: one test failed, with all 12 compositions affected. The 1280px success case overwrote 6,559 protected HUD pixels including 150 text pixels; the 800px blocked success case overwrote 11,194 protected HUD pixels including 292 text pixels.
- Evidence: [red results](red/CityHud.SaveStatusDriving/results.json), [red pixel report](red/composition.json).

## Minimal correction and Test-first acceptance

`SaveStatusNotice` now accepts an explicit driving-layout flag. While driving, it reserves the scaled height of the highest lower-screen driving status panel plus a 12px gap. The normal and map default layout is unchanged; an open pause menu retains its existing centered layout even when a driving session is active. Success/error durations remain 5/18 real seconds.

The helper and permanent tests were built and reviewed before integration into the main scene:

- `CityHud.SaveStatusLifetime`, `CityHud.SaveStatusPreview`, and `CityHud.SaveStatusDrivingComposition`: 3/3 passed.
- All 12 corrected driving compositions reported zero changed protected HUD pixels and zero changed HUD text pixels. Save notices remained readable; the minimum measured notice-text count was 154 pixels.
- Ordinary and pause-menu preview checks passed, including an explicit assertion that a driving session does not move the pause-menu notice.
- The integration owner inspected the 1280px blocked/error preview and confirmed separation between the save notice, recovery panel, and driving instructions.
- Evidence: [preview results](preview/CityHud.SaveStatus/results.json), [preview pixel report](preview/composition.json).

Only after that acceptance, `GameScene_Render.cpp`'s normal-world save-notice draw gained `m_driving.active()`. The full-screen-map call was left unchanged. The edited C++ files retain UTF-8 BOM + CRLF; the CR-aware diff check passed.

## Integration verification

The coordinated final serial City + CityTests build completed successfully, exit 0, with no compiler warnings or errors. The final focused run passed **12/12**:

- CityHud: 8/8, including ordinary/pause save previews, driving composition, and notice lifetime.
- Driving.CameraAndInputOwnership: 1/1.
- Camera.ModalFocusAndWalkingPace: 1/1.
- Camera.WalkingHeightAndWasd: 1/1.
- UI.PauseMenuAndControlHelp: 1/1.

The integration owner then launched the final binary on the disposable copy, entered with native C, paused with native P, and used the same existing same-frame save/capture command. The live result showed the success notice above the P-resume status with a visible gap; the status and C/Esc/WASD instructions remained readable. The saved-state recording confirmed driving mode, `timeSpeed=0`, zero speed, and exactly the same car state as before saving: [local final image](live_notice_after.png), [paused state](final_p_paused.json), [saved state](final_saved.json).

The final application exited 0. Preservation checks confirmed all 8,199 original files unchanged and the final isolated `development.bin` byte-identical to the grown-house baseline. No app or build remained running. See [preservation](preservation.json).

[Compact verification evidence](verification.json) records the native state subset, all red/preview composition measurements, focused test names/results, and preservation checks. Full stage dumps, images, and runtime logs are retained locally outside the scoped commit. No full-suite or Windows-build pass is claimed.
