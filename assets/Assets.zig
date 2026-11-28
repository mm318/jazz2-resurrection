//! Host asset preparation, Brotli packaging, and installation.
const std = @import("std");
const builtin = @import("builtin");
const Allocator = std.mem.Allocator;
const Io = std.Io;
const Dir = Io.Dir;

pub fn main(init: std.process.Init) !void {
    const gpa = init.arena.allocator();
    const args = try init.minimal.args.toSlice(gpa);
    const tool = Tool{ .gpa = gpa, .io = init.io };
    if (args.len == 5 and std.mem.eql(u8, args[1], "prepare") and std.mem.eql(u8, args[2], "--asset-packer")) {
        try tool.prepare(args[4], args[3]);
    } else if (args.len == 7 and std.mem.eql(u8, args[1], "package-web")) {
        try tool.packageWeb(args[2], args[3], args[4], args[5], args[6]);
    } else if (args.len == 4 and std.mem.eql(u8, args[1], "install")) {
        try tool.install(args[2], args[3]);
    } else if (args.len >= 4 and std.mem.eql(u8, args[1], "run-in")) {
        const argv = try gpa.alloc([]const u8, args.len - 3);
        for (argv, args[3..]) |*out, arg| out.* = arg;
        try tool.runIn(args[2], argv);
    } else {
        std.debug.print("Usage: AssetTools prepare --asset-packer <executable> <directory>\n" ++
            "       AssetTools package-web <source> <target> <emsdk> <web-assets> <decoder>\n" ++
            "       AssetTools install <source> <target>\n" ++
            "       AssetTools run-in <directory> <command> [arguments...]\n", .{});
        return error.InvalidArguments;
    }
}

