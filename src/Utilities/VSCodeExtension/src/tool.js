'use strict';

/**
 * Locating and running the offline ShaderCompiler executable.
 *
 * The tool is the extension's source of truth for diagnostics and for every transform preview: its
 * `--check`, `--hlsl`, `--cg`, `--vulkan`, `--msl` and `--essl100-check` modes print to stdout and write
 * nothing, so they are safe to run on an editor buffer. Diagnostics come from stderr in the three
 * shapes analysis.parseDiagnostics() knows.
 *
 * No `vscode` import here either - the caller passes plain paths and strings.
 */

var childProcess = require('child_process');
var fs = require('fs');
var os = require('os');
var path = require('path');

var IS_WINDOWS = (process.platform === 'win32');
var EXECUTABLE_NAME = (IS_WINDOWS ? 'ShaderCompiler.exe' : 'ShaderCompiler');

/** Infix of the temporary copies this extension writes, also used to sweep up leftovers */
var TEMP_INFIX = '.vscode-death-shader-tmp.';

function isExecutableFile(candidate) {
	try {
		var stat = fs.statSync(candidate);
		if (!stat.isFile()) {
			return false;
		}
		if (!IS_WINDOWS) {
			fs.accessSync(candidate, fs.constants.X_OK);
		}
		return true;
	} catch (e) {
		return false;
	}
}

/** The default Zig install directory for one workspace root. */
function candidatesForRoot(root) {
	return [path.join(root, 'zig-out', 'bin', EXECUTABLE_NAME)];
}

/** Looks @p name up in PATH (honoring PATHEXT on Windows). */
function findOnPath(name) {
	var pathValue = process.env.PATH || process.env.Path || '';
	var entries = pathValue.split(path.delimiter);
	var extensions = [''];
	if (IS_WINDOWS) {
		var pathExt = process.env.PATHEXT || '.EXE;.CMD;.BAT';
		extensions = pathExt.split(';');
		extensions.push('');
	}
	for (var i = 0; i < entries.length; i++) {
		if (entries[i].length === 0) {
			continue;
		}
		for (var j = 0; j < extensions.length; j++) {
			var candidate = path.join(entries[i], name + extensions[j]);
			if (isExecutableFile(candidate)) {
				return candidate;
			}
		}
	}
	return null;
}

/**
 * Resolves the executable to use.
 * @param {string} configuredPath the `deathShader.compilerPath` setting ('' when unset)
 * @param {Array<string>} workspaceRoots absolute workspace folder paths
 * @returns {{path: string, origin: string}|{path: null, origin: string}}
 */
function locate(configuredPath, workspaceRoots) {
	var i;
	if (typeof configuredPath === 'string' && configuredPath.length !== 0) {
		// A relative setting must resolve against the workspace, never against the extension host's
		// working directory - the runs below pass an unrelated cwd, so a CWD-relative hit would break
		var explicit = [];
		if (path.isAbsolute(configuredPath)) {
			explicit.push(configuredPath);
		} else {
			for (i = 0; i < workspaceRoots.length; i++) {
				explicit.push(path.join(workspaceRoots[i], configuredPath));
			}
			explicit.push(path.resolve(configuredPath));
		}
		for (i = 0; i < explicit.length; i++) {
			if (isExecutableFile(explicit[i])) {
				return { path: explicit[i], origin: 'the deathShader.compilerPath setting' };
			}
		}
		return { path: null, origin: "the deathShader.compilerPath setting ('" + configuredPath + "'), which does not name an executable file" };
	}

	for (i = 0; i < workspaceRoots.length; i++) {
		var candidates = candidatesForRoot(workspaceRoots[i]);
		for (var j = 0; j < candidates.length; j++) {
			if (isExecutableFile(candidates[j])) {
				return { path: candidates[j], origin: 'a build output in the workspace' };
			}
		}
	}

	var onPath = findOnPath(EXECUTABLE_NAME);
	if (onPath !== null) {
		return { path: onPath, origin: 'PATH' };
	}
	return { path: null, origin: 'nothing - no build output found and no ShaderCompiler on PATH' };
}

/**
 * Runs the executable and resolves with its exit code and captured output. Never rejects: a failure to
 * spawn is reported through `error`, so the caller has one code path.
 * @param {string} executable
 * @param {Array<string>} args
 * @param {{cwd?: string, timeoutMs?: number}} [options]
 * @returns {Promise<{code: number, stdout: string, stderr: string, error: (string|null)}>}
 */
