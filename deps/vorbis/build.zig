const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addIncludePath(b.path("include"));
    mod.addIncludePath(b.path("lib"));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const ogg = b.dependency("ogg", .{ .target = target, .optimize = optimize, .emscripten_include = sysroot });
    mod.linkLibrary(ogg.artifact("ogg"));
    mod.addIncludePath(ogg.path("include"));
    const flags = [_][]const u8{ "-std=c99", "-Dalloca=__builtin_alloca" };
    mod.addCSourceFiles(.{ .files = &.{
        "lib/mdct.c",
        "lib/smallft.c",
        "lib/block.c",
        "lib/envelope.c",
        "lib/window.c",
        "lib/lsp.c",
        "lib/lpc.c",
        "lib/analysis.c",
        "lib/synthesis.c",
        "lib/psy.c",
        "lib/info.c",
        "lib/floor1.c",
        "lib/floor0.c",
        "lib/res0.c",
        "lib/mapping0.c",
        "lib/registry.c",
        "lib/codebook.c",
        "lib/sharedbook.c",
        "lib/lookup.c",
        "lib/bitrate.c",
        "lib/vorbisenc.c",
        "lib/vorbisfile.c",
    }, .flags = if (sysroot != null) &(flags ++ .{"-fno-sanitize=undefined"}) else &flags });
    const artifact = b.addLibrary(.{ .name = "vorbis", .root_module = mod });
    b.installArtifact(artifact);
}