const Tool = struct {
    gpa: Allocator,
    io: Io,

    fn join(tool: Tool, parts: []const []const u8) ![]const u8 {
        return std.fs.path.join(tool.gpa, parts);
    }

    fn run(tool: Tool, argv: []const []const u8) !void {
        var child = try std.process.spawn(tool.io, .{ .argv = argv, .stdin = .ignore });
        defer child.kill(tool.io);
        if (!(try child.wait(tool.io)).success()) {
            std.log.err("{s} failed", .{argv[0]});
            return error.ChildProcessFailed;
        }
    }

    /// Emscripten names the preloaded data package after the path given to
    /// `-o` and writes that name into the generated JavaScript. Linking from
    /// inside the output directory with `-o index.html` keeps build cache
    /// paths such as `.zig-cache/o/<hash>/` out of the published files.
    fn runIn(tool: Tool, directory: []const u8, argv: []const []const u8) !void {
        var child = try std.process.spawn(tool.io, .{ .argv = argv, .cwd = .{ .path = directory }, .stdin = .ignore });
        defer child.kill(tool.io);
        if (!(try child.wait(tool.io)).success()) {
            std.log.err("{s} failed", .{argv[0]});
            return error.ChildProcessFailed;
        }
    }

    fn files(tool: Tool, directory: []const u8, extension: []const u8) ![]const []const u8 {
        var dir = Dir.cwd().openDir(tool.io, directory, .{ .iterate = true }) catch |err| switch (err) {
            error.FileNotFound => return &.{},
            else => return err,
        };
        defer dir.close(tool.io);
        var walker = try dir.walk(tool.gpa);
        defer walker.deinit();
        var result: std.ArrayList([]const u8) = .empty;
        while (try walker.next(tool.io)) |entry| {
            if (entry.kind == .file and std.mem.endsWith(u8, entry.basename, extension)) {
                try result.append(tool.gpa, try tool.join(&.{ directory, entry.path }));
            }
        }
        return result.toOwnedSlice(tool.gpa);
    }

    const SoundJob = struct {
        child: std.process.Child,
        source: []const u8,
        target: []const u8,
        temporary: []const u8,
    };

    fn finishSounds(tool: Tool, jobs: []SoundJob) !void {
        var failed = false;
        for (jobs) |*job| {
            if (!(try job.child.wait(tool.io)).success()) {
                std.log.err("Cannot encode {s}", .{job.source});
                failed = true;
                continue;
            }
            try Dir.cwd().rename(job.temporary, .cwd(), job.target, tool.io);
            try Dir.cwd().deleteFile(tool.io, job.source);
        }
        if (failed) return error.SoundEncodingFailed;
    }

    /// Explicit in-place preparation; never repeatedly encode lossy files.
    fn prepare(tool: Tool, directory: []const u8, asset_packer: []const u8) !void {
        var root = try Dir.cwd().openDir(tool.io, directory, .{});
        root.close(tool.io);
        const sounds = try tool.files(try tool.join(&.{ directory, "Sounds" }), ".wav");
        var jobs: std.ArrayList(SoundJob) = .empty;
        defer {
            for (jobs.items) |*job| {
                job.child.kill(tool.io);
                Dir.cwd().deleteFile(tool.io, job.temporary) catch {};
            }
            jobs.deinit(tool.gpa);
        }
        try jobs.ensureTotalCapacity(tool.gpa, 4);
        for (sounds) |source| {
            const wav = try Dir.cwd().readFileAlloc(tool.io, source, tool.gpa, .limited(64 * 1024 * 1024));
            defer tool.gpa.free(wav);
            const original_rate = try waveRate(wav);
            const rate = std.math.clamp(original_rate, 8000, 22050);
            const target = try std.fmt.allocPrint(tool.gpa, "{s}.ogg", .{source[0 .. source.len - 4]});
            const temporary = try std.fmt.allocPrint(tool.gpa, "{s}.tmp", .{target});
            const rate_arg = try std.fmt.allocPrint(tool.gpa, "{d}", .{rate});
            const metadata = try std.fmt.allocPrint(tool.gpa, "ORIGINAL_SAMPLE_RATE={d}", .{original_rate});
            const child = std.process.spawn(tool.io, .{ .argv = &.{
                "ffmpeg", "-nostdin", "-hide_banner",  "-loglevel", "error",    "-y",
                "-i",     source,     "-map_metadata", "-1",        "-ac",      "1",
                "-ar",    rate_arg,   "-metadata",     metadata,    "-c:a",     "libvorbis",
                "-b:a",   "32k",      "-fflags",       "+bitexact", "-flags:a", "+bitexact",
                "-f",     "ogg",      temporary,
            }, .stdin = .ignore }) catch |err| {
                std.log.err("Preparing sounds requires ffmpeg with its libvorbis encoder", .{});
                return err;
            };
            jobs.appendAssumeCapacity(.{ .child = child, .source = source, .target = target, .temporary = temporary });
            if (jobs.items.len == 4) {
                try tool.finishSounds(jobs.items);
                jobs.clearRetainingCapacity();
            }
        }
        try tool.finishSounds(jobs.items);
        jobs.clearRetainingCapacity();

        var videos: usize = 0;
        for (try tool.files(try tool.join(&.{ directory, "Cinematics" }), ".j2v")) |source| {
            var buffer: [25]u8 = undefined;
            const header = try Dir.cwd().readFile(tool.io, source, &buffer);
            if (try preparedVideo(header)) continue;
            const temporary = try std.fmt.allocPrint(tool.gpa, "{s}.tmp", .{source});
            defer Dir.cwd().deleteFile(tool.io, temporary) catch {};
            try tool.run(&.{ asset_packer, "recompress-video", source, temporary, "--video-downscale=2" });
            try Dir.cwd().rename(temporary, .cwd(), source, tool.io);
            videos += 1;
        }
        if (!builtin.is_test) {
            const summary = try std.fmt.allocPrint(tool.gpa, "Prepared {d} Vorbis effects and {d} half-resolution cinematics\n", .{ sounds.len, videos });
            try Io.File.stdout().writeStreamingAll(tool.io, summary);
        }
    }

    fn packageWeb(tool: Tool, source_path: []const u8, target_path: []const u8, sdk: []const u8, web_assets: []const u8, decoder: []const u8) !void {
        var source = try Dir.cwd().openDir(tool.io, source_path, .{ .iterate = true });
        defer source.close(tool.io);
        var target = try Dir.cwd().createDirPathOpen(tool.io, target_path, .{ .open_options = .{ .iterate = true } });
        defer target.close(tool.io);
        try tool.copyTree(source, target, false, true);
        var extra = try Dir.cwd().openDir(tool.io, web_assets, .{});
        defer extra.close(tool.io);
        for ([_][]const u8{ "coi-serviceworker.js", "asset-loader.js", "asset-worker.js" }) |name| {
            _ = try extra.updateFile(tool.io, name, target, name, .{});
        }
        _ = try Dir.cwd().updateFile(tool.io, decoder, target, "brotli.wasm", .{});
        _ = try Dir.cwd().updateFile(tool.io, "deps/brotli/LICENSE", target, "brotli.LICENSE", .{});
        const config = try Dir.cwd().readFileAlloc(tool.io, try tool.join(&.{ sdk, ".emscripten" }), tool.gpa, .limited(1024 * 1024));
        const node = try nodeCommand(tool.gpa, config, sdk);
        var argv: std.ArrayList([]const u8) = .empty;
        try argv.appendSlice(tool.gpa, node);
        try argv.appendSlice(tool.gpa, &.{ "-e", @embedFile("CompressWeb.cjs"), try tool.join(&.{ source_path, "index.data" }), try tool.join(&.{ target_path, "index.data.br" }) });
        try tool.run(argv.items);
    }

    fn install(tool: Tool, source_path: []const u8, target_path: []const u8) !void {
        const source_real = try Dir.cwd().realPathFileAlloc(tool.io, source_path, tool.gpa);
        try Dir.cwd().createDirPath(tool.io, target_path);
        const target_real = try Dir.cwd().realPathFileAlloc(tool.io, target_path, tool.gpa);
        if (inside(source_real, target_real) or inside(target_real, source_real)) return error.OverlappingDirectories;
        var source = try Dir.cwd().openDir(tool.io, source_real, .{ .iterate = true });
        defer source.close(tool.io);
        var target = try Dir.cwd().openDir(tool.io, target_real, .{ .iterate = true });
        defer target.close(tool.io);
        try tool.copyTree(source, target, true, false);
    }

    /// Mirrors build-owned output and atomically updates files, preserving
    /// timestamps and permissions so incremental installs skip unchanged data.
    fn copyTree(tool: Tool, source: Dir, target: Dir, prune: bool, skip_data: bool) anyerror!void {
        if (prune) {
            var entries = target.iterate();
            while (try entries.next(tool.io)) |entry| {
                const original = source.statFile(tool.io, entry.name, .{ .follow_symlinks = false }) catch |err| switch (err) {
                    error.FileNotFound => null,
                    else => return err,
                };
                if (original == null or original.?.kind != entry.kind) try tool.remove(target, entry.name, entry.kind);
            }
        }
        var entries = source.iterate();
        while (try entries.next(tool.io)) |entry| {
            if (skip_data and std.mem.eql(u8, entry.name, "index.data")) continue;
            switch (entry.kind) {
                .directory => {
                    var from = try source.openDir(tool.io, entry.name, .{ .iterate = true });
                    defer from.close(tool.io);
                    var to = try target.createDirPathOpen(tool.io, entry.name, .{ .open_options = .{ .iterate = true } });
                    defer to.close(tool.io);
                    try tool.copyTree(from, to, prune, skip_data);
                },
                .file => {
                    if (builtin.os.tag == .windows) {
                        const old = target.statFile(tool.io, entry.name, .{}) catch |err| switch (err) {
                            error.FileNotFound => null,
                            else => return err,
                        };
                        if (old) |previous| {
                            const current = try source.statFile(tool.io, entry.name, .{});
                            if (previous.size == current.size and previous.mtime.nanoseconds == current.mtime.nanoseconds and
                                previous.permissions == current.permissions) continue;
                            try target.setFilePermissions(tool.io, entry.name, previous.permissions.setReadOnly(false), .{});
                        }
                    }
                    _ = try source.updateFile(tool.io, entry.name, target, entry.name, .{});
                },
                else => return error.UnsupportedFileKind,
            }
        }
    }

    fn remove(tool: Tool, parent: Dir, name: []const u8, kind: Io.File.Kind) anyerror!void {
        if (builtin.os.tag == .windows and kind != .sym_link) {
            const stat = try parent.statFile(tool.io, name, .{});
            try parent.setFilePermissions(tool.io, name, stat.permissions.setReadOnly(false), .{});
        }
        if (kind == .directory) {
            {
                var dir = try parent.openDir(tool.io, name, .{ .iterate = true });
                defer dir.close(tool.io);
                var entries = dir.iterate();
                while (try entries.next(tool.io)) |entry| try tool.remove(dir, entry.name, entry.kind);
            }
            try parent.deleteDir(tool.io, name);
        } else {
            try parent.deleteFile(tool.io, name);
        }
    }
};

