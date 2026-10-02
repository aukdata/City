# Modal time controls and foreground-panel input

2026-10-02, baseline HEAD `4b91de5`. Bounded native checks cover pause/speed restoration, Settings dismissal, map/signal Escape priority, and panel pointer isolation. No performance claim or full-suite claim is made.

## Verified native time and modal behavior

The disposable native run recorded the following states:

- Running at ×2 (`running_x2.json`, `timeSpeed=2`) → pause (`pause_from_x2.json`, `timeSpeed=0`, `pauseMenu=true`).
- Settings remained modal (`settings_blocked.json`): camera focus, game hour 12.875, edit mode 0, and zero road draft points were unchanged. Cancel and unchanged Apply both returned to pause without resuming (`settings_cancel.json`, `settings_apply.json`).
- Resume restored ×2 (`resumed_x2.json`). Space then paused the clock (`space_paused.json`). Opening Settings while already paused, cancelling with Escape, and leaving pause preserved `timeSpeed=0` (`settings_escape.json`, `resumed_still_paused.json`). A subsequent Space restored ×2 (`space_restores_x2.json`).
- With signal 835 selected, Escape closed the full map without clearing the signal or opening pause (`map_escape_keeps_signal.json`). The next Escape cleared the signal selection only (`signal_escape.json`); the following Escape opened pause (`after_signal_pause.json`). Camera focus and game hour stayed unchanged through that chain.
- The native reviewer reported normal exit 0. `save-preservation.json` records 8,199 save files with no changed files. Unchanged Apply created settings only in the isolated test App; the main `App/settings.json` remained absent.

The contract is already specified in `plan/06_ui_spec.md`: preserve the pre-menu time speed, retain an already-paused state, consume the map's closing frame, and prioritize foreground panels over background pointer actions.

## Reproduced defect

At signal 835 (X 25751.047831, Z 23064.777660), the signal panel visibly covered the upper-right minimap. Clicking blank panel background at native content coordinate approximately (1196,112) nevertheless opened the hidden full map (`signal_before_map.json` → `signal_overlap_click.json`: `map=false` → `true`, selection 835 retained).

Wheeling over the same overlap changed the hidden minimap span from 2,000 m to 1,000 m (`before_overlap_wheel.json` → `after_overlap_wheel.json`). The world remained paused and the signal selection stayed unchanged.

Cause: `GameScene::update()` dispatched minimap pointer input before the panel handler without supplying visible-panel occlusion. `MinimapRenderer` therefore accepted clicks and wheel input by small-map bounds even when a panel was drawn above it.

## Shared production regression and bounded fix

The native minimap update now delegates its existing input stage to `MinimapRenderer::updateInput`. `GameScene` supplies the current visible-panel hit test before a closing click can hide its owner. `PanelManager` accepts explicit pointer values with native defaults, preserving its existing handler order and behavior while allowing the regression to exercise the actual close handler.

The three permanent `MapTransport.PanelOcclusion*` cases use those production paths and cover blank overlap clicks, overlap wheel input, repeated panel open/hide, unrelated visible panels, outside clicks, the actual closing click, held-button non-replay, exposed-minimap recovery, keyboard M, and full-map Home/Escape ownership.

The initial producer seam intentionally retained the original behavior. The measured red run (`red/MapTransport.PanelOcclusion/results.json`) was **3 tests: 1 passed, 2 failed**, with ten assertion failures in the click/wheel/closing paths. The keyboard/full-map case passed. The reviewer reported a clean red build.

The corrective behavior change is a single `!pointerBlocked` condition on the small-map pointer branch. The full-map branch remains before that guard, keyboard M remains independent, and the existing pause/Settings/text-focus guards are unchanged.

All six touched C++ files preserve UTF-8 BOM and CRLF. `git -c core.whitespace=cr-at-eol diff --check` passed after the fix.

## Final verification

The corrected Linux **City + CityTests build passed** (`build-green.exit=0`, `build-green.log`) without warnings or errors. Focused green results total **33/33**: MapTransport 13, Map.CacheAndResume 1, UI.FullScreenMap 1, CityHud 7, UI.PauseMenuAndControlHelp 1, and Settings 10. The three new pointer-occlusion regressions are included in MapTransport.

The final native reviewer repeated the real pointer path with signal 835 selected:

- Click and wheel over the foreground signal panel kept `map=false` and `localMapSpan=2000` (`fixed_signal_before.json`, `fixed_overlap_click_wheel.json`).
- Keyboard M still opened the map (`fixed_keyboard_map.json`). Escape closed only the map, preserving signal 835 and avoiding pause (`fixed_map_escape.json`).
- The signal panel's × visibly closed it without opening the map (`fixed_panel_closed.json`). Signal selection remains stored after × by existing behavior; the panel closure was checked on-screen rather than inferred from that selection field.
- With the panel closed, a wheel step changed the exposed minimap span to 1000 m and a fresh click opened the full map (`fixed_exposed_wheel.json`, `fixed_exposed_click.json`).
- Time stayed paused, camera focus stayed fixed, and no road draft points appeared throughout those pointer checks.

Both native runs exited normally (`baseline-exit=0`, `fixed-exit=0`). The final preservation check still records 8,199 unchanged save files and confirms the main settings file remains absent. Only the disposable test App acquired the deliberately applied settings.

This verifies the targeted behavior and adjacent focused suites; it is not a full-suite or Windows-build pass. No remote publication was performed.
