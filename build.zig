const std = @import("std");
const Build = std.Build;
const builtin = @import("builtin");

const Layer = enum { shared, engine, game, application };

const Settings = struct {
    target: Build.ResolvedTarget,
    optimize: std.builtin.Optimize,
    sysroot: ?Build.LazyPath = null,
    generated: ?Build.LazyPath = null,
    editor: bool = true,
    scripting: bool = true,
    online: bool = true,
};

pub fn build(b: *Build) void {
    var target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const editor = b.option(bool, "editor", "Build the level editor") orelse true;
    const scripting = b.option(bool, "scripting", "Enable AngelScript levels") orelse true;
    const online = b.option(bool, "online", "Enable online multiplayer") orelse true;
    const assets = b.option(
        Build.LazyPath,
        "content",
        "Prepared assets directory to install or preload",
    ) orelse b.path("assets");
    const asset_tools_module = b.createModule(.{
        .root_source_file = b.path("assets/Assets.zig"),
        .target = b.graph.host,
        .optimize = .fast,
    });
    const asset_tools = b.addExecutable(.{ .name = "AssetTools", .root_module = asset_tools_module });
    // Both targets consume exactly the same prepared runtime files. Keep the
    // translation and build-tool sources out of every installed/preloaded tree.
    const asset_inputs = b.addWriteFiles().addCopyDirectory(assets, "assets", .{
        .exclude_extensions = &.{ ".po", ".zig", ".cjs" },
    });

    const web = target.result.os.tag == .emscripten;
    switch (target.result.os.tag) {
        .linux, .windows, .macos, .emscripten => {},
        else => @panic("Supported targets: Linux, Windows, macOS, wasm32-emscripten"),
    }
    if (web and target.result.cpu.arch != .wasm32) @panic("Emscripten builds require wasm32");

    if (web) {
        var query = target.query;
        query.cpu_features_add.addFeature(@backingInt(std.Target.wasm.Feature.atomics));
        query.cpu_features_add.addFeature(@backingInt(std.Target.wasm.Feature.bulk_memory));
        target = b.resolveTargetQuery(query);
    }

    const emsdk = b.dependency("emsdk", .{});
    const install_sdk = sdkCommand(b, emsdk);
    install_sdk.addArgs(&.{ "install", "7.0.0" });

    const activate_sdk = sdkCommand(b, emsdk);
    activate_sdk.addArgs(&.{ "activate", "7.0.0" });
    activate_sdk.step.dependOn(&install_sdk.step);
    b.step("install-emsdk", "Install the pinned Emscripten 7.0.0 SDK").dependOn(&activate_sdk.step);

    const shader_tool = shaderCompiler(b);
    const install_shader = b.addInstallArtifact(shader_tool, .{});
    b.step("shader-compiler", "Build ShaderCompiler for the build host").dependOn(&install_shader.step);

    const test_mod = b.createModule(.{
        .target = b.graph.host,
        .optimize = .debug,
        .link_libc = true,
        .link_libcpp = true,
        .single_threaded = false,
    });
    test_mod.addIncludePath(b.path("src/Shared"));
    test_mod.addIncludePath(b.path("deps"));
    test_mod.addCSourceFiles(.{
        .files = &.{
            "tests/SharedTests.cpp",
            "src/Utilities/ShaderCompiler/Trace.cpp",
        },
        .flags = &.{
            "-std=c++20",
            "-DDEATH_TRACE",
        },
    });
    test_mod.addIncludePath(b.path("src"));
    test_mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &sources.shader_shared,
        .flags = &.{
            "-std=c++20",
            "-DZIG_BUILD",
            "-DDEATH_TRACE",
        },
    });
    const tests = b.addExecutable(.{ .name = "shared-tests", .root_module = test_mod });
    const test_step = b.step("test", "Test the shared library and asset tooling without game data");
    test_step.dependOn(&b.addRunArtifact(tests).step);
    const asset_tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("assets/Assets.zig"),
        .target = b.graph.host,
        .optimize = .debug,
    }) });
    test_step.dependOn(&b.addRunArtifact(asset_tests).step);
    const audio_tests = b.addRunArtifact(audioAssetTests(b));
    audio_tests.addFileArg(b.path("tests/data/resampled.ogg"));
    test_step.dependOn(&audio_tests.step);

    const asset_tool = assetPacker(b);
    const install_asset = b.addInstallArtifact(asset_tool, .{});
    b.step("asset-packer", "Build AssetPacker for the build host").dependOn(&install_asset.step);

    const tools = b.step("tools", "Build the host asset and shader tools");
    tools.dependOn(&install_shader.step);
    tools.dependOn(&install_asset.step);

    const pack = b.addRunArtifact(asset_tool);
    pack.addPassthruArgs();
    b.step("pack-assets", "Run AssetPacker with explicitly supplied game data").dependOn(&pack.step);

    const prepare = b.addRunArtifact(asset_tools);
    prepare.addArgs(&.{ "prepare", "--asset-packer" });
    prepare.addArtifactArg(asset_tool);
    prepare.addPassthruArgs();
    b.step("prepare-assets", "Prepare converted content in place for desktop and web (requires ffmpeg)").dependOn(&prepare.step);

    const generate = b.addRunArtifact(shader_tool);
    generate.addArgs(&.{ "--generate-all", "--gl-only", "--shaders-dir" });
    // A directory argument alone does not hash its contents. Copying the tree
    // tracks shader includes, edits, additions and removals in the build cache.
    const shader_inputs = b.addWriteFiles().addCopyDirectory(b.path("src/Shaders"), "Shaders", .{});
    generate.addDirectoryArg2(shader_inputs, .{});
    generate.addArg("--out-dir");
    const generated = generate.addOutputDirectoryArg("Shaders/Generated");
    b.step("shaders", "Generate OpenGL/WebGL shader headers in the build cache").dependOn(&generate.step);

    const settings = Settings{
        .target = target,
        .optimize = optimize,
        .sysroot = if (web) emsdk.path("upstream/emscripten/cache/sysroot/include") else null,
        .generated = generated.dirname().dirname(),
        .editor = editor,
        .scripting = scripting,
        .online = online,
    };
    const shared = library(b, "Shared", &sources.shared, settings, .shared);

    const engine = library(b, "nCine", &sources.engine, settings, .engine);
    engine.root_module.linkLibrary(shared);

    const game = library(b, "Jazz2Game", &sources.game, settings, .game);
    game.root_module.linkLibrary(engine);

    const root = createJazz2Module(b, settings, .application);
    const main_flags = [_][]const u8{ "-std=c++20", "-fno-exceptions", "-fno-rtti" };
    root.addCSourceFile(.{
        .file = b.path("src/Main.cpp"),
        .flags = if (web) &(main_flags ++ .{ "-fno-sanitize=undefined", "-pthread" }) else &main_flags,
    });
    root.linkLibrary(game);

    if (web) {
        const lib = b.addLibrary(.{ .name = "Jazz2", .root_module = root });
        const link = b.addRunFile(emTool(b, emsdk, "em++"));
        link.setName("Emscripten link Jazz2");
        link.addArgs(&.{
            if (optimize == .debug) "-Og" else if (optimize == .small) "-Oz" else "-O3",
            "-sMIN_WEBGL_VERSION=2",
            "-sMAX_WEBGL_VERSION=2",
            "-sFULL_ES3=1",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sINITIAL_MEMORY=128MB",
            "-sSTACK_SIZE=4MB",
            "-sMALLOC=emmalloc",
            "-sASYNCIFY=1",
            "-sASYNCIFY_STACK_SIZE=65536",
            "-sFETCH=1",
            "-sFORCE_FILESYSTEM=1",
            "-sGL_ENABLE_GET_PROC_ADDRESS=1",
            "-sSUPPORT_LONGJMP=emscripten",
            "-sDISABLE_EXCEPTION_CATCHING=0",
            "-pthread",
            "-sPTHREAD_POOL_SIZE=4",
            "-lwebsocket.js",
            "-lidbfs.js",
            "--bind",
        });
        if (optimize == .debug) {
            link.addArgs(&.{ "-g", "-sASSERTIONS=1", "-sSTACK_OVERFLOW_CHECK=1" });
        } else {
            link.addArgs(&.{ "-flto", "--closure", "1" });
        }

        link.addPrefixedFileArg("--shell-file=", b.path("web_assets/shell.html"));
        link.addArg("--preload-file");
        link.addDirectoryArg2(asset_inputs, .{ .suffix = "@/assets" });
        link.addArtifactArg(lib);

        for (lib.getCompileDependencies(false)) |dependency| {
            if (dependency != lib and dependency.kind == .lib) link.addArtifactArg(dependency);
        }

        link.addArg("-o");
        const output = link.addOutputFileArg("index.html");

        const package = b.addRunArtifact(asset_tools);
        package.addArg("package-web");
        package.addDirectoryArg2(output.dirname(), .{});
        const web_output = package.addOutputDirectoryArg("web");
        package.addDirectoryArg2(emsdk.path("."), .{});
        const web_assets = b.addWriteFiles().addCopyDirectory(b.path("web_assets"), "web_assets", .{});
        package.addDirectoryArg2(web_assets, .{});
        const decoder = b.addRunFile(emTool(b, emsdk, "emcc"));
        decoder.addArgs(&.{ "-Oz", "-DNDEBUG", "-DBROTLI_BUILD_PORTABLE", "-Ideps/brotli/c/include", "-sSTANDALONE_WASM=1", "-sALLOW_MEMORY_GROWTH=1", "-sMALLOC=emmalloc", "-sABORTING_MALLOC=0", "-sFILESYSTEM=0", "-sASSERTIONS=0", "-sSTACK_SIZE=64KB", "-sEXPORTED_FUNCTIONS=_malloc,_free,_BrotliDecoderDecompress", "--no-entry" });
        const decoder_sources = b.addWriteFiles().addCopyDirectory(b.path("deps/brotli"), "brotli", .{});
        decoder.addArg("-I");
        decoder.addDirectoryArg2(decoder_sources, .{ .suffix = "/c/include" });
        for ([_][]const u8{ "common/constants.c", "common/context.c", "common/dictionary.c", "common/platform.c", "common/shared_dictionary.c", "common/transform.c", "dec/bit_reader.c", "dec/decode.c", "dec/huffman.c", "dec/prefix.c", "dec/state.c", "dec/static_init.c" }) |file| decoder.addFileArg(decoder_sources.path(b, b.fmt("c/{s}", .{file})));
        decoder.addArg("-o");
        const decoder_output = decoder.addOutputFileArg("brotli.wasm");
        package.addFileArg(decoder_output);

        const install = b.addRunArtifact(asset_tools);
        install.addArg("install");
        install.addDirectoryArg2(web_output, .{});
        install.addDirectoryArg2(.{ .relative = .{ .base = .install_prefix, .sub_path = "web" } }, .{});
        b.getInstallStep().dependOn(&install.step);

        const run = b.addRunFile(emTool(b, emsdk, "emrun"));
        run.addFileArg(.{ .relative = .{ .base = .install_prefix, .sub_path = "web/index.html" } });
        run.addPassthruArgs();
        run.step.dependOn(b.getInstallStep());
        b.step("run", "Run Jazz2 in a browser").dependOn(&run.step);
    } else {
        if (target.result.os.tag == .windows) {
            root.addWin32ResourceFile(.{
                .file = b.path("src/Resources.rc"),
                .include_paths = &.{b.path("src")},
            });
        }

        const exe = b.addExecutable(.{
            .name = "Jazz2",
            .root_module = root,
            .win32_manifest = if (target.result.os.tag == .windows) b.path("src/App.manifest") else null,
        });
        if (target.result.os.tag == .windows) {
            exe.subsystem = .windows;
            exe.mingw_unicode_entry_point = true;
        }
        b.installArtifact(exe);
        const install_assets = b.addRunArtifact(asset_tools);
        install_assets.addArg("install");
        install_assets.addDirectoryArg2(asset_inputs, .{});
        install_assets.addDirectoryArg2(.{ .relative = .{ .base = .install_prefix, .sub_path = "bin/assets" } }, .{});
        b.getInstallStep().dependOn(&install_assets.step);

        const run = b.addRunFile(.{
            .relative = .{
                .base = .install_prefix,
                .sub_path = if (target.result.os.tag == .windows) "bin/Jazz2.exe" else "bin/Jazz2",
            },
        });
        run.addPassthruArgs();
        run.step.dependOn(b.getInstallStep());
        b.step("run", "Run Jazz2 on the desktop").dependOn(&run.step);
    }
}

