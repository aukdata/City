# Settings screen

## Scope

- Shared title/pause editor: 描画品質 (標準 / 軽量), building/tree draw distance in meters, and effect volume.
- No prohibited performance-tier terminology is exposed in UI labels.
- Draft changes are applied explicitly. Cancel/Esc discard them; Restore Defaults remains a draft.
- Apply from pause returns to pause without resuming time. Background inspector interaction is excluded while a modal is active.
- Local `App/settings.json` preferences are independent of city saves. Strict loading falls back to defaults for corrupt/missing/invalid records. Atomic replacement preserves the old file on failure.
- Startup precedence: defaults → local preferences → explicit CLI overrides. CLI and `/render distance` remain session-only unless Apply is clicked.
- A quality change prepares new raster targets and a fresh lighting instance between frames before persisting and switching. Native camera/UI coordinates and world/map state are retained. Shadow target allocation failures now report initialization failure.

Touched C++ files retain UTF-8 BOM + CRLF. Two edited legacy LF files (`PauseMenu.hpp` and `GameScene_InspectorPanels.cpp`) were brought into that required format; their logical changes remain limited to this screen.

## Test-first UI review

Before connecting the editor to the title/pause scene, the exclusive native reviewer built and ran `Settings`:
- 8/8 tests passed (five new settings tests and three existing related settings tests)
- Settings preview layout accepted; labels fit
- New persistence tests cover defaults, round-trip/overwrite, corrupt JSON/types/ranges, nonfinite values, blocked staging, replacement failure, cleanup, and boundaries
- Draft test covers three repeated open/edit/apply/cancel cycles and defaults, invalid values, rendered controls and panel containment
- Two `u8path` deprecation warnings were then replaced with C++20 UTF-8 path construction before final verification

## Input review

Source review of Siv3D TextBoxAt confirmed deletion only calls TextInput::UpdateText, the raw-control path already reproduced failing for the command palette on this Linux backend. The distance editor therefore reuses BufferedTextEdit after native text processing, retaining cursor positions, IME ownership and native-control deduplication. Open/default/close/focus loss reset fallback state. A new permanent test exercises middle-cursor short Backspace, forward Delete, no replay, and reopening. No separate actual-settings red observation is claimed for this source-evidenced fix.

## Final integration verification

Native `City` + `CityTests` build passed without warnings. Final focused suite: **37/37 passing**:
- Settings: 9
- Input: 16 (includes the independently checkpointed save-chord regression)
- UI.PauseMenuAndControlHelp: 1
- RenderDistance: 4
- RenderQuality: 1
- CityLighting: 3
- WorldSelection: 3

## Native application checks

On the disposable acute-corner verification city:
- Cancel left active Light / 500 m / 60% unchanged and created no preferences file; reopen restored those current values
- Invalid distance displayed a Japanese validation message and left runtime values unchanged
- Held Backspace cleared the field; a short Backspace deleted a digit
- Apply Standard / 750 m / about 39% recreated the target at 1280×768 and saved those preferences; reopen displayed them correctly
- Restore Defaults staged 0 m / 60% without modifying the file; Apply then saved Standard / 0 m / 60%
- Applying Light / 500 m / 60% again recreated the target at 853×512
- Restart without CLI overrides restored Light / 500 m
- The title Settings entry displayed the same preferences; Esc closed it without changes
- Pause was retained after Apply/Cancel; underlying city data was not modified by settings

- CLI `--render-distance 750` overrides the saved 500 m only for that session; the preferences file remains 500 m
- The original preferences file was absent. The test-generated preferences were retained as a local verification artifact and `App/settings.json` was restored to absence

## Final modal layering correction

The optional F3 graph initially covered 26 pixels of the settings border/gutter, without hiding labels or controls. Modal rendering now runs after diagnostics through the shared `ModalLayer` ordering helper, while the early pause return still excludes background inspector interaction.

The first GPU assertion sampled panel interior x=320 instead of the actual one-pixel frame at x=319. Readback identified the frame as RGB (102,122,143); the regression now compares a 3×14-pixel border strip against an intentionally reversed rendering-order baseline, away from the F3 caption.

After this final change:
- Targeted build passed
- Settings **10/10** and Pause **1/1** passed, including the new GPU layering regression
- The earlier other 27 focused tests were unaffected by this renderer ordering change
- Actual F3 + Settings check passed: the graph is dimmed behind the modal, border/gutter remain intact, and labels/buttons are clear
- No settings were applied during the final overlay check; the originally absent preferences file remains absent

All five native application runs exited successfully (exit 0). The focused suite covers 38 unique passing tests, with the final 11 affected settings/pause cases rerun after the modal change. No remote publication is claimed by this review.

## Environment limits

Verification used the existing native Linux Siv3D build in the dot cloud desktop (Mesa software rendering / NoSound). Volume state and persistence were verified; audible output and a Windows/MSVC run were not verified here. No performance guarantee for a physical GPU is inferred from these checks.
