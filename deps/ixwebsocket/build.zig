const std = @import("std");
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sysroot = b.option(std.Build.LazyPath, "emscripten_include", "Emscripten sysroot headers");
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true });
    mod.addIncludePath(b.path("."));
    mod.addIncludePath(b.path(".."));
    if (sysroot) |include| mod.addSystemIncludePath(include);
    const zlib = b.dependency("zlib", .{ .target = target, .optimize = optimize, .emscripten_include = sysroot });
    mod.linkLibrary(zlib.artifact("z"));
    mod.addIncludePath(zlib.path("."));
    const flags = [_][]const u8{ "-std=c++20", "-Wno-deprecated-declarations" };
    mod.addCSourceFiles(.{ .files = &.{
        "IXCancellationRequest.cpp",
        "IXConnectionState.cpp",
        "IXDNSLookup.cpp",
        "IXExponentialBackoff.cpp",
        "IXGetFreePort.cpp",
        "IXGzipCodec.cpp",
        "IXHttp.cpp",
        "IXHttpClient.cpp",
        "IXHttpServer.cpp",
        "IXLogger.cpp",
        "IXNetSystem.cpp",
        "IXSelectInterrupt.cpp",
        "IXSelectInterruptEvent.cpp",
        "IXSelectInterruptFactory.cpp",
        "IXSelectInterruptPipe.cpp",
        "IXSetThreadName.cpp",
        "IXSocket.cpp",
        "IXSocketAppleSSL.cpp",
        "IXSocketConnect.cpp",
        "IXSocketFactory.cpp",
        "IXSocketMbedTLS.cpp",
        "IXSocketOpenSSL.cpp",
        "IXSocketSChannel.cpp",
        "IXSocketServer.cpp",
        "IXSocketTLSOptions.cpp",
        "IXStrCaseCompare.cpp",
        "IXUdpSocket.cpp",
        "IXUrlParser.cpp",
        "IXUserAgent.cpp",
        "IXUuid.cpp",
        "IXWebSocket.cpp",
        "IXWebSocketCloseConstants.cpp",
        "IXWebSocketHandshake.cpp",
        "IXWebSocketHttpHeaders.cpp",
        "IXWebSocketPerMessageDeflate.cpp",
        "IXWebSocketPerMessageDeflateCodec.cpp",
        "IXWebSocketPerMessageDeflateOptions.cpp",
        "IXWebSocketProxyServer.cpp",
        "IXWebSocketServer.cpp",
        "IXWebSocketTransport.cpp",
    }, .flags = if (sysroot != null) &(flags ++ .{"-fno-sanitize=undefined"}) else &flags });
    mod.addCMacro("IXWEBSOCKET_USE_TLS", "1");
    mod.addCMacro("IXWEBSOCKET_USE_ZLIB", "1");
    switch (target.result.os.tag) {
        .linux => {
            mod.addCMacro("IXWEBSOCKET_USE_OPEN_SSL", "1");
            mod.linkSystemLibrary("ssl", .{});
            mod.linkSystemLibrary("crypto", .{});
        },
        .windows => {
            mod.addCMacro("IXWEBSOCKET_USE_SCHANNEL", "1");
            mod.linkSystemLibrary("crypt32", .{});
            mod.linkSystemLibrary("secur32", .{});
            mod.linkSystemLibrary("ncrypt", .{});
            mod.linkSystemLibrary("ws2_32", .{});
        },
        .macos => {
            mod.addCMacro("IXWEBSOCKET_USE_SECURE_TRANSPORT", "1");
            mod.linkFramework("Security", .{});
            mod.linkFramework("CoreFoundation", .{});
        },
        else => {},
    }
    const artifact = b.addLibrary(.{ .name = "ixwebsocket", .root_module = mod });
    b.installArtifact(artifact);
}
