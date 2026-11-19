const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true, .single_threaded = false });
    mod.addIncludePath(b.path("."));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const flags = [_][]const u8{ "-std=c++20", "-DJSON_USE_EXCEPTION=0", "-fno-exceptions" };
    mod.addCSourceFiles(.{ .files = &.{
        "reader.cpp",
        "value.cpp",
        "writer.cpp",
    }, .flags = if (sysroot != null) &(flags ++ .{ "-fno-sanitize=undefined", "-pthread" }) else &flags });
    const artifact = b.addLibrary(.{ .name = "jsoncpp", .root_module = mod });
    b.installArtifact(artifact);
}
