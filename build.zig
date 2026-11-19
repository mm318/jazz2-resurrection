const std = @import("std");
const Build = std.Build;
const builtin = @import("builtin");
const sources = @import("build/Sources.zig");

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
    b.step("test", "Test the shared library without game data").dependOn(&b.addRunArtifact(tests).step);

    const asset_tool = assetPacker(b);
    const install_asset = b.addInstallArtifact(asset_tool, .{});
    b.step("asset-packer", "Build AssetPacker for the build host").dependOn(&install_asset.step);

    const tools = b.step("tools", "Build the host asset and shader tools");
    tools.dependOn(&install_shader.step);
    tools.dependOn(&install_asset.step);

    const pack = b.addRunArtifact(asset_tool);
    pack.addPassthruArgs();
    b.step("pack-assets", "Run AssetPacker with explicitly supplied game data").dependOn(&pack.step);

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
        const asset_inputs = b.addWriteFiles().addCopyDirectory(assets, "assets", .{});
        link.addDirectoryArg2(asset_inputs, .{ .suffix = "@/assets" });
        link.addArtifactArg(lib);

        for (lib.getCompileDependencies(false)) |dependency| {
            if (dependency != lib and dependency.kind == .lib) link.addArtifactArg(dependency);
        }

        link.addArg("-o");
        const output = link.addOutputFileArg("index.html");

        const install = b.addInstallDirectory(.{
            .source_dir = output.dirname(),
            .install_dir = .prefix,
            .install_subdir = "web",
        });
        b.getInstallStep().dependOn(&install.step);
        b.installFile("web_assets/coi-serviceworker.js", "web/coi-serviceworker.js");

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
        b.installDirectory(.{ .source_dir = assets, .install_dir = .bin, .install_subdir = "assets" });

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
