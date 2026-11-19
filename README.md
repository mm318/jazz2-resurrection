# Jazz² Resurrection

An open-source reimplementation of Jazz Jackrabbit 2, with local and online
multiplayer, scripting, and a built-in level editor.

You need files from your own Jazz Jackrabbit 2 installation to play. The game
supports the original game, Shareware Demo, Holiday Hare ’98, The Secret Files,
and Christmas Chronicles.

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
`Content` directory beside it. Copy your original game files, including
`Anims.j2a`, levels, tilesets, and music, into `zig-out/bin/Source/`.
The game converts them into a local cache when needed.

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
supplies the cross-origin isolation needed by pthreads. Original game data is
not included in the browser bundle.

To play with your own game data in the browser, prepare a Content directory and
select it when building:

```sh
zig build pack-assets -- convert /path/to/original-game /tmp/jazz2-content --content=Content --target=emscripten
zig build -Dtarget=wasm32-emscripten -Doptimize=fast -Dcontent=/tmp/jazz2-content
zig build -Dtarget=wasm32-emscripten -Doptimize=fast -Dcontent=/tmp/jazz2-content run
```

The prepared directory is preloaded into the WebAssembly filesystem.

## Tools and shaders

```sh
zig build tools                 # Host ShaderCompiler and AssetPacker
zig build shader-compiler
zig build asset-packer
zig build shaders               # Generate shader headers in the build cache
zig build test                  # Shared-library regression checks
```

Normal game builds generate shader headers automatically from `Sources/Shaders`.
Edits to shaders or their includes invalidate the cached output. The generator
runs on the build host, including when cross-compiling the game. It emits GLSL
and reflection metadata; the driver or browser compiles the GLSL for the GPU at
runtime. No separate shader SDK is required.

Asset conversion is explicit. For example:

```sh
zig build pack-assets -- convert /path/to/original-game /path/to/converted --content=Content --target=desktop
```

The build compiles separate Shared, nCine, and Jazz2 game libraries, with
individual dependency builds under `deps/`. Generated shaders and compiler
outputs stay outside the source tree.

[Project website](https://de4th.dev/jazz2/) · [License](LICENSE)