fn createJazz2Module(b: *Build, cfg: Settings, layer: Layer) *Build.Module {
    const mod = b.createModule(.{
        .target = cfg.target,
        .optimize = cfg.optimize,
        .link_libc = true,
        .link_libcpp = true,
        .single_threaded = false,
    });
    if (cfg.generated) |generated| mod.addIncludePath(generated);

    for ([_][]const u8{
        "src",
        "src/Shared",
        "deps",
        "deps/angelscript/include",
    }) |include| mod.addIncludePath(b.path(include));
    if (cfg.sysroot) |include| mod.addSystemIncludePath(include);

    for ([_][]const u8{
        "ZIG_BUILD",
        "DEATH_TRACE",
        "WITH_SDL2",
        "SDL_MAIN_HANDLED",
        "WITH_RHI_GL",
        "WITH_AUDIO",
        "WITH_SDLAUDIO",
        "WITH_VORBIS",
        "WITH_OPENMPT",
        "WITH_ZLIB",
        "WITH_THREADS",
        "WITH_MULTIPLAYER",
        "WITH_WEBSOCKET",
        "WITH_WEBSOCKET_TLS",
    }) |name| {
        const websocket = std.mem.eql(u8, name, "WITH_WEBSOCKET") or
            std.mem.eql(u8, name, "WITH_WEBSOCKET_TLS");
        if (!cfg.online and websocket) continue;
        mod.addCMacro(name, "1");
    }

    mod.addCMacro("NCINE_VERSION", "\"3.9.0\"");
    mod.addCMacro("NCINE_BUILD_YEAR", "\"2026\"");
    mod.addCMacro("NCINE_APP_NAME", "\"Jazz² Resurrection\"");

    if (cfg.editor) {
        mod.addCMacro("WITH_EDITOR", "1");
        mod.addCMacro("WITH_UI_WINDOWS", "1");
    }
    if (cfg.scripting) mod.addCMacro("WITH_ANGELSCRIPT", "1");
    if (cfg.online) mod.addCMacro("WITH_ONLINE_MULTIPLAYER", "1");
    if (cfg.optimize == .debug) mod.addCMacro("DEATH_DEBUG", "1");

    const options = .{ .target = cfg.target, .optimize = .fast, .emscripten_include = cfg.sysroot };
    const zlib = b.dependency("zlib", options);
    mod.addIncludePath(zlib.path("."));
    if (layer == .shared) mod.linkLibrary(zlib.artifact("z"));

    if (layer != .shared) {
        const sdl = b.dependency("sdl", options).artifact("SDL2");
        mod.addIncludePath(sdl.getEmittedIncludeTree());
        mod.addIncludePath(sdl.getEmittedIncludeTree().path(b, "SDL2"));
        if (layer == .engine) mod.linkLibrary(sdl);

        for ([_][2][]const u8{ .{ "vorbis", "vorbis" }, .{ "openmpt", "openmpt" } }) |dep| {
            const dependency = b.dependency(dep[0], options);
            mod.addIncludePath(dependency.path(if (std.mem.eql(u8, dep[0], "vorbis")) "include" else "libopenmpt"));
            if (layer == .engine) mod.linkLibrary(dependency.artifact(dep[1]));
        }
        mod.addIncludePath(b.dependency("ogg", options).path("include"));
    }

    if (layer == .game or layer == .application) {
        const json = b.dependency("jsoncpp", options);
        mod.addIncludePath(json.path("."));
        if (layer == .game) {
            mod.linkLibrary(json.artifact("jsoncpp"));
            if (cfg.scripting) mod.linkLibrary(b.dependency("angelscript", options).artifact("angelscript"));
            if (cfg.online and cfg.target.result.os.tag != .emscripten) {
                mod.linkLibrary(b.dependency("ixwebsocket", options).artifact("ixwebsocket"));
            }
        }
    }

    if (cfg.target.result.os.tag == .emscripten) {
        mod.addCMacro("EMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES", "0");
        mod.addCMacro("RHI_GL_PROFILE_ES", "1");
        mod.addCMacro("RHI_GL_PROFILE_ES3", "1");
    } else {
        mod.addCMacro("RHI_GL_PROFILE_CORE", "1");
        mod.addCMacro("WITH_LEVEL_PACKS", "1");

        if (layer != .shared and cfg.target.result.os.tag != .macos) {
            const glew = b.dependency("glew", options);
            if (layer == .engine) mod.linkLibrary(glew.artifact("GLEW"));
            mod.addIncludePath(glew.path("include"));
            mod.addCMacro("WITH_GLEW", "1");
            mod.addCMacro("GLEW_STATIC", "1");
        }

        if (cfg.target.result.os.tag == .windows) {
            mod.addCMacro("UNICODE", "1");
            mod.addCMacro("_UNICODE", "1");
            for ([_][]const u8{
                "opengl32",
                "winhttp",
                "comctl32",
                "ws2_32",
                "iphlpapi",
                "shlwapi",
                "version",
                "ole32",
                "uuid",
            }) |name| mod.linkSystemLibrary(name, .{});
        } else {
            mod.addCMacro("WITH_CURL", "1");
            if (layer == .shared) mod.linkSystemLibrary("curl", .{});
            if (cfg.target.result.os.tag == .linux) {
                mod.addCMacro("DEATH_USE_GMTOFF_IN_TM", "1");
                if (layer == .engine) mod.linkSystemLibrary("GL", .{});
            }
            if (layer == .engine and cfg.target.result.os.tag == .macos) mod.linkFramework("OpenGL", .{});
        }
    }

    return mod;
}

