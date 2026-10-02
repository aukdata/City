# Linux development build

This local port targets Siv3D 0.6.16 and x86-64 OpenGL 4.1 or newer. The
upstream platform instructions are at <https://siv3d.github.io/ja-jp/download/ubuntu/>.

1. Check out OpenSiv3D tag v0.6.16. Apply `chore/siv3d-linux.patch` to that SDK
   checkout, build, and install to a chosen prefix. The patch supplies the Linux
   event stream used by City's buffered keyboard actions, plus three compatibility
   fixes for Debian 13 / GCC 14.
2. Install the SDK's documented Linux dependencies, CMake, Ninja and pkg-config.
   The runtime uses OpenGL timestamp queries for GPU timing.
3. Configure City using the installed SDK and original Linux resources:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/siv3d/install \
  -DSIV3D_RESOURCE_DIR=/path/to/OpenSiv3D/Linux/App/resources
cmake --build build-linux --parallel 3
cd App
./City --playtest --seed 42 --new
```

Always run from `App/`. Start without arguments for the title screen. New worlds
are the focus; older save compatibility is not part of this port.

## Shaders

The checked-in GLSL files are generated from the current HLSL entry points with
Khronos glslang and SPIRV-Cross. Install `glslang-tools` and `spirv-cross` if the
HLSL changes, then run:

```sh
python3 chore/generate_linux_shaders.py
```

The conversion accounts for Siv3D's built-in matrix upload layout and OpenGL
shadow-depth mapping. Windows continues to use the original HLSL shaders.

## Tests

```sh
cmake --build build-linux --target CityTests --parallel 3
cd Test/App
./CityTests --filter RoadPlan
```

The Linux test source list mirrors `Test/Test.vcxproj`. Results remain in
`Test/App/TestResults/`. Keep screenshots local according to `AGENTS.md`.

The cloud computer uses Mesa llvmpipe software rendering. Its frame times do not
represent Windows GPU performance, and the current cloud audio backend is NoSound.

## Opt-in low-spec rendering

Start `./City --low-spec` from `App/` (Windows: `City.exe --low-spec`).
The option also works with playtests or saved worlds; put it before the final
`--new` or `--load <saveName>` argument, for example:

```sh
./City --low-spec --playtest --seed 42 --new
./City --low-spec --load my_city
```

This profile renders only the 3D world at two-thirds width and height, without
MSAA or cast shadows. At the default window size the 3D target is 853×512 instead
of 1280×768. Text, HUD, maps, selection outlines and input coordinates remain at
native resolution. Lighting, fog, world geometry, traffic and simulation rules
are unchanged. Expect softer 3D edges and no cast shadows; select 標準 in Settings to
restore normal rendering. No city save-data change is involved.

This is a rendering fallback, not a guarantee for a particular minimum GPU or
RAM size. Large-world geometry and simulation memory are not capped by it.
Measured comparisons and limitations are recorded in `artifacts/low_spec/REVIEW.md`.

## Adjustable scenery distance

Use `./City --render-distance 1500 --load my_city` or combine it with
`--low-spec`. Place these options before the final `--new` / `--load` argument.
At runtime press `/`, enter `/render distance 1500`, and press Enter. Query the
current setting with `/render distance`; reset with `/render distance 0`.

The allowed finite range is 100–20000 meters, measured in 3D from the camera eye.
The default 0 adds no distance cutoff and retains existing frustum/LOD behavior.
This controls buildings, trees and their associated scenery and shadows. Ground,
fields, roads, transport and simulation retain their existing behavior. Large
objects and already-merged batches use conservative geometry bounds, so a batch
straddling the range can remain partly visible beyond it. It is a draw-submission
setting, not a world-streaming or memory cap; geometry caches are reused when the
range increases. CLI/command changes last for the current process unless applied
in Settings; preferences are never stored in city data.

## Settings screen

Use **設定** at the top-right of the title screen, or **Esc → 設定** while
playing. The screen groups rendering quality (標準 / 軽量), building/tree draw
distance (0, or 100–20000 m), and effect volume. 0 means no additional distance
limit; existing visibility/LOD behavior still applies. Terrain and roads are
not affected by that distance limit. HUD, maps, camera input and picking remain
at native resolution in either quality profile.

Edits are staged: **適用して戻る** prepares rendering resources and saves the
settings before updating the active values; **キャンセル** or Esc discards the
draft. **初期値に戻す** only changes the draft until Apply is clicked. Applying
from the pause menu returns to that menu and keeps the city paused.

Settings are stored locally in `App/settings.json`, separately from city saves.
Startup precedence is built-in defaults, then a valid settings file, then
explicit CLI options (`--low-spec`, `--render-distance`). CLI overrides and
`/render distance` commands remain session-only unless explicitly saved with
Apply in the settings screen. Invalid/corrupt settings files fall back safely
to defaults. A failed write leaves both the previous file and active settings
unchanged and shows an error.
