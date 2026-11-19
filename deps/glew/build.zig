const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addIncludePath(b.path("include"));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const flags = [_][]const u8{ "-DGLEW_STATIC", "-DGLEW_NO_GLU" };
    mod.addCSourceFiles(.{ .files = &.{
        "src/glew.c",
    }, .flags = if (sysroot != null) &(flags ++ .{"-fno-sanitize=undefined"}) else &flags });
    if (target.result.os.tag == .windows) mod.linkSystemLibrary("opengl32", .{}) else if (target.result.os.tag == .linux) mod.linkSystemLibrary("GL", .{});
    const artifact = b.addLibrary(.{ .name = "GLEW", .root_module = mod });
    b.installArtifact(artifact);
}
