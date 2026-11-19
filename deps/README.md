# Dependencies

The Zig build uses vendored sources for these libraries:

| Directory | Version | Purpose |
| --- | --- | --- |
| `sdl` | 2.32.10 | Window, input, controllers, and queued audio |
| `zlib` | 1.3.1 | Game data, PNG, and archive compression |
| `ogg` | 1.3.5 | Ogg container |
| `vorbis` | 1.3.7 | Vorbis music decoding |
| `openmpt` | 0.7.13 | Tracker music, including J2B |
| `glew` | 2.2.0 | Desktop OpenGL loading on Linux/Windows |
| `angelscript` | Bundled 2.39.0 WIP | Level scripting and native thunks |
| `jsoncpp` | Bundled 1.9.7 | JSON formats |
| `ixwebsocket` | Bundled 11.4.6 | Native WebSocket networking |
| `enet` | Bundled single-header fork | Native UDP networking |
| `parallel_hashmap`, `pdqsort`, `stb` | Bundled | Containers, sorting, and font rendering |

Each compiled dependency has its own `build.zig` and `build.zig.zon`. SDL is
copied from the trimmed distribution in the sibling Destruct project, with its
provenance and platform configuration described in `sdl/README.md`.
The other existing bundled sources are retained from this repository.

Linux uses system libcurl and OpenSSL for HTTPS. macOS uses system libcurl and
Secure Transport; Windows uses WinHTTP and Schannel. Browser builds use the
browser's networking and WebGL implementations. The SDL audio backend needs
no OpenAL library. GLEW is unnecessary on macOS and WebAssembly.

The parallel_hashmap 64-bit hash combiner takes its mixing value as `uint64_t`
to keep its 47-bit shift well-defined while compiling on wasm32.
ENet uses MinGW's `clock_gettime` instead of defining a conflicting Windows shim.
Vorbis uses Clang's portable `__builtin_alloca`. libopenmpt uses zlib and Vorbis
from these same builds; its Emscripten build enables pthreads and Emscripten
exception/longjmp lowering.

See each dependency's license and `VENDORED.md` where supplied.