fn library(b: *Build, name: []const u8, files: []const []const u8, cfg: Settings, layer: Layer) *Build.Step.Compile {
    const mod = createJazz2Module(b, cfg, layer);
    var selected: std.ArrayList([]const u8) = .empty;

    for (files) |file| {
        if (cfg.target.result.os.tag == .windows and std.mem.endsWith(u8, file, "PosixThreadSync.cpp")) continue;
        const windows_only = std.mem.endsWith(u8, file, "WindowsThreadSync.cpp") or
            std.mem.endsWith(u8, file, "AuxiliaryWindowWin32.cpp");
        if (cfg.target.result.os.tag != .windows and windows_only) continue;
        selected.append(b.allocator, file) catch @panic("OOM");
    }

    const flags = [_][]const u8{
        "-std=c++20",
        "-fno-exceptions",
        "-fno-rtti",
        "-Wno-deprecated-declarations",
        "-Wno-braced-scalar-init",
        "-Wno-invalid-constexpr",
        "-Wno-switch",
        "-Wno-deprecated-enum-enum-conversion",
    };
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = selected.items,
        .flags = if (cfg.sysroot != null) &(flags ++ .{
            "-fno-sanitize=undefined",
            "-pthread",
        }) else &flags,
    });

    return b.addLibrary(.{ .name = name, .root_module = mod });
}

fn shaderShared(b: *Build) *Build.Step.Compile {
    const shared = b.createModule(.{
        .target = b.graph.host,
        .optimize = .fast,
        .link_libc = true,
        .link_libcpp = true,
        .single_threaded = false,
    });
    for ([_][]const u8{ "src", "src/Shared" }) |include| shared.addIncludePath(b.path(include));

    shared.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &sources.shader_shared,
        .flags = &.{
            "-std=c++20",
            "-DZIG_BUILD",
            "-DUNICODE",
            "-D_UNICODE",
            "-DDEATH_TRACE",
            "-Wno-deprecated-declarations",
        },
    });

    return b.addLibrary(.{ .name = "SharedHost", .root_module = shared });
}

fn audioAssetTests(b: *Build) *Build.Step.Compile {
    const options = .{ .target = b.graph.host, .optimize = .fast, .emscripten_include = @as(?Build.LazyPath, null) };
    const vorbis = b.dependency("vorbis", options);
    const ogg = b.dependency("ogg", options);
    const mod = b.createModule(.{
        .target = b.graph.host,
        .optimize = .debug,
        .link_libc = true,
        .link_libcpp = true,
    });
    for ([_][]const u8{ "src", "src/Shared", "deps" }) |include| mod.addIncludePath(b.path(include));
    mod.addIncludePath(vorbis.path("include"));
    mod.addIncludePath(ogg.path("include"));
    mod.linkLibrary(vorbis.artifact("vorbis"));
    mod.linkLibrary(shaderShared(b));
    mod.addCSourceFiles(.{
        .files = &.{
            "tests/AudioAssetTests.cpp",
            "src/Utilities/ShaderCompiler/Trace.cpp",
            "src/nCine/Audio/IAudioLoader.cpp",
            "src/nCine/Audio/AudioLoaderOgg.cpp",
            "src/nCine/Audio/AudioReaderOgg.cpp",
            "src/nCine/Audio/AudioLoaderWav.cpp",
            "src/nCine/Audio/AudioReaderWav.cpp",
        },
        .flags = &.{ "-std=c++20", "-DZIG_BUILD", "-DDEATH_TRACE", "-DWITH_AUDIO", "-DWITH_VORBIS" },
    });
    if (b.graph.host.result.os.tag == .windows) mod.linkSystemLibrary("shlwapi", .{});
    return b.addExecutable(.{ .name = "audio-asset-tests", .root_module = mod });
}

fn shaderCompiler(b: *Build) *Build.Step.Compile {
    const base = shaderShared(b);

    const mod = b.createModule(.{
        .target = b.graph.host,
        .optimize = .fast,
        .link_libc = true,
        .link_libcpp = true,
        .single_threaded = false,
    });
    for ([_][]const u8{ "src", "src/Shared" }) |include| mod.addIncludePath(b.path(include));
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &sources.shader_tool,
        .flags = &.{
            "-std=c++20",
            "-DZIG_BUILD",
            "-DUNICODE",
            "-D_UNICODE",
            "-DDEATH_TRACE",
            "-Wno-deprecated-declarations",
        },
    });
    mod.linkLibrary(base);
    if (b.graph.host.result.os.tag == .windows) mod.linkSystemLibrary("shlwapi", .{});

    return b.addExecutable(.{ .name = "ShaderCompiler", .root_module = mod });
}

fn emTool(b: *Build, emsdk: *Build.Dependency, name: []const u8) Build.LazyPath {
    return emsdk.path(b.fmt("upstream/emscripten/{s}{s}", .{
        name,
        if (builtin.os.tag == .windows) ".bat" else "",
    }));
}

fn sdkCommand(b: *Build, emsdk: *Build.Dependency) *Build.Step.Run {
    if (builtin.os.tag == .windows) return b.addRunFile(emsdk.path("emsdk.bat"));

    const run = b.addSystemCommand(&.{"bash"});
    run.addFileArg(emsdk.path("emsdk"));

    return run;
}

fn assetPacker(b: *Build) *Build.Step.Compile {
    const options = .{
        .target = b.graph.host,
        .optimize = .fast,
        .emscripten_include = @as(?Build.LazyPath, null),
    };
    const zlib = b.dependency("zlib", options);

    const flags = [_][]const u8{
        "-std=c++20",
        "-fno-exceptions",
        "-DZIG_BUILD",
        "-DUNICODE",
        "-D_UNICODE",
        "-DDEATH_TRACE",
        "-DWITH_ZLIB",
        "-DWITH_THREADS",
        "-DWITH_PNG_ENCODER",
        "-DNCINE_VERSION=\"3.9.0\"",
        "-DNCINE_APP_NAME=\"Jazz2 Resurrection\"",
    };

    const base = b.createModule(.{
        .target = b.graph.host,
        .optimize = .fast,
        .link_libc = true,
        .link_libcpp = true,
        .single_threaded = false,
    });
    for ([_][]const u8{ "src", "src/Shared", "deps" }) |include| base.addIncludePath(b.path(include));
    base.addIncludePath(zlib.path("."));
    base.linkLibrary(zlib.artifact("z"));

    for (sources.shared) |file| {
        const browser_io = std.mem.endsWith(u8, file, "WebRequest.cpp") or
            std.mem.endsWith(u8, file, "EmscriptenFileStream.cpp");
        if (browser_io) continue;
        base.addCSourceFile(.{ .file = b.path(b.fmt("src/{s}", .{file})), .flags = &flags });
    }

    const support = b.addLibrary(.{ .name = "AssetSharedHost", .root_module = base });

    const mod = b.createModule(.{
        .target = b.graph.host,
        .optimize = .fast,
        .link_libc = true,
        .link_libcpp = true,
        .single_threaded = false,
    });
    for ([_][]const u8{ "src", "src/Shared", "deps" }) |include| mod.addIncludePath(b.path(include));
    mod.addIncludePath(zlib.path("."));
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &sources.asset_tool, .flags = &flags });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &sources.converters, .flags = &flags });
    mod.addCSourceFile(.{ .file = b.path("src/nCine/Base/Algorithms.cpp"), .flags = &flags });
    mod.addCSourceFile(.{ .file = b.path("src/nCine/Graphics/PngCodec.cpp"), .flags = &flags });

    for ([_][]const u8{
        "src/Jazz2/WeaponDefinition.cpp",
        "src/Jazz2/Formats/WeaponsChunk.cpp",
        "src/Jazz2/Formats/LevelTextsChunk.cpp",
        "src/nCine/Threading/Thread.cpp",
        "src/Utilities/AssetPacker/N64Content.cpp",
    }) |file| mod.addCSourceFile(.{
        .file = b.path(file),
        .flags = &flags,
    });

    const thread_sync = if (b.graph.host.result.os.tag == .windows)
        "src/nCine/Threading/WindowsThreadSync.cpp"
    else
        "src/nCine/Threading/PosixThreadSync.cpp";
    mod.addCSourceFile(.{
        .file = b.path(thread_sync),
        .flags = &flags,
    });

    mod.linkLibrary(b.dependency("jsoncpp", options).artifact("jsoncpp"));
    mod.linkLibrary(support);
    if (b.graph.host.result.os.tag == .windows) mod.linkSystemLibrary("shlwapi", .{});

    return b.addExecutable(.{ .name = "AssetPacker", .root_module = mod });
}