fn inside(parent: []const u8, child: []const u8) bool {
    var path: ?[]const u8 = child;
    while (path) |current| : (path = std.fs.path.dirname(current)) {
        if (if (builtin.os.tag == .windows) std.ascii.eqlIgnoreCase(parent, current) else std.mem.eql(u8, parent, current)) return true;
    }
    return false;
}

fn waveRate(bytes: []const u8) !u32 {
    if (bytes.len < 12 or !std.mem.eql(u8, bytes[0..4], "RIFF") or !std.mem.eql(u8, bytes[8..12], "WAVE")) return error.InvalidWave;
    var offset: usize = 12;
    while (bytes.len - offset >= 8) {
        const size = std.mem.readInt(u32, bytes[offset + 4 ..][0..4], .little);
        const start = offset + 8;
        if (size > bytes.len - start) return error.InvalidWave;
        if (std.mem.eql(u8, bytes[offset..][0..4], "fmt ")) {
            if (size < 16) return error.InvalidWave;
            const rate = std.mem.readInt(u32, bytes[start + 4 ..][0..4], .little);
            if (rate == 0) return error.InvalidWave;
            return rate;
        }
        offset = start + size + (size % 2);
        if (offset > bytes.len) return error.InvalidWave;
    }
    return error.InvalidWave;
}

