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
are unchanged. Expect softer 3D edges and no cast shadows; omit the option to
restore normal rendering on the next launch. No save-data change is involved.

This is a rendering fallback, not a guarantee for a particular minimum GPU or
RAM size. Large-world geometry and simulation memory are not capped by it.
Measured comparisons and limitations are recorded in `artifacts/low_spec/REVIEW.md`.