function run(executable, args, options) {
	var settings = options || {};
	return new Promise(function (resolve) {
		var spawnOptions = {
			cwd: settings.cwd,
			timeout: (settings.timeoutMs === undefined ? 20000 : settings.timeoutMs),
			maxBuffer: 32 * 1024 * 1024,			// a --vulkan dump of a big shader is not small
			windowsHide: true
		};
		var child;
		try {
			child = childProcess.execFile(executable, args, spawnOptions,
				function (error, stdout, stderr) {
					var code = 0;
					var message = null;
					if (error !== null && error !== undefined) {
						if (typeof error.code === 'number') {
							code = error.code;
						} else if (error.killed === true) {
							code = -1;
							message = 'ShaderCompiler timed out after ' + spawnOptions.timeout + ' ms';
						} else {
							code = -1;
							message = String(error.message || error);
						}
					}
					resolve({
						code: code,
						stdout: String(stdout || ''),
						stderr: String(stderr || ''),
						error: message
					});
				});
		} catch (e) {
			resolve({ code: -1, stdout: '', stderr: '', error: String(e && e.message ? e.message : e) });
			return;
		}
		if (child === null || child === undefined) {
			resolve({ code: -1, stdout: '', stderr: '', error: 'could not start ShaderCompiler' });
		}
	});
}

/**
 * Writes @p text where the compiler can read it as a stand-in for a dirty buffer.
 *
 * A document with `#include` lines must be validated from a copy in its own directory, because the
 * compiler resolves include paths relative to the input file. Without includes the OS temp directory
 * is used instead, which keeps the source tree untouched.
 *
 * @param {string|null} originalPath the document's path, or null for an untitled buffer
 * @param {string} text
 * @param {boolean} needsSiblingDirectory
 * @returns {string} the temporary file's path (the caller must call removeTemp())
 */
function writeTempCopy(originalPath, text, needsSiblingDirectory) {
	var directory;
	var base;
	if (needsSiblingDirectory && originalPath !== null) {
		directory = path.dirname(originalPath);
		base = '.' + path.basename(originalPath, '.shader');
	} else {
		directory = fs.mkdtempSync(path.join(os.tmpdir(), 'death-shader-'));
		base = (originalPath !== null ? path.basename(originalPath, '.shader') : 'untitled');
	}
	var target = path.join(directory, base + TEMP_INFIX + process.pid + '.shader');
	fs.writeFileSync(target, text, { encoding: 'utf8' });
	return target;
}

/** Deletes a writeTempCopy() result (and its private directory when it made one). Never throws. */
function removeTemp(tempPath) {
	if (typeof tempPath !== 'string' || tempPath.length === 0) {
		return;
	}
	try {
		fs.unlinkSync(tempPath);
	} catch (e) {
		// already gone
	}
	var directory = path.dirname(tempPath);
	if (path.basename(directory).indexOf('death-shader-') === 0) {
		try {
			fs.rmdirSync(directory);
		} catch (e) {
			// not empty or already gone
		}
	}
}

/**
 * Removes temporary copies a previous session left behind (a crash between write and delete). Only
 * files carrying this extension's infix are ever touched.
 * @param {Array<string>} directories
 * @returns {number} how many were removed
 */
function sweepStaleTemps(directories) {
	var removed = 0;
	for (var i = 0; i < directories.length; i++) {
		var names;
		try {
			names = fs.readdirSync(directories[i]);
		} catch (e) {
			continue;
		}
		for (var j = 0; j < names.length; j++) {
			if (names[j].indexOf(TEMP_INFIX) < 0) {
				continue;
			}
			try {
				fs.unlinkSync(path.join(directories[i], names[j]));
				removed++;
			} catch (e) {
				// leave it
			}
		}
	}
	return removed;
}

module.exports = {
	EXECUTABLE_NAME: EXECUTABLE_NAME,
	TEMP_INFIX: TEMP_INFIX,
	isExecutableFile: isExecutableFile,
	candidatesForRoot: candidatesForRoot,
	findOnPath: findOnPath,
	locate: locate,
	run: run,
	writeTempCopy: writeTempCopy,
	removeTemp: removeTemp,
	sweepStaleTemps: sweepStaleTemps
};
