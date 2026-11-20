# Jazz² Resurrection

An open-source reimplementation of Jazz Jackrabbit 2, with local and online
multiplayer, scripting, and a built-in level editor.

Runtime game data is packaged in `assets/`, including converted animations,
sounds, episodes, levels, tilesets, music, and cinematics. The game loads this
data directly; it does not scan a `Source/` directory or convert files on startup.

## Build and play

Use [Zig 0.17.1](https://ziglang.org/download/). Linux, Windows, macOS, and
WebAssembly are supported, using SDL 2.32.10 for windowing, input, and audio.
Desktop rendering uses OpenGL 3.3; browsers use WebGL 2.

On Ubuntu, install the native system development libraries:

```sh
sudo apt install libxext-dev libgles-dev libglx-dev libpulse-dev libcurl4-openssl-dev libssl-dev
```

From this directory:

```sh
zig build
zig build -Doptimize=fast
zig build -Doptimize=fast run
```

The executable is `zig-out/bin/Jazz2` (`Jazz2.exe` on Windows). Keep its
`assets` directory beside it. The build installs the packaged game data there.
`Cache/` holds generated shader data and other temporary runtime files.
The default desktop window is 1440 × 810.

`debug`, `safe`, `fast`, and `small` are the optimization modes. The editor,
AngelScript support, and online multiplayer are enabled by default and can be
selected with `-Deditor=false`, `-Dscripting=false`, and `-Donline=false`.

SDL and the compression, audio, scripting, and OpenGL loader libraries are built
from vendored sources. Native Linux uses system libcurl and OpenSSL for HTTPS;
macOS uses libcurl and the system Security framework; Windows uses WinHTTP and
Schannel. See [deps/README.md](deps/README.md) for the dependency inventory.

## Browser build

Install the pinned Emscripten 7.0.0 SDK once:

```sh
zig build install-emsdk
zig build -Dtarget=wasm32-emscripten -Doptimize=fast
zig build -Dtarget=wasm32-emscripten -Doptimize=fast run
```

Append `-- --no_browser` to serve without opening a browser. Deploy the complete
`zig-out/web` directory, including the HTML, JavaScript, WebAssembly, data file,
and `coi-serviceworker.js`. Serve over localhost or HTTPS; the service worker
supplies the cross-origin isolation needed by pthreads. The browser bundle
preloads the same packaged game data as the desktop build.

Use `-Dcontent=/path/to/prepared-assets` to build with a different complete
assets directory containing the converted runtime files under `Animations/`,
`Sounds/`, `Cinematics/`, `Episodes/`, `Tilesets/`, and `Music/`.

## Tools and shaders

```sh
zig build tools                 # Host ShaderCompiler and AssetPacker
zig build shader-compiler
zig build asset-packer
zig build shaders               # Generate shader headers in the build cache
zig build test                  # Shared-library regression checks
```

Normal game builds generate shader headers automatically from `src/Shaders`.
Edits to shaders or their includes invalidate the cached output. The generator
runs on the build host, including when cross-compiling the game. It emits GLSL
and reflection metadata; the driver or browser compiles the GLSL for the GPU at
runtime. No separate shader SDK is required.

Asset conversion is an explicit developer operation. To prepare replacement game
data without copying unused original files:

```sh
zig build pack-assets -- convert /path/to/original-game /tmp/jazz2-content --target=desktop
```

Copy the generated `Animations/`, `Sounds/`, `Cinematics/`, `Episodes/`,
`Tilesets/`, and `Music/` into `assets/` before rebuilding. `Anims.j2a` produces
individual `.aura` sprite sheets and `.wav` sound effects; `Data.j2d` produces
menu textures and `.j2sfx` cinematic cue lists. Desktop and browser builds load
these files directly without `Prebaked.pak`. The converter selects tilesets
and music used by the levels, includes menu music and the intro/ending videos,
and leaves out installers, manuals, screenshots, and unrelated data.

The build compiles separate Shared, nCine, and Jazz2 game libraries, with
individual dependency builds under `deps/`. Generated shaders and compiler
outputs stay outside the source tree.

[Project website](https://de4th.dev/jazz2/) · [License](LICENSE)
