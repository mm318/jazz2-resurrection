# AssetPacker browser frontend

These files implement the standalone browser UI for importing original game data.
The repository's supported utility build is the host tool:

```sh
zig build asset-packer
zig build pack-assets -- --help
```

The game browser build is produced by `zig build -Dtarget=wasm32-emscripten`.