fn preparedVideo(header: []const u8) !bool {
    if (header.len < 8 or std.mem.readInt(u64, header[0..8], .little) != 0x2095A59FF0BFBBEF) return false;
    if (header.len < 25) return error.InvalidVideo;
    const width = std.mem.readInt(u16, header[11..13], .little);
    const height = std.mem.readInt(u16, header[13..15], .little);
    if (width <= 320 and height <= 240) return true;
    std.log.err("Restore the original cinematic before preparing a native video at another resolution", .{});
    return error.RestoreOriginalCinematic;
}

/// The activated SDK stores NODE_JS as a quoted path or a list of quoted
/// arguments. Read these literals without executing the Python configuration.
fn nodeCommand(gpa: Allocator, config: []const u8, sdk: []const u8) ![]const []const u8 {
    var lines = std.mem.splitScalar(u8, config, '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trimStart(u8, line, " \t\r");
        if (!std.mem.startsWith(u8, trimmed, "NODE_JS")) continue;
        var cursor = std.mem.trimStart(u8, trimmed[7..], " \t");
        if (cursor.len == 0 or cursor[0] != '=') continue;
        // Include subsequent lines for SDK configurations using a multiline list.
        cursor = std.mem.trimStart(u8, config[@intFromPtr(cursor.ptr) - @intFromPtr(config.ptr) + 1 ..], " \t\r\n");
        var result: std.ArrayList([]const u8) = .empty;
        const list = cursor.len > 0 and cursor[0] == '[';
        if (list) cursor = cursor[1..];
        while (true) {
            cursor = std.mem.trimStart(u8, cursor, " \t\r\n");
            if (list and cursor.len > 0 and cursor[0] == ']') {
                if (result.items.len == 0) return error.InvalidNodeConfig;
                return result.toOwnedSlice(gpa);
            }
            const value = try quotedString(gpa, &cursor);
            try result.append(gpa, try std.mem.replaceOwned(u8, gpa, value, "$CFGDIR", sdk));
            if (!list) return result.toOwnedSlice(gpa);
            cursor = std.mem.trimStart(u8, cursor, " \t\r\n");
            if (cursor.len == 0) return error.InvalidNodeConfig;
            if (cursor[0] == ',') {
                cursor = cursor[1..];
            } else if (cursor[0] != ']') return error.InvalidNodeConfig;
        }
    }
    return error.NodeConfigMissing;
}