const sources = struct {
    pub const shared = [_][]const u8{
        "Shared/Environment.cpp",
        "Shared/Cpu.cpp",
        "Shared/Utf8.cpp",
        "Shared/Base/Format.cpp",
        "Shared/Containers/DateTime.cpp",
        "Shared/Containers/SmallVector.cpp",
        "Shared/Containers/String.cpp",
        "Shared/Containers/StringUtils.cpp",
        "Shared/Containers/StringView.cpp",
        "Shared/Core/Logger.cpp",
        "Shared/Cryptography/Sha256.cpp",
        "Shared/Cryptography/xxHash.cpp",
        "Shared/IO/BoundedFileStream.cpp",
        "Shared/IO/EmscriptenFileStream.cpp",
        "Shared/IO/FileStream.cpp",
        "Shared/IO/FileSystem.cpp",
        "Shared/IO/MemoryStream.cpp",
        "Shared/IO/PakFile.cpp",
        "Shared/IO/Stream.cpp",
        "Shared/IO/WebRequest.cpp",
        "Shared/IO/Compression/DeflateStream.cpp",
        "Shared/IO/Compression/Lz4Stream.cpp",
        "Shared/IO/Compression/ZstdStream.cpp",
        "Shared/Threading/Implementation/WaitOnAddress.cpp",
    };

    pub const converters = [_][]const u8{
        "Jazz2/Compatibility/AnimSetMapping.cpp",
        "Jazz2/Compatibility/AssetConverter.cpp",
        "Jazz2/Compatibility/EventConverter.cpp",
        "Jazz2/Compatibility/J2vRecompressor.cpp",
        "Jazz2/Compatibility/JJ2Anims.cpp",
        "Jazz2/Compatibility/JJ2Block.cpp",
        "Jazz2/Compatibility/JJ2Data.cpp",
        "Jazz2/Compatibility/JJ2Episode.cpp",
        "Jazz2/Compatibility/JJ2Level.cpp",
        "Jazz2/Compatibility/JJ2Strings.cpp",
        "Jazz2/Compatibility/JJ2Tileset.cpp",
        "Jazz2/Formats/LevelFile.cpp",
        "Jazz2/Formats/LevelPreview.cpp",
        "Jazz2/Formats/TileSetFile.cpp",
        "Jazz2/Tiles/LayerTypes.cpp",
    };

    pub const engine = [_][]const u8{
        "nCine/AppConfiguration.cpp",
        "nCine/Application.cpp",
        "nCine/I18n.cpp",
        "nCine/ServiceLocator.cpp",
        "nCine/Audio/AudioBuffer.cpp",
        "nCine/Audio/AudioStream.cpp",
        "nCine/Audio/IAudioLoader.cpp",
        "nCine/Audio/IAudioPlayer.cpp",
        "nCine/Base/BitArray.cpp",
        "nCine/Base/Clock.cpp",
        "nCine/Base/FrameStatistics.cpp",
        "nCine/Base/FrameTimer.cpp",
        "nCine/Base/HashFunctions.cpp",
        "nCine/Base/Object.cpp",
        "nCine/Base/Random.cpp",
        "nCine/Base/Timer.cpp",
        "nCine/Base/TimeStamp.cpp",
        "nCine/Graphics/AnimatedSprite.cpp",
        "nCine/Graphics/BaseSprite.cpp",
        "nCine/Graphics/BinaryShaderCache.cpp",
        "nCine/Graphics/Camera.cpp",
        "nCine/Graphics/DrawableNode.cpp",
        "nCine/Graphics/Geometry.cpp",
        "nCine/Graphics/IGfxDevice.cpp",
        "nCine/Graphics/AuxiliaryWindow.cpp",
        "nCine/Graphics/AuxiliaryWindowPresenter.cpp",
        "nCine/Graphics/RHI/RhiCapabilitiesBase.cpp",
        "nCine/Graphics/ITextureLoader.cpp",
        "nCine/Graphics/ITextureSaver.cpp",
        "nCine/Graphics/Material.cpp",
        "nCine/Graphics/MeshSprite.cpp",
        "nCine/Graphics/Particle.cpp",
        "nCine/Graphics/PngCodec.cpp",
        "nCine/Graphics/ParticleAffectors.cpp",
        "nCine/Graphics/ParticleInitializer.cpp",
        "nCine/Graphics/ParticleSystem.cpp",
        "nCine/Graphics/RectAnimation.cpp",
        "nCine/Graphics/RenderBatcher.cpp",
        "nCine/Graphics/RenderBuffersManager.cpp",
        "nCine/Graphics/RenderCommand.cpp",
        "nCine/Graphics/RenderCommandPool.cpp",
        "nCine/Graphics/RenderQueue.cpp",
        "nCine/Graphics/RenderResources.cpp",
        "nCine/Graphics/RenderStatistics.cpp",
        "nCine/Graphics/RenderVaoPool.cpp",
        "nCine/Graphics/SceneNode.cpp",
        "nCine/Graphics/ScreenViewport.cpp",
        "nCine/Graphics/Shader.cpp",
        "nCine/Graphics/ShaderState.cpp",
        "nCine/Graphics/Sprite.cpp",
        "nCine/Graphics/Texture.cpp",
        "nCine/Graphics/TextureFormat.cpp",
        "nCine/Graphics/TextureLoaderDds.cpp",
        "nCine/Graphics/TextureLoaderKtx.cpp",
        "nCine/Graphics/TextureLoaderPvr.cpp",
        "nCine/Graphics/TextureLoaderRaw.cpp",
        "nCine/Graphics/TextureLoaderPng.cpp",
        "nCine/Graphics/TextureLoaderQoi.cpp",
        "nCine/Graphics/Viewport.cpp",
        "nCine/Input/IInputManager.cpp",
        "nCine/Input/JoyMapping.cpp",
        "nCine/Primitives/Color.cpp",
        "nCine/Primitives/Colorf.cpp",
        "nCine/Primitives/Half.cpp",
        "nCine/Threading/Thread.cpp",
        "Utilities/ShaderCompiler/ConstFold.cpp",
        "Utilities/ShaderCompiler/GlslReflect.cpp",
        "Utilities/ShaderCompiler/RuntimeShader.cpp",
        "Utilities/ShaderCompiler/ShaderParser.cpp",
        "nCine/Graphics/RHI/GL/GLAttribute.cpp",
        "nCine/Graphics/RHI/GL/GLBlending.cpp",
        "nCine/Graphics/RHI/GL/GLBufferObject.cpp",
        "nCine/Graphics/RHI/GL/GLClearColor.cpp",
        "nCine/Graphics/RHI/GL/GLCullFace.cpp",
        "nCine/Graphics/RHI/GL/GLDebug.cpp",
        "nCine/Graphics/RHI/GL/GLDevice.cpp",
        "nCine/Graphics/RHI/GL/GLDepthTest.cpp",
        "nCine/Graphics/RHI/GL/GLFramebuffer.cpp",
        "nCine/Graphics/RHI/GL/GLRenderbuffer.cpp",
        "nCine/Graphics/RHI/GL/GLRenderTarget.cpp",
        "nCine/Graphics/RHI/GL/GLRhiCapabilities.cpp",
        "nCine/Graphics/RHI/GL/GLScissorTest.cpp",
        "nCine/Graphics/RHI/GL/GLShader.cpp",
        "nCine/Graphics/RHI/GL/GLShaderProgram.cpp",
        "nCine/Graphics/RHI/GL/GLShaderUniformBlocks.cpp",
        "nCine/Graphics/RHI/GL/GLShaderUniforms.cpp",
        "nCine/Graphics/RHI/GL/GLTexture.cpp",
        "nCine/Graphics/RHI/GL/GLTextureFormat.cpp",
        "nCine/Graphics/RHI/GL/GLUniform.cpp",
        "nCine/Graphics/RHI/GL/GLUniformBlock.cpp",
        "nCine/Graphics/RHI/GL/GLUniformBlockCache.cpp",
        "nCine/Graphics/RHI/GL/GLUniformCache.cpp",
        "nCine/Graphics/RHI/GL/GLVertexArrayObject.cpp",
        "nCine/Graphics/RHI/GL/GLVertexFormat.cpp",
        "nCine/Graphics/RHI/GL/GLViewport.cpp",
        "nCine/MainApplication.cpp",
        "nCine/Backends/SdlInputManager.cpp",
        "nCine/Backends/SdlKeys.cpp",
        "nCine/Backends/SdlGfxDevice.cpp",
        "nCine/Backends/SdlAuxiliaryWindow.cpp",
        "nCine/Audio/AudioDeviceBase.cpp",
        "nCine/Audio/AudioBufferPlayer.cpp",
        "nCine/Audio/AudioStreamPlayer.cpp",
        "nCine/Audio/AudioLoaderWav.cpp",
        "nCine/Audio/AudioReaderWav.cpp",
        "nCine/Audio/Backends/SDL/SdlAudioDevice.cpp",
        "nCine/Audio/AudioLoaderOgg.cpp",
        "nCine/Audio/AudioReaderOgg.cpp",
        "nCine/Audio/AudioLoaderMpt.cpp",
        "nCine/Audio/AudioReaderMpt.cpp",
        "nCine/Threading/WindowsThreadSync.cpp",
        "nCine/Threading/PosixThreadSync.cpp",
        "nCine/Threading/ThreadPool.cpp",
        "nCine/Base/Algorithms.cpp",
        "nCine/Backends/AuxiliaryWindowWin32.cpp",
    };

    pub const game = [_][]const u8{
        "Jazz2/Actors/ActorBase.cpp",
        "Jazz2/Actors/Collectibles/AmmoCollectible.cpp",
        "Jazz2/Actors/Collectibles/CarrotCollectible.cpp",
        "Jazz2/Actors/Collectibles/CarrotFlyCollectible.cpp",
        "Jazz2/Actors/Collectibles/CarrotInvincibleCollectible.cpp",
        "Jazz2/Actors/Collectibles/CoinCollectible.cpp",
        "Jazz2/Actors/Collectibles/CollectibleBase.cpp",
        "Jazz2/Actors/Collectibles/FastFireCollectible.cpp",
        "Jazz2/Actors/Collectibles/FoodCollectible.cpp",
        "Jazz2/Actors/Collectibles/GemCollectible.cpp",
        "Jazz2/Actors/Collectibles/GemGiant.cpp",
        "Jazz2/Actors/Collectibles/GemRing.cpp",
        "Jazz2/Actors/Collectibles/OneUpCollectible.cpp",
        "Jazz2/Actors/Collectibles/Stopwatch.cpp",
        "Jazz2/Actors/Enemies/Bat.cpp",
        "Jazz2/Actors/Enemies/Bee.cpp",
        "Jazz2/Actors/Enemies/Bosses/Bilsy.cpp",
        "Jazz2/Actors/Enemies/Bosses/Bolly.cpp",
        "Jazz2/Actors/Enemies/Bosses/BossBase.cpp",
        "Jazz2/Actors/Enemies/Bosses/Bubba.cpp",
        "Jazz2/Actors/Enemies/Bosses/Devan.cpp",
        "Jazz2/Actors/Enemies/Bosses/DevanRemote.cpp",
        "Jazz2/Actors/Enemies/Bosses/Queen.cpp",
        "Jazz2/Actors/Enemies/Bosses/Robot.cpp",
        "Jazz2/Actors/Enemies/Bosses/TurtleBoss.cpp",
        "Jazz2/Actors/Enemies/Bosses/Uterus.cpp",
        "Jazz2/Actors/Enemies/Caterpillar.cpp",
        "Jazz2/Actors/Enemies/Crab.cpp",
        "Jazz2/Actors/Enemies/Demon.cpp",
        "Jazz2/Actors/Enemies/Doggy.cpp",
        "Jazz2/Actors/Enemies/Dragon.cpp",
        "Jazz2/Actors/Enemies/Dragonfly.cpp",
        "Jazz2/Actors/Enemies/EnemyBase.cpp",
        "Jazz2/Actors/Enemies/FatChick.cpp",
        "Jazz2/Actors/Enemies/Fencer.cpp",
        "Jazz2/Actors/Enemies/Fish.cpp",
        "Jazz2/Actors/Enemies/Helmut.cpp",
        "Jazz2/Actors/Enemies/LabRat.cpp",
        "Jazz2/Actors/Enemies/Lizard.cpp",
        "Jazz2/Actors/Enemies/LizardFloat.cpp",
        "Jazz2/Actors/Enemies/MadderHatter.cpp",
        "Jazz2/Actors/Enemies/Monkey.cpp",
        "Jazz2/Actors/Enemies/Rapier.cpp",
        "Jazz2/Actors/Enemies/Raven.cpp",
        "Jazz2/Actors/Enemies/Skeleton.cpp",
        "Jazz2/Actors/Enemies/Sparks.cpp",
        "Jazz2/Actors/Enemies/Sucker.cpp",
        "Jazz2/Actors/Enemies/SuckerFloat.cpp",
        "Jazz2/Actors/Enemies/Turtle.cpp",
        "Jazz2/Actors/Enemies/TurtleShell.cpp",
        "Jazz2/Actors/Enemies/TurtleTough.cpp",
        "Jazz2/Actors/Enemies/TurtleTube.cpp",
        "Jazz2/Actors/Enemies/Witch.cpp",
        "Jazz2/Actors/Environment/AirboardGenerator.cpp",
        "Jazz2/Actors/Environment/AmbientBubbles.cpp",
        "Jazz2/Actors/Environment/AmbientSound.cpp",
        "Jazz2/Actors/Environment/Bird.cpp",
        "Jazz2/Actors/Environment/BirdCage.cpp",
        "Jazz2/Actors/Environment/Bomb.cpp",
        "Jazz2/Actors/Environment/BonusWarp.cpp",
        "Jazz2/Actors/Environment/Checkpoint.cpp",
        "Jazz2/Actors/Environment/Copter.cpp",
        "Jazz2/Actors/Environment/EndOfLevel.cpp",
        "Jazz2/Actors/Environment/Eva.cpp",
        "Jazz2/Actors/Environment/IceBlock.cpp",
        "Jazz2/Actors/Environment/Moth.cpp",
        "Jazz2/Actors/Environment/RollingRock.cpp",
        "Jazz2/Actors/Environment/Spring.cpp",
        "Jazz2/Actors/Environment/SteamNote.cpp",
        "Jazz2/Actors/Environment/SwingingVine.cpp",
        "Jazz2/Actors/Explosion.cpp",
        "Jazz2/Actors/Lighting/FlickerLight.cpp",
        "Jazz2/Actors/Lighting/PulsatingRadialLight.cpp",
        "Jazz2/Actors/Lighting/StaticRadialLight.cpp",
        "Jazz2/Actors/Multiplayer/CtfBase.cpp",
        "Jazz2/Actors/Multiplayer/Flag.cpp",
        "Jazz2/Actors/Multiplayer/GameModeObjectActor.cpp",
        "Jazz2/Actors/Multiplayer/LocalPlayerOnServer.cpp",
        "Jazz2/Actors/Multiplayer/MpPlayer.cpp",
        "Jazz2/Actors/Multiplayer/PlayerOnServer.cpp",
        "Jazz2/Actors/Multiplayer/RemotablePlayer.cpp",
        "Jazz2/Actors/Multiplayer/RemoteActor.cpp",
        "Jazz2/Actors/Multiplayer/RemoteElectroShot.cpp",
        "Jazz2/Actors/Multiplayer/RemotePlayerOnServer.cpp",
        "Jazz2/Actors/Multiplayer/RemoteThunderbolt.cpp",
        "Jazz2/Actors/Player.cpp",
        "Jazz2/Actors/PlayerCorpse.cpp",
        "Jazz2/Actors/Solid/AmmoBarrel.cpp",
        "Jazz2/Actors/Solid/AmmoCrate.cpp",
        "Jazz2/Actors/Solid/BarrelContainer.cpp",
        "Jazz2/Actors/Solid/Bridge.cpp",
        "Jazz2/Actors/Solid/CrateContainer.cpp",
        "Jazz2/Actors/Solid/GemBarrel.cpp",
        "Jazz2/Actors/Solid/GemCrate.cpp",
        "Jazz2/Actors/Solid/GenericContainer.cpp",
        "Jazz2/Actors/Solid/MovingPlatform.cpp",
        "Jazz2/Actors/Solid/PinballBumper.cpp",
        "Jazz2/Actors/Solid/PinballPaddle.cpp",
        "Jazz2/Actors/Solid/Pole.cpp",
        "Jazz2/Actors/Solid/PowerUpMorphMonitor.cpp",
        "Jazz2/Actors/Solid/PowerUpShieldMonitor.cpp",
        "Jazz2/Actors/Solid/PowerUpWeaponMonitor.cpp",
        "Jazz2/Actors/Solid/PushableBox.cpp",
        "Jazz2/Actors/Solid/SpikeBall.cpp",
        "Jazz2/Actors/Solid/TriggerCrate.cpp",
        "Jazz2/Actors/SolidObjectBase.cpp",
        "Jazz2/Actors/Weapons/BlasterShot.cpp",
        "Jazz2/Actors/Weapons/BouncerShot.cpp",
        "Jazz2/Actors/Weapons/CustomShot.cpp",
        "Jazz2/Actors/Weapons/ElectroShot.cpp",
        "Jazz2/Actors/Weapons/FreezerShot.cpp",
        "Jazz2/Actors/Weapons/PepperShot.cpp",
        "Jazz2/Actors/Weapons/RFShot.cpp",
        "Jazz2/Actors/Weapons/SeekerShot.cpp",
        "Jazz2/Actors/Weapons/ShieldFireShot.cpp",
        "Jazz2/Actors/Weapons/ShieldLightningShot.cpp",
        "Jazz2/Actors/Weapons/ShieldWaterShot.cpp",
        "Jazz2/Actors/Weapons/ShotBase.cpp",
        "Jazz2/Actors/Weapons/TNT.cpp",
        "Jazz2/Actors/Weapons/Thunderbolt.cpp",
        "Jazz2/Actors/Weapons/ToasterShot.cpp",
        "Jazz2/Collisions/DynamicTree.cpp",
        "Jazz2/Collisions/DynamicTreeBroadPhase.cpp",
        "Jazz2/Compatibility/AnimSetMapping.cpp",
        "Jazz2/Compatibility/AssetConverter.cpp",
        "Jazz2/Compatibility/EventConverter.cpp",
        "Jazz2/Compatibility/J2vRecompressor.cpp",
        "Jazz2/Compatibility/JJ2Anims.cpp",
        "Jazz2/Compatibility/JJ2Block.cpp",
        "Jazz2/Compatibility/JJ2Data.cpp",
        "Jazz2/Compatibility/JJ2Episode.cpp",
        "Jazz2/Compatibility/JJ2Level.cpp",
        "Jazz2/Compatibility/JJ2Strings.cpp",
        "Jazz2/Compatibility/JJ2Tileset.cpp",
        "Jazz2/ContentResolver.cpp",
        "Jazz2/Editor/Common/BackgroundJob.cpp",
        "Jazz2/Editor/Common/LayerView.cpp",
        "Jazz2/Editor/Common/StampClipboard.cpp",
        "Jazz2/Editor/Common/TileStamp.cpp",
        "Jazz2/Editor/Document/Annotations.cpp",
        "Jazz2/Editor/Document/CommandArchive.cpp",
        "Jazz2/Editor/Document/CommandHistory.cpp",
        "Jazz2/Editor/Document/Commands.cpp",
        "Jazz2/Editor/Document/DocumentChanges.cpp",
        "Jazz2/Editor/Document/DocumentSnapshot.cpp",
        "Jazz2/Editor/Document/DocumentViewState.cpp",
        "Jazz2/Editor/Document/LevelDocument.cpp",
        "Jazz2/Editor/Document/TerrainSplines.cpp",
        "Jazz2/Editor/EditorSelfTest.cpp",
        "Jazz2/Editor/EditorSession.cpp",
        "Jazz2/Editor/EditorSessionActions.cpp",
        "Jazz2/Editor/EditorSessionEditing.cpp",
        "Jazz2/Editor/EditorSessionFiles.cpp",
        "Jazz2/Editor/EditorSessionPlayTest.cpp",
        "Jazz2/Editor/EditorSessionTileSets.cpp",
        "Jazz2/Editor/Events/EventCatalog.Area.cpp",
        "Jazz2/Editor/Events/EventCatalog.Basic.cpp",
        "Jazz2/Editor/Events/EventCatalog.Enemy.cpp",
        "Jazz2/Editor/Events/EventCatalog.Environment.cpp",
        "Jazz2/Editor/Events/EventCatalog.Items.cpp",
        "Jazz2/Editor/Events/EventCatalog.cpp",
        "Jazz2/Editor/Events/EventGizmos.cpp",
        "Jazz2/Editor/Events/EventPreviewResolver.cpp",
        "Jazz2/Editor/Events/EventPreviews.Overlays.cpp",
        "Jazz2/Editor/Events/EventPreviews.Sounds.cpp",
        "Jazz2/Editor/Events/EventPreviews.cpp",
        "Jazz2/Editor/Files/AutosaveService.cpp",
        "Jazz2/Editor/Files/ContentTileSetLibrary.cpp",
        "Jazz2/Editor/Files/EditorSettings.cpp",
        "Jazz2/Editor/Files/EpisodeFile.cpp",
        "Jazz2/Editor/Files/PoFile.cpp",
        "Jazz2/Editor/Files/TileSetDataCache.cpp",
        "Jazz2/Editor/Files/WebFileBridge.cpp",
        "Jazz2/Editor/Imaging/AccentColor.cpp",
        "Jazz2/Editor/Imaging/LevelThumbnail.cpp",
        "Jazz2/Editor/Imaging/PaletteFile.cpp",
        "Jazz2/Editor/Imaging/PaletteMatch.cpp",
        "Jazz2/Editor/Imaging/PngCodec.cpp",
        "Jazz2/Editor/Imaging/Quantizer.cpp",
        "Jazz2/Editor/Imaging/TileImages.cpp",
        "Jazz2/Editor/Input/ActionRegistry.cpp",
        "Jazz2/Editor/Input/TouchGestures.cpp",
        "Jazz2/Editor/Runtime/EditorCamera.cpp",
        "Jazz2/Editor/Runtime/EditorLevelHandler.cpp",
        "Jazz2/Editor/Runtime/MaskOverlay.cpp",
        "Jazz2/Editor/Runtime/PlayPathHistory.cpp",
        "Jazz2/Editor/Runtime/PlayTest.cpp",
        "Jazz2/Editor/Runtime/PlayTestController.cpp",
        "Jazz2/Editor/Runtime/PlayTestDebug.cpp",
        "Jazz2/Editor/Runtime/PlayTestLevelHandler.cpp",
        "Jazz2/Editor/Runtime/PlayTestMpLevelHandler.cpp",
        "Jazz2/Editor/Runtime/PlayTransition.cpp",
        "Jazz2/Editor/Runtime/RuntimeSync.cpp",
        "Jazz2/Editor/Runtime/ScriptGraphics.cpp",
        "Jazz2/Editor/Runtime/TileDrawing.cpp",
        "Jazz2/Editor/Runtime/TileLod.cpp",
        "Jazz2/Editor/Session/DocumentFiles.cpp",
        "Jazz2/Editor/Session/EditorDebugConsole.cpp",
        "Jazz2/Editor/Session/EditorInputRouter.cpp",
        "Jazz2/Editor/Session/PlayOverlays.cpp",
        "Jazz2/Editor/Session/SavedComparison.cpp",
        "Jazz2/Editor/Shell/EditorShell.cpp",
        "Jazz2/Editor/Shell/EpisodeDialog.cpp",
        "Jazz2/Editor/Shell/EventOutline.cpp",
        "Jazz2/Editor/Shell/EventPanels.cpp",
        "Jazz2/Editor/Shell/FilePickers.cpp",
        "Jazz2/Editor/Shell/FindReplaceDialog.cpp",
        "Jazz2/Editor/Shell/FxTileGenerator.cpp",
        "Jazz2/Editor/Shell/HelpDialogs.cpp",
        "Jazz2/Editor/Shell/LayerDialogs.cpp",
        "Jazz2/Editor/Shell/LayerResizeHandles.cpp",
        "Jazz2/Editor/Shell/LevelCheck.cpp",
        "Jazz2/Editor/Shell/LevelDialogs.cpp",
        "Jazz2/Editor/Shell/LevelStatistics.cpp",
        "Jazz2/Editor/Shell/LevelTextLanguages.cpp",
        "Jazz2/Editor/Shell/Navigator.cpp",
        "Jazz2/Editor/Shell/PackageDialog.cpp",
        "Jazz2/Editor/Shell/PaletteDialog.cpp",
        "Jazz2/Editor/Shell/PrefabsDialog.cpp",
        "Jazz2/Editor/Shell/ProgressDialog.cpp",
        "Jazz2/Editor/Shell/PropertyRows.cpp",
        "Jazz2/Editor/Shell/RecoveryDialog.cpp",
        "Jazz2/Editor/Shell/ShellActions.cpp",
        "Jazz2/Editor/Shell/ShellCommon.cpp",
        "Jazz2/Editor/Shell/ShellPanels.cpp",
        "Jazz2/Editor/Shell/SmartFill.cpp",
        "Jazz2/Editor/Shell/TerrainCoverageView.cpp",
        "Jazz2/Editor/Shell/TerrainRulesDialog.cpp",
        "Jazz2/Editor/Shell/TextPrompt.cpp",
        "Jazz2/Editor/Shell/TileEditor.cpp",
        "Jazz2/Editor/Shell/TileSetApplyDialog.cpp",
        "Jazz2/Editor/Shell/TileSetImportDialog.cpp",
        "Jazz2/Editor/Shell/TileSetLayoutDialog.cpp",
        "Jazz2/Editor/Shell/TileSetsDialog.cpp",
        "Jazz2/Editor/Shell/ToolOptionsBar.cpp",
        "Jazz2/Editor/Shell/WeaponPreview.cpp",
        "Jazz2/Editor/Shell/WeaponScripts.cpp",
        "Jazz2/Editor/Shell/WeaponTemplates.cpp",
        "Jazz2/Editor/Shell/WeaponsDialog.cpp",
        "Jazz2/Editor/Smart/RulesFile.cpp",
        "Jazz2/Editor/Smart/SmartTileModel.cpp",
        "Jazz2/Editor/Smart/TerrainRules.cpp",
        "Jazz2/Editor/Smart/TilePrefabs.cpp",
        "Jazz2/Editor/Smart/TileShapes.cpp",
        "Jazz2/Editor/Tests/FakeToolHost.cpp",
        "Jazz2/Editor/Tests/ToolTests.cpp",
        "Jazz2/Editor/Tools/BlockGrab.cpp",
        "Jazz2/Editor/Tools/EventTools.cpp",
        "Jazz2/Editor/Tools/GroundLineTool.cpp",
        "Jazz2/Editor/Tools/MeasureTool.cpp",
        "Jazz2/Editor/Tools/PathTool.cpp",
        "Jazz2/Editor/Tools/ShapeTool.cpp",
        "Jazz2/Editor/Tools/SmartBrushTool.cpp",
        "Jazz2/Editor/Tools/TilePixelTool.cpp",
        "Jazz2/Editor/Tools/TileTools.cpp",
        "Jazz2/Editor/Tools/ToolActions.cpp",
        "Jazz2/Editor/UI/EditorIcons.cpp",
        "Jazz2/Events/EventMap.cpp",
        "Jazz2/Events/EventSpawner.cpp",
        "Jazz2/Formats/LevelFile.cpp",
        "Jazz2/Formats/LevelPreview.cpp",
        "Jazz2/Formats/LevelTextsChunk.cpp",
        "Jazz2/Formats/TileSetFile.cpp",
        "Jazz2/Formats/WeaponsChunk.cpp",
        "Jazz2/Input/ControlScheme.cpp",
        "Jazz2/Input/RgbLights.cpp",
        "Jazz2/Input/RumbleProcessor.cpp",
        "Jazz2/LevelHandler.cpp",
        "Jazz2/LevelInitialization.cpp",
        "Jazz2/LevelPacks/LevelPackLibrary.cpp",
        "Jazz2/LevelPacks/LevelPackManifest.cpp",
        "Jazz2/Multiplayer/ConnectionResult.cpp",
        "Jazz2/Multiplayer/GameModes/BattleMode.cpp",
        "Jazz2/Multiplayer/GameModes/CaptureTheFlagMode.cpp",
        "Jazz2/Multiplayer/GameModes/CooperationMode.cpp",
        "Jazz2/Multiplayer/GameModes/CustomGameModeBase.cpp",
        "Jazz2/Multiplayer/GameModes/DeathCaptureTheFlagMode.cpp",
        "Jazz2/Multiplayer/GameModes/DominationMode.cpp",
        "Jazz2/Multiplayer/GameModes/FlagRunMode.cpp",
        "Jazz2/Multiplayer/GameModes/GameModeFactory.cpp",
        "Jazz2/Multiplayer/GameModes/HeadHuntersMode.cpp",
        "Jazz2/Multiplayer/GameModes/JailbreakMode.cpp",
        "Jazz2/Multiplayer/GameModes/LastRabbitStandingMode.cpp",
        "Jazz2/Multiplayer/GameModes/PestilenceMode.cpp",
        "Jazz2/Multiplayer/GameModes/RaceMode.cpp",
        "Jazz2/Multiplayer/GameModes/RoastTagMode.cpp",
        "Jazz2/Multiplayer/GameModes/TeamBattleMode.cpp",
        "Jazz2/Multiplayer/GameModes/TeamLastRabbitStandingMode.cpp",
        "Jazz2/Multiplayer/GameModes/TreasureHuntMode.cpp",
        "Jazz2/Multiplayer/MpLevelHandler.cpp",
        "Jazz2/Multiplayer/NetworkManager.cpp",
        "Jazz2/Multiplayer/NetworkManagerBase.cpp",
        "Jazz2/Multiplayer/Peer.cpp",
        "Jazz2/Multiplayer/RaceRouteGenerator.cpp",
        "Jazz2/Multiplayer/ServerDiscovery.cpp",
        "Jazz2/Multiplayer/WebhookClient.cpp",
        "Jazz2/PreferencesCache.cpp",
        "Jazz2/Rendering/BlurRenderPass.cpp",
        "Jazz2/Rendering/CombineRenderer.cpp",
        "Jazz2/Rendering/LightingRenderer.cpp",
        "Jazz2/Rendering/PlayerViewport.cpp",
        "Jazz2/Rendering/PolylineRenderer.cpp",
        "Jazz2/Rendering/UpscaleRenderPass.cpp",
        "Jazz2/Rendering/WaterRenderer.cpp",
        "Jazz2/Rendering/XRayMaskRenderer.cpp",
        "Jazz2/Resources.cpp",
        "Jazz2/Scripting/Legacy/AnimationTables.cpp",
        "Jazz2/Scripting/Legacy/Animations.cpp",
        "Jazz2/Scripting/Legacy/Canvas.cpp",
        "Jazz2/Scripting/Legacy/Common.cpp",
        "Jazz2/Scripting/Legacy/Enums.cpp",
        "Jazz2/Scripting/Legacy/EventTranslator.cpp",
        "Jazz2/Scripting/Legacy/Game.cpp",
        "Jazz2/Scripting/Legacy/Layers.cpp",
        "Jazz2/Scripting/Legacy/Level.cpp",
        "Jazz2/Scripting/Legacy/ObjectHost.cpp",
        "Jazz2/Scripting/Legacy/Objects.cpp",
        "Jazz2/Scripting/Legacy/Palette.cpp",
        "Jazz2/Scripting/Legacy/PixelMaps.cpp",
        "Jazz2/Scripting/Legacy/Player.cpp",
        "Jazz2/Scripting/Legacy/Sound.cpp",
        "Jazz2/Scripting/Legacy/Stream.cpp",
        "Jazz2/Scripting/LevelScriptLoader.cpp",
        "Jazz2/Scripting/RegisterArray.cpp",
        "Jazz2/Scripting/RegisterDictionary.cpp",
        "Jazz2/Scripting/RegisterImGuiBindings.cpp",
        "Jazz2/Scripting/RegisterMath.cpp",
        "Jazz2/Scripting/RegisterRef.cpp",
        "Jazz2/Scripting/RegisterString.cpp",
        "Jazz2/Scripting/ScriptActorWrapper.cpp",
        "Jazz2/Scripting/ScriptLoader.cpp",
        "Jazz2/Scripting/ScriptPlayerWrapper.cpp",
        "Jazz2/Tests/PhysicsProbe.cpp",
        "Jazz2/Tiles/LayerTypes.cpp",
        "Jazz2/Tiles/TileMap.cpp",
        "Jazz2/Tiles/TileSet.cpp",
        "Jazz2/UI/Canvas.cpp",
        "Jazz2/UI/Cinematics.cpp",
        "Jazz2/UI/DiscordRpcClient.cpp",
        "Jazz2/UI/Font.cpp",
        "Jazz2/UI/FormattedTextBlock.cpp",
        "Jazz2/UI/HUD.cpp",
        "Jazz2/UI/InGameConsole.cpp",
        "Jazz2/UI/LoadingHandler.cpp",
        "Jazz2/UI/Menu/AboutSection.cpp",
        "Jazz2/UI/Menu/BeginSection.cpp",
        "Jazz2/UI/Menu/ControlsOptionsSection.cpp",
        "Jazz2/UI/Menu/CreateLocalGameOptionsSection.cpp",
        "Jazz2/UI/Menu/CreateServerOptionsSection.cpp",
        "Jazz2/UI/Menu/CustomLevelSelectSection.cpp",
        "Jazz2/UI/Menu/EpisodeSelectSection.cpp",
        "Jazz2/UI/Menu/FirstRunSection.cpp",
        "Jazz2/UI/Menu/GameplayEnhancementsSection.cpp",
        "Jazz2/UI/Menu/GameplayOptionsSection.cpp",
        "Jazz2/UI/Menu/GraphicsOptionsSection.cpp",
        "Jazz2/UI/Menu/HighscoresSection.cpp",
        "Jazz2/UI/Menu/IMenuContainer.cpp",
        "Jazz2/UI/Menu/InGameMenu.cpp",
        "Jazz2/UI/Menu/InputDiagnosticsSection.cpp",
        "Jazz2/UI/Menu/LanguageSelectSection.cpp",
        "Jazz2/UI/Menu/LevelPackDetailSection.cpp",
        "Jazz2/UI/Menu/LevelPacksSection.cpp",
        "Jazz2/UI/Menu/MainMenu.cpp",
        "Jazz2/UI/Menu/MenuContainerBase.cpp",
        "Jazz2/UI/Menu/MenuSection.cpp",
        "Jazz2/UI/Menu/MenuTransition.cpp",
        "Jazz2/UI/Menu/MenuWidgets.cpp",
        "Jazz2/UI/Menu/MultiplayerGameModeSelectSection.cpp",
        "Jazz2/UI/Menu/OptionsSection.cpp",
        "Jazz2/UI/Menu/PauseSection.cpp",
        "Jazz2/UI/Menu/PlayMultiplayerSection.cpp",
        "Jazz2/UI/Menu/RemapControlsSection.cpp",
        "Jazz2/UI/Menu/RescaleModeSection.cpp",
        "Jazz2/UI/Menu/SafeAreaOptionsSection.cpp",
        "Jazz2/UI/Menu/ScoreboardSection.cpp",
        "Jazz2/UI/Menu/ServerSelectSection.cpp",
        "Jazz2/UI/Menu/SimpleMessageSection.cpp",
        "Jazz2/UI/Menu/SoundsOptionsSection.cpp",
        "Jazz2/UI/Menu/StartGameOptionsSection.cpp",
        "Jazz2/UI/Menu/TextInputBuffer.cpp",
        "Jazz2/UI/Menu/TouchControlsOptionsSection.cpp",
        "Jazz2/UI/Menu/UserProfileOptionsSection.cpp",
        "Jazz2/UI/Menu/WidgetSection.cpp",
        "Jazz2/UI/Multiplayer/MpHUD.cpp",
        "Jazz2/UI/Multiplayer/MpInGameCanvasLayer.cpp",
        "Jazz2/UI/Multiplayer/MpInGameLobby.cpp",
        "Jazz2/UI/PerformanceOverlay.cpp",
        "Jazz2/UI/Windows/Colors.cpp",
        "Jazz2/UI/Windows/DetachedWindow.cpp",
        "Jazz2/UI/Windows/Dialogs.cpp",
        "Jazz2/UI/Windows/Dock.cpp",
        "Jazz2/UI/Windows/Element.cpp",
        "Jazz2/UI/Windows/Icons.cpp",
        "Jazz2/UI/Windows/Keyboard.cpp",
        "Jazz2/UI/Windows/Menu.cpp",
        "Jazz2/UI/Windows/Painter.cpp",
        "Jazz2/UI/Windows/RadialMenu.cpp",
        "Jazz2/UI/Windows/ReadableFont.cpp",
        "Jazz2/UI/Windows/Skin.cpp",
        "Jazz2/UI/Windows/Surface.cpp",
        "Jazz2/UI/Windows/Theme.cpp",
        "Jazz2/UI/Windows/VectorArt.cpp",
        "Jazz2/UI/Windows/VirtualList.cpp",
        "Jazz2/UI/Windows/Widgets.cpp",
        "Jazz2/Utils/AtomicFile.cpp",
        "Jazz2/Utils/JsonFile.cpp",
        "Jazz2/Utils/ZipArchive.cpp",
        "Jazz2/Utils/ZipFormat.cpp",
        "Jazz2/Utils/ZipWriter.cpp",
        "Jazz2/WeaponDefinition.cpp",
    };

    pub const shader_shared = [_][]const u8{
        "Shared/Containers/String.cpp",
        "Shared/Containers/StringView.cpp",
        "Shared/Containers/StringUtils.cpp",
        "Shared/Containers/SmallVector.cpp",
        "Shared/Containers/DateTime.cpp",
        "Shared/Base/Format.cpp",
        "Shared/Cpu.cpp",
        "Shared/Environment.cpp",
        "Shared/Utf8.cpp",
        "Shared/IO/FileSystem.cpp",
        "Shared/IO/FileStream.cpp",
        "Shared/IO/MemoryStream.cpp",
        "Shared/IO/Stream.cpp",
    };

    pub const shader_tool = [_][]const u8{
        "Utilities/ShaderCompiler/ConsoleFixedFunction.cpp",
        "Utilities/ShaderCompiler/ConstFold.cpp",
        "Utilities/ShaderCompiler/Emit.cpp",
        "Utilities/ShaderCompiler/Essl100.cpp",
        "Utilities/ShaderCompiler/GlslReflect.cpp",
        "Utilities/ShaderCompiler/GlslToCpp.cpp",
        "Utilities/ShaderCompiler/Hlsl.cpp",
        "Utilities/ShaderCompiler/Main.cpp",
        "Utilities/ShaderCompiler/Msl.cpp",
        "Utilities/ShaderCompiler/ShaderParser.cpp",
        "Utilities/ShaderCompiler/Trace.cpp",
        "Utilities/ShaderCompiler/Vulkan.cpp",
    };

    pub const asset_tool = [_][]const u8{
        "Utilities/AssetPacker/Iso9660.cpp",
        "Utilities/AssetPacker/TranslationFilter.cpp",
        "Utilities/AssetPacker/LevelText.cpp",
        "Utilities/AssetPacker/ModuleConverter.cpp",
        "Utilities/AssetPacker/FontPacker.cpp",
        "Utilities/AssetPacker/Main.cpp",
        "Utilities/AssetPacker/FormatVerifier.cpp",
        "Utilities/AssetPacker/DiscImage.cpp",
        "Utilities/AssetPacker/ContentFilter.cpp",
        "Utilities/AssetPacker/SpriteRepacker.cpp",
        "Utilities/AssetPacker/CartridgeImage.cpp",
    };
};
