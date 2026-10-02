# Navigation help / Road Planning overlap

## Reproduction and cause

The live 1280×768 review reproduced a six-line help panel being covered by the right-hand Road Planning sidebar. Both were anchored to the same corner, and the sidebar renders after the HUD. This is a layout collision rather than missing font glyphs.

## Change

- Dock the help panel below the compact top-left HUD, with its open/close button immediately above it.
- Reserve space for the default 374px right-hand editor, including at 800×600. At that width the help text scales from 12px to approximately 11.57px; larger layouts retain 12px.
- Keep the lower-left address/road tooltip clear. The new help does not move with Road Planning entry/exit.
- Hide the help button while a City/Traffic/Notices detail tab occupies the left column. Closing the tab restores the button. Hidden controls do not receive clicks.
- Continue using the shared rectangle for drawing and mouse blocking. Driving still closes help.

## Test-first verification

Before connecting the new button layout to CityHud, `CityHud.NavigationHelpEditorPreview` was built and run. The six combinations of 800×600, 1280×768 and 1920×1080 with overview/walking passed. Actual PanelManager and RoadPlanToolbar components were rendered after NavigationHelp, matching production order.

All six previews were visually reviewed by the visual-test worker: Japanese text readable and editor/help separated. GPU readback found zero covered help pixels and zero overflow pixels. Bright text-pixel counts were 653 at 800px and 750/764 at larger overview/walking sizes.

Evidence: `preview-results.json`, `preview-pixels.json`, `build-preview.log`, and local `Test/App/Screenshot/navigation_help_editor_*.png`.

## Integration verification

The integrated CityHud suite passed 5/5, including the new interaction regression and editor preview. The existing pause-menu/control-help regression also passed 1/1. Across the 27 HUD variants and six editor/help variants, GPU overflow/covered-help pixels remained zero. Hidden-button input, click consumption, repeated open/close, detail-tab return and driving transition passed. The visual-test worker reviewed the integrated 1280px and 800px screenshots and confirmed the button/help/editor separation.

The coordinated CityTests single-job build completed with no warnings or errors. Evidence: `integrated-results.json`, `help-results.json`, `integrated-pixels.json`, `integrated-hud-pixels.json`, `integrated.exit`, and local `Test/App/Screenshot/navigation_help_hud_*.png`.

Final City + CityTests single-job build completed successfully at 2026-10-02 00:26 UTC, with no compiler warnings or errors (`build-final.log`, `build-final.exit`). It includes the coordinated save-consistency and low-spec changes. The final binaries were retested at 00:27 UTC: CityHud passed 5/5 again and UI.PauseMenuAndControlHelp passed 1/1 again. The visual-test worker then verified the final live app at 00:31–00:32 UTC with a fresh paused seed-42 world: opening help then Road Planning kept all six help lines readable below the left HUD and separate from the right sidebar. The close button hid help while the editor remained open; reopening restored it. The recorded draft point count stayed zero, confirming no accidental road clicks. No full test-suite or Windows-build pass is claimed.

The changes are limited to `src/ui/NavigationHelp.hpp`, `src/ui/CityHud.cpp`, and `Test/CityHudTests.cpp`. These touched files had UTF-8 BOM with LF in the checkout; they are now BOM + CRLF as required by AGENTS.md.

## Scope limits

This avoids the default right-docked 374px Road Planning panel. Users can still deliberately drag movable editor panels over other controls; this change is not a general automatic panel-collision solver. Wider custom/debug editors are not claimed to be covered. No app was started or restarted by this worker, and no changes were pushed.