fn quotedString(gpa: Allocator, cursor: *[]const u8) ![]const u8 {
    var text = cursor.*;
    if (text.len == 0 or (text[0] != '\'' and text[0] != '"')) return error.InvalidNodeConfig;
    const quote = text[0];
    text = text[1..];
    var result: std.ArrayList(u8) = .empty;
    while (text.len > 0) {
        const byte = text[0];
        text = text[1..];
        if (byte == quote) {
            cursor.* = text;
            return result.toOwnedSlice(gpa);
        }
        if (byte == '\\') {
            if (text.len == 0) return error.InvalidNodeConfig;
            const escaped = text[0];
            text = text[1..];
            try result.append(gpa, switch (escaped) {
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                '\\', '\'', '"' => escaped,
                else => return error.InvalidNodeConfig,
            });
        } else try result.append(gpa, byte);
    }
    return error.InvalidNodeConfig;
}

test "install upgrades read-only files, prunes obsolete payloads, and skips unchanged files" {
    const io = std.testing.io;
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    const tool = Tool{ .gpa = arena.allocator(), .io = io };
    var tmp = std.testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try tmp.dir.createDirPath(io, "source/switch");
    try tmp.dir.createDirPath(io, "target/obsolete");
    try tmp.dir.writeFile(io, .{ .sub_path = "source/index.data.br", .data = "new compressed payload" });
    try tmp.dir.writeFile(io, .{ .sub_path = "source/index.js", .data = "new loader" });
    try tmp.dir.writeFile(io, .{ .sub_path = "source/switch/effect.ogg", .data = "prepared sound" });
    try tmp.dir.writeFile(io, .{ .sub_path = "target/index.data", .data = "obsolete uncompressed payload" });
    try tmp.dir.writeFile(io, .{ .sub_path = "target/index.js", .data = "old loader" });
    try tmp.dir.setFilePermissions(io, "target/index.js", Io.File.Permissions.default_file.setReadOnly(true), .{});
    try tmp.dir.writeFile(io, .{ .sub_path = "target/obsolete/effect.wav", .data = "old sound" });
    try tmp.dir.writeFile(io, .{ .sub_path = "target/switch", .data = "old file replaced by a directory" });
    const source = try tmp.dir.realPathFileAlloc(io, "source", tool.gpa);
    const target = try tmp.dir.realPathFileAlloc(io, "target", tool.gpa);
    try tool.install(source, target);
    try std.testing.expectError(error.FileNotFound, tmp.dir.statFile(io, "target/index.data", .{}));
    try std.testing.expectError(error.FileNotFound, tmp.dir.statFile(io, "target/obsolete", .{}));
    const loader = try tmp.dir.readFileAlloc(io, "target/index.js", tool.gpa, .limited(100));
    try std.testing.expectEqualStrings("new loader", loader);
    const sound = try tmp.dir.readFileAlloc(io, "target/switch/effect.ogg", tool.gpa, .limited(100));
    try std.testing.expectEqualStrings("prepared sound", sound);
    const before = try tmp.dir.statFile(io, "target/index.data.br", .{});
    try tool.install(source, target);
    const after = try tmp.dir.statFile(io, "target/index.data.br", .{});
    try std.testing.expectEqual(before.mtime.nanoseconds, after.mtime.nanoseconds);
    try std.testing.expectEqual(before.inode, after.inode);
    try std.testing.expectError(error.OverlappingDirectories, tool.install(source, source));
}

