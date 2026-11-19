const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addIncludePath(b.path("."));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const flags = [_][]const u8{ "-std=c99", "-D_LARGEFILE64_SOURCE=1", "-DHAVE_UNISTD_H" };
    mod.addCSourceFiles(.{ .files = &.{
        "adler32.c",
        "compress.c",
        "crc32.c",
        "deflate.c",
        "gzclose.c",
        "gzlib.c",
        "gzread.c",
        "gzwrite.c",
        "inflate.c",
        "infback.c",
        "inftrees.c",
        "inffast.c",
        "trees.c",
        "uncompr.c",
        "zutil.c",
    }, .flags = if (sysroot != null) &(flags ++ .{"-fno-sanitize=undefined"}) else &flags });
    const artifact = b.addLibrary(.{ .name = "z", .root_module = mod });
    b.installArtifact(artifact);
}
