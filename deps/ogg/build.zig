const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addIncludePath(b.path("include"));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const flags = [_][]const u8{"-std=c99"};
    mod.addCSourceFiles(.{ .files = &.{
        "src/bitwise.c",
        "src/framing.c",
    }, .flags = if (sysroot != null) &(flags ++ .{"-fno-sanitize=undefined"}) else &flags });
    const artifact = b.addLibrary(.{ .name = "ogg", .root_module = mod });
    b.installArtifact(artifact);
}