test "preparing lossy assets again leaves them untouched without external tools" {
    const io = std.testing.io;
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    const tool = Tool{ .gpa = arena.allocator(), .io = io };
    var tmp = std.testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try tmp.dir.createDirPath(io, "Sounds");
    try tmp.dir.createDirPath(io, "Cinematics");
    try tmp.dir.writeFile(io, .{ .sub_path = "Sounds/effect.ogg", .data = "already encoded audio" });
    var header: [25]u8 = @splat(0);
    std.mem.writeInt(u64, header[0..8], 0x2095A59FF0BFBBEF, .little);
    header[8] = 8;
    std.mem.writeInt(u16, header[9..11], 1, .little);
    std.mem.writeInt(u16, header[11..13], 320, .little);
    std.mem.writeInt(u16, header[13..15], 240, .little);
    try tmp.dir.writeFile(io, .{ .sub_path = "Cinematics/intro.j2v", .data = &header });
    const path = try tmp.dir.realPathFileAlloc(io, ".", tool.gpa);
    try tool.prepare(path, "nonexistent-packer");
    const video = try tmp.dir.readFileAlloc(io, "Cinematics/intro.j2v", tool.gpa, .limited(100));
    try std.testing.expectEqualSlices(u8, &header, video);
    const sound = try tmp.dir.readFileAlloc(io, "Sounds/effect.ogg", tool.gpa, .limited(100));
    try std.testing.expectEqualStrings("already encoded audio", sound);
}

test "WAV sample rate handles additional chunks and rejects truncated data" {
    var wav: [54]u8 = @splat(0);
    @memcpy(wav[0..4], "RIFF");
    @memcpy(wav[8..12], "WAVE");
    @memcpy(wav[12..16], "JUNK");
    std.mem.writeInt(u32, wav[16..20], 1, .little);
    @memcpy(wav[22..26], "fmt ");
    std.mem.writeInt(u32, wav[26..30], 16, .little);
    std.mem.writeInt(u32, wav[34..38], 44100, .little);
    try std.testing.expectEqual(@as(u32, 44100), try waveRate(&wav));
    try std.testing.expectError(error.InvalidWave, waveRate(wav[0..37]));
}

test "SDK Node path and optional arguments are read without evaluating configuration" {
    var arena: std.heap.ArenaAllocator = .init(std.testing.allocator);
    defer arena.deinit();
    const gpa = arena.allocator();
    const single = try nodeCommand(gpa, "NODE_JS = '$CFGDIR/node/bin/node'\n", "/sdk path");
    try std.testing.expectEqual(@as(usize, 1), single.len);
    try std.testing.expectEqualStrings("/sdk path/node/bin/node", single[0]);
    const list = try nodeCommand(gpa, "# config\nNODE_JS = [\n'C:\\\\sdk path\\\\node.exe', '--flag',\n]\n", "/unused");
    try std.testing.expectEqual(@as(usize, 2), list.len);
    try std.testing.expectEqualStrings("C:\\sdk path\\node.exe", list[0]);
    try std.testing.expectEqualStrings("--flag", list[1]);
    try std.testing.expectError(error.InvalidNodeConfig, nodeCommand(gpa, "NODE_JS = run_something()", "/sdk"));
}
