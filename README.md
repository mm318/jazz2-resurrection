# Jazz² Resurrection

An open-source reimplementation of Jazz Jackrabbit 2, with local and online
multiplayer, scripting, and a built-in level editor.

Runtime game data is packaged in `assets/`, including converted animations,
sounds, episodes, levels, tilesets, music, and cinematics. Desktop and browser
builds load the same prepared assets.

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
`zig-out/web` directory. It contains the game HTML, JavaScript, and WebAssembly,
plus the files used to load its assets:

- `index.data.br` and `index.data.json`: the compressed bundle and its size and checksum metadata.
- `asset-loader.js` and `asset-worker.js`: download, decompression, and startup.
- `brotli.wasm` and `brotli.LICENSE`: the decoder and its license.
- `coi-serviceworker.js`: cross-origin isolation for pthreads.

The build compresses the bundle with Brotli quality 11 using the SDK's bundled
Node. The browser downloads, decompresses, and verifies it in a worker before
starting Emscripten. Plain static hosting, including GitHub Pages project sites
deployed through Actions, is supported without custom compression headers.

Serve over localhost or HTTPS so the service worker can supply the cross-origin
isolation needed by pthreads. All loader URLs are relative to the deployed game,
so hosting under a repository subdirectory works.

Use `-Dcontent=/path/to/prepared-assets` to build with a different complete
assets directory containing the converted runtime files under `Animations/`,
`Sounds/`, `Cinematics/`, `Episodes/`, `Tilesets/`, and `Music/`.

## Tools and shaders

```sh
zig build tools                 # Host ShaderCompiler and AssetPacker
zig build shader-compiler
zig build asset-packer
zig build shaders               # Generate shader headers in the build cache
zig build test                  # Shared-library, audio, and asset-tool checks
```

Normal game builds generate shader headers automatically from `src/Shaders`.
Edits to shaders or their includes invalidate the cached output. The generator
runs on the build host, including when cross-compiling the game. It emits GLSL
and reflection metadata; the driver or browser compiles the GLSL for the GPU at
runtime.

Asset preparation and installation use `assets/Assets.zig`;
`assets/CompressWeb.cjs` handles browser bundle compression. These tool sources
are excluded from installed game data. Build outputs and generated shaders are
stored in the build cache and `zig-out/`.

To generate runtime assets from an original Jazz Jackrabbit 2 installation:

```sh
zig build pack-assets -- convert /path/to/original-game /tmp/jazz2-content --target=desktop
zig build prepare-assets -- /tmp/jazz2-content
```

The first command converts game resources and collects the tilesets and music
used by the levels, menu music, and intro/ending cinematics. The second command
requires FFmpeg with its `libvorbis` encoder and prepares mono Vorbis sound
effects at 32 kbps and up to 22.05 kHz, plus 320 × 240 cinematics. Repeating
preparation preserves existing Ogg files and prepared cinematics. Keep the
original input files separately for future regeneration.

Copy the prepared `Animations/`, `Sounds/`, `Cinematics/`, `Episodes/`,
`Tilesets/`, and `Music/` into `assets/` before rebuilding. The prepared files
include `.aura` sprite sheets, `.ogg` sound effects, menu textures, and `.j2sfx`
cinematic cue lists. Music uses tracker formats. Vorbis metadata stores source
sample rates for scripted playback frequency calculations.

Translation sources are `.po` files in `assets/Translations/`. Builds install
the compiled `.mo` translations.

The build compiles separate Shared, nCine, and Jazz2 game libraries, with
individual dependency builds under `deps/`.

[Project website](https://de4th.dev/jazz2/) · [License](LICENSE)
