const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true, .single_threaded = false });
    mod.addIncludePath(b.path("include"));
    mod.addIncludePath(b.path("source"));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const flags = [_][]const u8{ "-std=c++17", "-DAS_NO_EXCEPTIONS", "-fno-exceptions", "-Wno-deprecated-declarations" };
    mod.addCSourceFiles(.{ .files = &.{
        "source/as_atomic.cpp",
        "source/as_builder.cpp",
        "source/as_bytecode.cpp",
        "source/as_callfunc.cpp",
        "source/as_callfunc_e2k.cpp",
        "source/as_callfunc_mips.cpp",
        "source/as_callfunc_ppc.cpp",
        "source/as_callfunc_ppc_64.cpp",
        "source/as_callfunc_sh4.cpp",
        "source/as_callfunc_x64_gcc.cpp",
        "source/as_callfunc_x64_mingw.cpp",
        "source/as_callfunc_x64_msvc.cpp",
        "source/as_callfunc_x86.cpp",
        "source/as_callfunc_xenon.cpp",
        "source/as_compiler.cpp",
        "source/as_configgroup.cpp",
        "source/as_context.cpp",
        "source/as_datatype.cpp",
        "source/as_gc.cpp",
        "source/as_generic.cpp",
        "source/as_globalproperty.cpp",
        "source/as_memory.cpp",
        "source/as_module.cpp",
        "source/as_objecttype.cpp",
        "source/as_outputbuffer.cpp",
        "source/as_parser.cpp",
        "source/as_restore.cpp",
        "source/as_scriptcode.cpp",
        "source/as_scriptengine.cpp",
        "source/as_scriptfunction.cpp",
        "source/as_scriptnode.cpp",
        "source/as_scriptobject.cpp",
        "source/as_string.cpp",
        "source/as_string_util.cpp",
        "source/as_thread.cpp",
        "source/as_tokenizer.cpp",
        "source/as_typeinfo.cpp",
        "source/as_variablescope.cpp",
    }, .flags = if (sysroot != null) &(flags ++ .{ "-fno-sanitize=undefined", "-pthread" }) else &flags });
    if (target.result.cpu.arch == .aarch64) {
        mod.addCSourceFile(.{ .file = b.path("source/as_callfunc_arm64.cpp"), .flags = &flags });
        mod.addAssemblyFile(b.path(if (target.result.os.tag == .macos) "source/as_callfunc_arm64_xcode.S" else "source/as_callfunc_arm64_gcc.S"));
    } else if (target.result.os.tag == .windows and target.result.cpu.arch == .x86_64) {
        mod.addAssemblyFile(b.path("source/as_callfunc_x64_msvc_clang.S"));
    }
    const artifact = b.addLibrary(.{ .name = "angelscript", .root_module = mod });
    b.installArtifact(artifact);
}
