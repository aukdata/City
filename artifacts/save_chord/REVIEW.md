# Save-chord event-time recognition

2026-10-02 UTC. Scoped input correction following an intermittent native Ctrl+Shift+S miss. The exact event sequence of that observed miss was not captured, so this report does not claim to establish its individual cause.

## Confirmed defect

Before the fix, `GameInput::saveShortcutActive` combined three frame-local `pressed` values. These values included fresh buffered downs, but the buffer retained no modifier held state across frames. Two reproducible consequences followed:

1. Frame A consumes Control-down and Shift-down. Frame B receives S-down, S-up and both modifier releases. S remains buffered, but the earlier modifier downs are gone and native modifiers are now released, so the valid chord is rejected.
2. Non-overlapping Control, Shift and S taps in one event batch can be falsely combined into a chord. S pressed before the modifiers can also be interpreted retroactively.

A complete overlapping chord contained in a single update was already handled correctly. This distinguishes the confirmed split-frame defect from a general inability to buffer short chords.

## Bounded change

- `src/ui/KeyboardActions.hpp`: retain save-specific generic, left and right Control/Shift states while consuming fresh ordered events; latch one save action when S-down occurs with both modifiers held.
- Reset the save latch each frame and clear save-specific held state on focus loss. Existing event-index deduplication still prevents history replay.
- Preserve ordinary `GameInput::down`, `GameInput::pressed`, edit-key press counts, text/IME ownership and pause-menu exclusions.
- Keep native held-chord recognition for existing held-input consumption and native-only input. It cannot override the modifier decision of a fresh buffered S event.
- No save format, disk-writing, map-input, engine keyboard backend or global input routing changes.

## Regression and verification

`Test/SaveChordTests.hpp` registers eight permanent `Input.SaveChord.*` tests through the two added include/registration lines in `Test/PlayabilityTests.cpp`. They call the actual `KeyboardActionBuffer` and the same save predicate plus S edge used by `GameScene::handleGlobalShortcuts`; save logic is not duplicated in a test implementation.

Coverage: split-frame releases, same-batch full taps, modifiers held across empty frames, retained-history no replay, non-overlapping taps, S-before-modifiers, focus reset, text/pause ownership, and left/right modifier combinations including one side released while the other remains held.

- Before production changes: **8 tests, 3 passed, 5 failed, 9 failed assertions**. See [baseline results](red/results.json). The unchanged baseline already passed same-batch tap, focus-reset and text/pause ownership cases.
- After production changes: **16 Input tests, all passed**, including all eight unchanged new cases and eight existing input/editing/focus cases. See [green results](green/results.json). This is an unchanged copy of the integrated `settings_screen/green/Input./results.json` result, with exit code copied alongside it.
- The integrated build/test owner reported both main and Test targets built successfully without warnings. This report's author did not independently launch a build or the application.
- Both edited C++ headers preserve UTF-8 BOM and CRLF; byte inspection found no trailing spaces/tabs. `git -c core.whitespace=cr-at-eol diff --check -- src/ui/KeyboardActions.hpp` passed.

## Limits

The input tests establish the source defect and correction for supplied ordered key events. They do not establish that this defect caused the previously observed native save miss, or independently verify the full native shortcut-to-disk save path. Native application verification is coordinated separately with the integration playtest.

## 最終本体の実操作

2026-10-02 04:30 UTC、設定画面を閉じた停止中の街で短いCtrl+Shift+Sを送信し、`[Save] Saved atomically to saves/mouse_road_edit_20261002`（517.479s）とmeta.json更新を確認した。`native-after.log`に保存。この成功は実操作から保存までの確認であり、以前の取りこぼし時のイベント順序を証明したものではない。
