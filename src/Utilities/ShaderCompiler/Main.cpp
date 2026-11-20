/**
	@file Main.cpp

	Command-line entry point of ShaderCompiler — the offline shader preprocessor of
	Jazz² Resurrection. Reads an annotated ".shader" file, expands its variants,
	reflects the GLSL declarations of every variant (uniforms, std140 blocks with
	computed offsets/strides, texture bindings, vertex attributes) and emits a
	self-contained C++ header with the sources and reflection data, so the runtime
	does not need glGetActiveUniform introspection or double compilation of
	batched shaders.

	Usage:
	    ShaderCompiler <input.shader> -o <output.h> [-n <namespace>] [--check]

	With --check the tool parses the input and prints a human-readable reflection
	dump to stdout instead of writing the output header. With --essl100-check (or
	--target essl100) it prints the ESSL 100 (OpenGL ES 2.0) transform of every
	variant's stage sources to stdout, for inspection — a tool-only
	surface that does NOT change the emitted headers. Errors are reported to
	stderr as "<file>:<line>: error: <message>" and the exit code is non-zero.
*/

#include "Emit.h"
#include "Essl100.h"
#include "Hlsl.h"
#include "Msl.h"
#include "Vulkan.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <Base/Format.h>
#include <Containers/GrowableArray.h>
#include <Containers/SmallVector.h>
#include <Containers/StringConcatenable.h>
#include <IO/FileSystem.h>
#include <IO/Stream.h>
#include <Utf8.h>

#if defined(DEATH_TARGET_WINDOWS)
#include <CommonWindows.h>
#include <d3dcompiler.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// posix_spawn needs the caller's environment passed explicitly, and it is not declared by any header
extern char** environ;
#endif

using namespace ShaderCompiler;
using namespace Death::Containers;
using namespace Death::Containers::Literals;
using namespace Death::IO;

namespace
{
	/** Reads the file at @p path in full; false when it cannot be opened or read to its end */
	bool ReadFileToString(StringView path, String& content)
	{
		std::unique_ptr<Stream> s = fs::Open(path, FileAccess::Read);
		if (s == nullptr || !s->IsValid()) {
			return false;
		}
		std::int64_t size = s->GetSize();
		if (size < 0) {
			return false;
		}
		content = String { NoInit, static_cast<std::size_t>(size) };
		return (size == 0 || s->Read(content.data(), size) == size);
	}

	/** Truncates the file at @p path to @p content, creating it when it does not exist yet */
	bool WriteStringToFile(StringView path, StringView content)
	{
		std::unique_ptr<Stream> s = fs::Open(path, FileAccess::Write);
		if (s == nullptr || !s->IsValid()) {
			return false;
		}
		std::int64_t size = static_cast<std::int64_t>(content.size());
		return (size == 0 || s->Write(content.data(), size) == size);
	}

	/**
		Collects the names (not paths) of the files in @p directory whose name ends with @p extension,
		sorted by a plain byte-wise comparison so the enumeration order — and therefore every aggregate
		artifact built from it — is identical on every machine, shell and locale.
	*/
	bool ListFilesInDirectory(StringView directory, StringView extension, SmallVectorImpl<String>& outNames)
	{
		if (!fs::DirectoryExists(directory)) {
			return false;
		}
		for (StringView path : fs::Directory(directory, fs::EnumerationOptions::SkipDirectories | fs::EnumerationOptions::SkipSpecial)) {
			StringView name = fs::GetFileName(path);
			if (name.hasSuffix(extension)) {
				outNames.emplace_back(name);
			}
		}
		std::sort(outNames.begin(), outNames.end(), [](const String& a, const String& b) {
			return std::strcmp(a.data(), b.data()) < 0;
		});
		return true;
	}

	/** Identifies the running process, so concurrent tool invocations cannot collide in the temp directory */
	std::uint32_t CurrentProcessId()
	{
#if defined(DEATH_TARGET_WINDOWS)
		return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
		return static_cast<std::uint32_t>(::getpid());
#endif
	}

	/** Creates a fresh, empty directory below the system temp location for the staleness guard */
	bool CreateTemporaryDirectory(String& outPath)
	{
		String temp = fs::GetTempDirectory();
		if (temp.empty()) {
			return false;
		}
		for (std::uint32_t attempt = 0; attempt < 64; attempt++) {
			String candidate = fs::CombinePath(temp, Death::format("Jazz2-ShaderCheck-{:.8X}", CurrentProcessId() + attempt * 7919u));
			if (fs::Exists(candidate)) {
				continue;
			}
			if (fs::CreateDirectories(candidate)) {
				outPath = Death::move(candidate);
				return true;
			}
		}
		return false;
	}

	/**
		Builds the shared path prefix of one scratch file set below the system temp directory, unique per
		process and per @p serial. Empty when the temp directory cannot be resolved.
	*/
	String MakeTemporaryPathPrefix(StringView tag, unsigned serial)
	{
		String temp = fs::GetTempDirectory();
		if (temp.empty()) {
			return {};
		}
		return fs::CombinePath(temp, Death::format("{}_{}_{}", tag, CurrentProcessId(), serial));
	}

	/**
		Reads the environment variable @p name as UTF-8; false when it is not set or is empty.

		Windows keeps the environment in UTF-16, so the wide entry point is the only lossless one:
		GetEnvironmentVariableA transcodes through the active code page and mangles (or outright
		rejects) any path that is not representable in it, which the SDK and Program Files paths
		this is used for very much can be.
	*/
	bool TryGetEnvironmentVariable(StringView name, String& outValue)
	{
#if defined(DEATH_TARGET_WINDOWS)
		Array<wchar_t> nameW = Death::Utf8::ToUtf16(name);
		SmallVector<wchar_t, MAX_PATH> valueW(DefaultInit, MAX_PATH);
		DWORD length = ::GetEnvironmentVariableW(nameW, valueW.data(), DWORD(valueW.size()));
		if (length >= valueW.size()) {
			// The buffer was too small, the call reported how much is actually needed (including the terminator)
			valueW.resize_for_overwrite(length);
			length = ::GetEnvironmentVariableW(nameW, valueW.data(), DWORD(valueW.size()));
		}
		if (length == 0 || length >= valueW.size()) {
			return false;
		}
		outValue = Death::Utf8::FromUtf16(valueW.data(), std::int32_t(length));
		return !outValue.empty();
#else
		const char* value = std::getenv(String::nullTerminatedView(name).data());
		if (value == nullptr || value[0] == '\0') {
			return false;
		}
		outValue = value;
		return true;
#endif
	}

	/** Byte-compares two files; false when either cannot be read or their contents differ */
	bool FilesHaveEqualContent(StringView a, StringView b)
	{
		String left, right;
		if (!ReadFileToString(a, left) || !ReadFileToString(b, right)) {
			return false;
		}
		return (left.size() == right.size() && std::memcmp(left.data(), right.data(), left.size()) == 0);
	}

	bool IsValidNamespace(StringView ns)
	{
		if (ns.empty() || (ns[0] >= '0' && ns[0] <= '9')) {
			return false;
		}
		for (std::size_t i = 0; i < ns.size(); i++) {
			char c = ns[i];
			bool valid = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':');
			if (!valid) {
				return false;
			}
		}
		return true;
	}

	void PrintUsage()
	{
		std::fprintf(stderr,
			"ShaderCompiler - offline shader variant expansion and reflection\n"
			"\n"
			"Usage: ShaderCompiler <input.shader> -o <output.h> [-n <namespace>] [--check]\n"
			"\n"
			"  -o <output.h>     Path of the generated C++ header\n"
			"  -n <namespace>    Namespace for the generated program data (default: ShaderArtifacts)\n"
			"  --check           Parse only and print a reflection dump to stdout (no output written)\n"
			"  --essl100-check   Print the ESSL 100 (OpenGL ES 2.0) transform of every stage to stdout\n"
			"  --hlsl            Print the HLSL (Shader Model 4/5) transform of every stage to stdout\n"
			"  --no-dxbc         Embed HLSL sources instead of precompiled DXBC bytecode (default: DXBC via\n"
			"                    d3dcompiler_47 when available, with the HLSL text left out of the header)\n"
			"  --vulkan          Print the Vulkan GLSL (#version 450) transform of every stage to stdout\n"
			"  --msl             Print the Metal Shading Language transform of every stage to stdout\n"
			"  --glslang <path>  glslangValidator to compile SPIR-V with (default: VULKAN_SDK / PATH)\n"
			"  --target <t>      Selects an inspection target for the transform dump (only: essl100)\n"
			"\n"
			"Standalone modes:\n"
			"  --generate-all [--gl-only] [--shaders-dir <dir>] [--out-dir <dir>] [--check] [--no-dxbc] [--glslang <path>]\n"
			"                                                    Generate shader artifacts (the shared types, one\n"
			"                                                    header per shader, the umbrella and the aggregates)\n"
			"                                                    from one enumeration of the shader directory; --check only\n"
			"                                                    compares and never writes into the tree. Both directories\n"
			"                                                    are auto-detected\n"
			"  --gl-only                                         Generate OpenGL/WebGL headers without platform shader SDKs\n"
			"  --hlsl-check <input.shader ...>                    Emit + D3DCompile each stage as HLSL; print a pass/fail table\n"
			"  --spirv-check [--glslang <path>] <input.shader ...> Emit + glslang-compile each stage to SPIR-V; print a pass/fail table\n");
	}

	int ReportError(const char* inputPath, const Diagnostic& diag)
	{
		std::fprintf(stderr, "%s:%d: error: %s\n", inputPath, diag.Line, diag.Message.data());
		return 1;
	}

	/** Runs strip-comments + preprocess + reflect for one stage of one variant */
	bool ReflectVariantStage(const ShaderDocument& document, bool vertexStage, StringView define,
		StageReflection& result, Diagnostic& diag)
	{
		SmallVector<SourceLine, 0> lines = document.Prelude;
		const SmallVectorImpl<SourceLine>& stage = (vertexStage ? document.VertexLines : document.FragmentLines);
		lines.insert(lines.end(), stage.begin(), stage.end());

		ShaderParser::StripComments(lines);

		Preprocessor preprocessor;
		preprocessor.DefineVariant(define);
		SmallVector<SourceLine, 0> preprocessed;
		if (!preprocessor.Run(lines, preprocessed, diag)) {
			return false;
		}
		return GlslReflector::ReflectStage(preprocessed, vertexStage, result, diag);
	}

	/**
		Builds the ESSL 100 (OpenGL ES 2.0) transform dump printed by --essl100-check: one section
		per program / variant / stage, with either the transformed source or an "unsupported"
		diagnostic. Inspection-only — never touches the emitted header.
	*/
	String BuildEssl100Dump(const SmallVectorImpl<ProgramReflection>& programs)
	{
		String dump;
		for (const ProgramReflection& program : programs) {
			dump += "program " + program.Document->ProgramName + "\n";
			for (const VariantReflection& v : program.Variants) {
				dump += (v.Name.empty() ? String("variant (base)\n") : String("variant " + v.Name + "\n"));
				for (std::int32_t stage = 0; stage < 2; stage++) {
					bool vertexStage = (stage == 0);
					dump += (vertexStage ? "--- vertex (essl100) ---\n" : "--- fragment (essl100) ---\n");
					// The ES2 profile is a low-power target, so the dump has to be built the way EmitProgram
					// builds the baked _Vs100/_Fs100 sources - from the LOW_POWER_GPU view of the stage
					String modern = ShaderParser::BuildStageSource(*program.Document, vertexStage, v.Define,
						/*softwareRenderer*/ false, /*noDynamicBranching*/ false, /*lowPowerGpu*/ true);
					String es2;
					Diagnostic diag;
					if (Essl100Emitter::Transform(modern, vertexStage, es2, diag)) {
						dump += es2;
						if (!es2.empty() && es2.back() != '\n') {
							dump += "\n";
						}
					} else {
						dump += "essl100: " + diag.Message + " (line " + Death::format("{}", diag.Line) + ")\n";
					}
				}
			}
		}
		return dump;
	}

	/**
		Builds the HLSL (Shader Model 4/5) transform dump printed by --hlsl: one section per program /
		variant / stage, with either the emitted HLSL or an "unsupported" diagnostic. Inspection-only.
	*/
	String BuildHlslDump(const SmallVectorImpl<ProgramReflection>& programs)
	{
		const char* tag = "hlsl";
		String dump;
		for (const ProgramReflection& program : programs) {
			dump += "program " + program.Document->ProgramName + "\n";
			for (const VariantReflection& v : program.Variants) {
				dump += (v.Name.empty() ? String("variant (base)\n") : String("variant " + v.Name + "\n"));
				for (std::int32_t stage = 0; stage < 2; stage++) {
					bool vertexStage = (stage == 0);
					dump += String(vertexStage ? "--- vertex (" : "--- fragment (") + tag + ") ---\n";
					String modern = ShaderParser::BuildStageSource(*program.Document, vertexStage, v.Define,
						/*softwareRenderer*/ false, /*noDynamicBranching*/ false, /*lowPowerGpu*/ false);
					String transformed;
					Diagnostic diag;
					if (HlslEmitter::Transform(modern, vertexStage, v.Reflection, transformed, diag)) {
						dump += transformed;
						if (!transformed.empty() && transformed.back() != '\n') {
							dump += "\n";
						}
					} else {
						dump += String(tag) + ": " + diag.Message + " (line " + Death::format("{}", diag.Line) + ")\n";
					}
				}
			}
		}
		return dump;
	}

	/**
		Builds the Vulkan-flavored GLSL ("#version 450") transform dump printed by --vulkan: one section per
		program / variant / stage, with either the emitted Vulkan GLSL or an "unsupported" diagnostic.
		Inspection-only — never touches the emitted header, and does not require glslang.
	*/
	String BuildVulkanDump(const SmallVectorImpl<ProgramReflection>& programs)
	{
		String dump;
		for (const ProgramReflection& program : programs) {
			dump += "program " + program.Document->ProgramName + "\n";
			for (const VariantReflection& v : program.Variants) {
				dump += (v.Name.empty() ? String("variant (base)\n") : String("variant " + v.Name + "\n"));
				for (std::int32_t stage = 0; stage < 2; stage++) {
					bool vertexStage = (stage == 0);
					dump += (vertexStage ? "--- vertex (vulkan) ---\n" : "--- fragment (vulkan) ---\n");
					String modern = ShaderParser::BuildStageSource(*program.Document, vertexStage, v.Define);
					String vulkanGlsl;
					Diagnostic diag;
					if (VulkanGlslEmitter::Transform(modern, vertexStage, v.Reflection, vulkanGlsl, diag)) {
						dump += vulkanGlsl;
						if (!vulkanGlsl.empty() && vulkanGlsl.back() != '\n') {
							dump += "\n";
						}
					} else {
						dump += "vulkan: " + diag.Message + " (line " + Death::format("{}", diag.Line) + ")\n";
					}
				}
			}
		}
		return dump;
	}

	/**
		Builds the Metal Shading Language transform dump printed by --msl: one section per program / variant /
		stage, with either the emitted MSL or an "unsupported" diagnostic. Inspection-only - never touches the
		emitted header. There is no MSL compiler outside Xcode, so this is also the one place to eyeball what
		the Metal backend will hand to MTLDevice::newLibrary().
	*/
	String BuildMslDump(const SmallVectorImpl<ProgramReflection>& programs)
	{
		String dump;
		for (const ProgramReflection& program : programs) {
			for (const VariantReflection& v : program.Variants) {
				for (std::int32_t stage = 0; stage < 2; stage++) {
					const bool vertexStage = (stage == 0);
					dump += "=== " + program.Document->ProgramName + (v.Name.empty() ? String("") : String(" [" + v.Name + "]")) + " ";
					dump += (vertexStage ? "--- vertex (msl) ---\n" : "--- fragment (msl) ---\n");
					String modern = ShaderParser::BuildStageSource(*program.Document, vertexStage, v.Define);
					String msl;
					Diagnostic diag;
					if (MslEmitter::Transform(modern, vertexStage, v.Reflection, msl, diag)) {
						dump += msl;
					} else {
						dump += "// unsupported: " + diag.Message + "\n";
					}
					dump += "\n";
				}
			}
		}
		return dump;
	}

	// ── SwGeneratedShaders.h Emission (GLSL-to-C++ software fragment functions) ──────────────────

	/** One non-sampler uniform of a transpiled shader's "<Program>_Uniforms" struct */
	bool LoadProgramsForFile(const char* inputPath, SmallVectorImpl<ShaderDocument>& documents,
		SmallVectorImpl<ProgramReflection>& programs, String& errorMsg, bool strictTextureUnits = false,
		std::int32_t* outErrorLine = nullptr)
	{
		String content;
		if (!ReadFileToString(inputPath, content)) {
			errorMsg = "cannot read input file";
			return false;
		}
		{
			String includeError;
			FileReader reader = [](StringView path, String& out) {
				return ReadFileToString(path, out);
			};
			if (!ShaderParser::ExpandIncludes(content, ShaderParser::DirectoryOf(inputPath), reader, 0, includeError)) {
				errorMsg = includeError;
				return false;
			}
		}
		Diagnostic diag;
		if (!ShaderParser::ParseDocuments(content, documents, diag)) {
			errorMsg = diag.Message;
			if (outErrorLine != nullptr) {
				*outErrorLine = diag.Line;
			}
			return false;
		}
		programs.reserve(documents.size());
		for (const ShaderDocument& document : documents) {
			ProgramReflection program;
			program.Document = &document;
			program.Variants.emplace_back();
			for (const VariantDirective& variant : document.Variants) {
				VariantReflection v;
				v.Name = variant.Name;
				v.Define = variant.Define;
				program.Variants.push_back(Death::move(v));
			}
			for (VariantReflection& v : program.Variants) {
				StageReflection vertex, fragment;
				if (!ReflectVariantStage(document, true, v.Define, vertex, diag) ||
					!ReflectVariantStage(document, false, v.Define, fragment, diag) ||
					!GlslReflector::MergeStages(vertex, fragment, v.Reflection, diag)) {
					errorMsg = diag.Message;
					if (outErrorLine != nullptr) {
						*outErrorLine = diag.Line;
					}
					return false;
				}
			}
			// Apply "texture_unit(N)" hints. Leniently by default - an unmatched hint just leaves the sampler
			// unassigned, which makes the transpiler decline that shader rather than fail the whole aggregate run
			for (const TextureDirective& directive : document.Textures) {
				bool found = false;
				for (VariantReflection& v : program.Variants) {
					for (TextureInfo& t : v.Reflection.Textures) {
						if (t.Name == directive.Name) {
							t.Unit = directive.Unit;
							found = true;
						}
					}
				}
				if (!found && strictTextureUnits) {
					errorMsg = "texture unit assignment \"" + directive.Name + "\" does not match any sampler uniform";
					if (outErrorLine != nullptr) {
						*outErrorLine = directive.Line;
					}
					return false;
				}
			}
			programs.push_back(Death::move(program));
		}
		return true;
	}

	// ── HLSL Validation via d3dcompiler_47's D3DCompile ──────────────────────────────────────────

#if defined(DEATH_TARGET_WINDOWS)
	pD3DCompile g_D3DCompile = nullptr;

	/** Loads d3dcompiler_47.dll (ships with Windows) and resolves D3DCompile; false if unavailable */
	bool LoadD3DCompiler(String& error)
	{
		if (g_D3DCompile != nullptr) {
			return true;
		}
		HMODULE mod = ::LoadLibraryW(L"d3dcompiler_47.dll");
		if (mod == nullptr) {
			error = "cannot load d3dcompiler_47.dll";
			return false;
		}
		g_D3DCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(mod, "D3DCompile"));
		if (g_D3DCompile == nullptr) {
			error = "d3dcompiler_47.dll has no D3DCompile export";
			return false;
		}
		return true;
	}

	/**
		Compiles @p source as HLSL (entry @p entry, profile @p target) to DXBC bytes in @p dxbc; returns
		success + the compiler log. The flags match the D3D11 backend's runtime-compilation contract —
		column-major cbuffer matrix packing (the emitter's mul(M, v) column-vector algebra reads the engine's
		OpenGL-convention uniform data verbatim) and strictness — but with full optimization, since this runs
		offline where compile time doesn't matter.
	*/
	bool CompileHlslToDxbc(const String& source, const char* entry, const char* target, SmallVectorImpl<std::uint8_t>& dxbc, String& log)
	{
		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;
		const UINT flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		HRESULT hr = g_D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
			entry, target, flags, 0, &code, &errors);
		if (errors != nullptr) {
			log = String { reinterpret_cast<const char*>(errors->GetBufferPointer()) };
			errors->Release();
		}
		if (code != nullptr) {
			if (SUCCEEDED(hr)) {
				dxbc.assign(reinterpret_cast<const std::uint8_t*>(code->GetBufferPointer()),
					reinterpret_cast<const std::uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
			}
			code->Release();
		}
		return SUCCEEDED(hr) && !dxbc.empty();
	}

	/** Compiles @p source as HLSL (entry @p entry, profile @p target); returns success + the compiler log */
	bool CompileHlsl(const String& source, const char* entry, const char* target, String& log)
	{
		SmallVector<std::uint8_t, 0> dxbc;
		return CompileHlslToDxbc(source, entry, target, dxbc, log);
	}

	// ── Vulkan SPIR-V Compilation via a Child glslangValidator Process ───────────────────────────

	/** Matches @p name against @p pattern, where the pattern may contain "*" and "?" wildcards */
	bool MatchesWildcard(StringView pattern, StringView name)
	{
		constexpr std::size_t Npos = ~std::size_t { 0 };
		std::size_t p = 0, n = 0, star = Npos, mark = 0;
		while (n < name.size()) {
			if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
				p++;
				n++;
			} else if (p < pattern.size() && pattern[p] == '*') {
				star = p++;
				mark = n;
			} else if (star != Npos) {
				p = star + 1;
				n = ++mark;
			} else {
				return false;
			}
		}
		while (p < pattern.size() && pattern[p] == '*') {
			p++;
		}
		return (p == pattern.size());
	}

	/**
		Resolves the first existing file matching @p pattern, whose components may contain wildcards
		("C:\...\Microsoft Visual Studio\*\*\...\glslangValidator.exe"). Only needed to dig the copy that
		ships inside a Visual Studio installation out of its version/edition/extension-id directories.
	*/
	bool ResolveWildcardPath(StringView pattern, String& outPath)
	{
		SmallVector<String, 0> pending;
		std::size_t start = 0;
		// Split off the (wildcard-free) root so the walk starts from a real directory
		for (std::size_t i = 0; i < pattern.size(); i++) {
			if (pattern[i] == '\\' || pattern[i] == '/') {
				StringView component = pattern.slice(start, i);
				if (component.contains('*') || component.contains('?')) {
					break;
				}
				start = i + 1;
			}
		}
		if (start == 0) {
			return false;
		}
		pending.emplace_back(pattern.prefix(start - 1));

		while (start < pattern.size()) {
			std::size_t end = start;
			while (end < pattern.size() && pattern[end] != '\\' && pattern[end] != '/') {
				end++;
			}
			StringView component = pattern.slice(start, end);
			const bool lastComponent = (end >= pattern.size());

			SmallVector<String, 0> next;
			for (const String& directory : pending) {
				if (!component.contains('*') && !component.contains('?')) {
					String candidate = fs::CombinePath(directory, component);
					if (lastComponent ? fs::FileExists(candidate) : fs::DirectoryExists(candidate)) {
						next.push_back(Death::move(candidate));
					}
					continue;
				}
				// The last component names the file to find, every earlier one a directory to descend into
				for (StringView entry : fs::Directory(directory, lastComponent ? fs::EnumerationOptions::SkipDirectories | fs::EnumerationOptions::SkipSpecial : fs::EnumerationOptions::SkipFiles | fs::EnumerationOptions::SkipSpecial)) {
					StringView name = fs::GetFileName(entry);
					if (MatchesWildcard(component, name)) {
						next.push_back(fs::CombinePath(directory, name));
					}
				}
			}
			if (next.empty()) {
				return false;
			}
			// Deterministic pick when several installations match
			std::sort(next.begin(), next.end(), [](const String& a, const String& b) {
				return std::strcmp(a.data(), b.data()) < 0;
			});
			pending = Death::move(next);
			start = end + 1;
		}

		if (pending.empty()) {
			return false;
		}
		outPath = pending.front();
		return true;
	}

	/**
		Locates the offline SPIR-V compiler (glslangValidator). An explicit @p overridePath wins (and is an
		error when it does not exist); otherwise "%VULKAN_SDK%\Bin[32]\glslangValidator.exe", the executable
		search PATH, a Visual Studio-bundled copy and finally a repo-local build-tree copy below
		@p searchRoot are tried. Returns false when none is found — SPIR-V is then omitted (the Vulkan
		backend is not buildable), which the callers handle gracefully. glslang is only ever a BUILD-TIME
		dependency.
	*/
	bool LocateGlslang(StringView overridePath, String& outPath, StringView searchRoot = {})
	{
		if (!overridePath.empty()) {
			String candidate = overridePath;
			if (fs::FileExists(candidate)) {
				outPath = Death::move(candidate);
				return true;
			}
			return false;
		}
		String sdk;
		if (TryGetEnvironmentVariable("VULKAN_SDK"_s, sdk)) {
			for (StringView relative : { "Bin\\glslangValidator.exe"_s, "Bin32\\glslangValidator.exe"_s }) {
				String candidate = fs::CombinePath(sdk, relative);
				if (fs::FileExists(candidate)) {
					outPath = Death::move(candidate);
					return true;
				}
			}
		}
		wchar_t found[MAX_PATH];
		DWORD foundLength = ::SearchPathW(nullptr, L"glslangValidator.exe", nullptr, MAX_PATH, found, nullptr);
		if (foundLength > 0 && foundLength < MAX_PATH) {
			outPath = Death::Utf8::FromUtf16(found, std::int32_t(foundLength));
			return true;
		}
		// Visual Studio ships a copy with its shader tooling extension
		for (StringView variable : { "ProgramW6432"_s, "ProgramFiles"_s, "ProgramFiles(x86)"_s }) {
			String programFiles;
			if (!TryGetEnvironmentVariable(variable, programFiles)) {
				continue;
			}
			String pattern = fs::CombinePath(programFiles,
				"Microsoft Visual Studio\\*\\*\\Common7\\IDE\\Extensions\\*\\external\\glslangValidator.exe"_s);
			if (ResolveWildcardPath(pattern, outPath)) {
				return true;
			}
		}
		// Repo-local build-tree copy (may be transient)
		if (!searchRoot.empty()) {
			String candidate = fs::CombinePath(searchRoot, ".fake\\_legacy\\.fake\\glsl\\glslangValidator.exe"_s);
			if (fs::FileExists(candidate)) {
				outPath = Death::move(candidate);
				return true;
			}
		}
		return false;
	}

	/**
		Runs @p commandLine with stdout+stderr redirected to @p logFile; false if the process could not start.
		Both are widened first: an SDK or Program Files path outside the active code page would otherwise be
		mangled on the way into the ANSI entry points.
	*/
	bool RunProcessCaptured(StringView commandLine, StringView logFile, DWORD& exitCode)
	{
		exitCode = ~DWORD { 0 };
		SECURITY_ATTRIBUTES security = {};
		security.nLength = sizeof(security);
		security.bInheritHandle = TRUE;
		HANDLE log = ::CreateFileW(Death::Utf8::ToUtf16(logFile), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (log == INVALID_HANDLE_VALUE) {
			return false;
		}
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = log;
		startup.hStdError = log;
		PROCESS_INFORMATION process = {};
		// CreateProcessW may modify the command line in place, so it cannot be the read-only conversion result
		Array<wchar_t> commandLineW = Death::Utf8::ToUtf16(commandLine);
		SmallVector<wchar_t, 0> mutableCommand(InPlaceInit, commandLineW.begin(), commandLineW.end());
		mutableCommand.push_back(L'\0');
		BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
			nullptr, nullptr, &startup, &process);
		CloseHandle(log);
		if (!created) {
			return false;
		}
		WaitForSingleObject(process.hProcess, INFINITE);
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		return true;
	}

	/**
		Compiles @p source (Vulkan-flavored GLSL) to SPIR-V words via glslangValidator (child process, targeting
		SPIR-V for Vulkan 1.0 so the output is deterministic across glslang builds bar the generator id). Writes
		the stage to a temp file, runs the compiler with "-o", and reads the words back. Returns true and fills
		@p spirv on success; @p log receives the compiler output either way.
	*/
	bool CompileSpirvWithGlslang(const String& glslangPath, const String& source, bool vertexStage,
		SmallVectorImpl<std::uint32_t>& spirv, String& log)
	{
		spirv.clear();
		log = {};
		static unsigned counter = 0;
		String base = MakeTemporaryPathPrefix("sc_vk"_s, counter++);
		if (base.empty()) {
			log = "cannot resolve the temporary directory"_s;
			return false;
		}
		String inputPath = base + (vertexStage ? ".vert" : ".frag");
		String spirvPath = base + ".spv";
		String logPath = base + ".log";

		if (!WriteStringToFile(inputPath, source)) {
			log = "cannot write the temporary shader input";
			return false;
		}

		String command = "\""_s + glslangPath + "\" -V --target-env vulkan1.0 -S "_s +
			(vertexStage ? "vert"_s : "frag"_s) + " \""_s + inputPath + "\" -o \""_s + spirvPath + "\""_s;
		DWORD exitCode = ~DWORD { 0 };
		bool ran = RunProcessCaptured(command, logPath, exitCode);
		{
			String logContent;
			if (ReadFileToString(logPath, logContent)) {
				log = Death::move(logContent);
			}
		}
		bool ok = ran && (exitCode == 0);
		if (ok) {
			String bytes;
			if (ReadFileToString(spirvPath, bytes) && bytes.size() >= 4 && (bytes.size() % 4) == 0) {
				std::size_t words = bytes.size() / 4;
				spirv.resize(words);
				std::memcpy(spirv.data(), bytes.data(), words * 4);
				if (spirv.empty() || spirv[0] != 0x07230203u) { // SPIR-V magic number
					spirv.clear();
					ok = false;
					if (log.empty()) {
						log = "glslang output is not a SPIR-V module";
					}
				}
			} else {
				ok = false;
				if (log.empty()) {
					log = "glslang produced no SPIR-V output";
				}
			}
		}
		fs::RemoveFile(inputPath);
		fs::RemoveFile(spirvPath);
		fs::RemoveFile(logPath);
		return ok && !spirv.empty();
	}
#else
	// D3DCompile is a Windows DLL entry point with no counterpart elsewhere, so the HLSL/DXBC paths stay
	// unavailable; the child-process ones below do not have that excuse and are implemented for real.
	bool LoadD3DCompiler(String& error)
	{
		error = "D3DCompile is only available on Windows";
		return false;
	}
	bool CompileHlslToDxbc(const String&, const char*, const char*, SmallVectorImpl<std::uint8_t>&, String&)
	{
		return false;
	}
	bool CompileHlsl(const String&, const char*, const char*, String&)
	{
		return false;
	}

	/** Runs @p argv with stdout+stderr redirected to @p logFile; false if the process could not start */
	bool RunProcessCaptured(const SmallVectorImpl<String>& argv, StringView logFile, int& exitCode)
	{
		exitCode = -1;
		if (argv.empty()) {
			return false;
		}

		SmallVector<char*, 0> args;
		args.reserve(argv.size() + 1);
		for (const String& a : argv) {
			args.push_back(const_cast<char*>(a.data()));
		}
		args.push_back(nullptr);

		posix_spawn_file_actions_t actions;
		if (posix_spawn_file_actions_init(&actions) != 0) {
			return false;
		}
		// Both streams go to the same file, so a compiler that writes diagnostics to either is captured
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, String::nullTerminatedView(logFile).data(),
			O_WRONLY | O_CREAT | O_TRUNC, 0644);
		posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

		pid_t pid = 0;
		// posix_spawnp so a bare tool name is resolved against PATH, matching how the Windows arm behaves
		const int spawned = ::posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), ::environ);
		posix_spawn_file_actions_destroy(&actions);
		if (spawned != 0) {
			return false;
		}

		int status = 0;
		while (::waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR) {
				return false;
			}
		}
		exitCode = (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return true;
	}

	/** Resolves `glslangValidator`, from @p explicitPath, then `$VULKAN_SDK/bin`, then `PATH` */
	bool LocateGlslang(StringView explicitPath, String& outPath, StringView = {})
	{
		if (!explicitPath.empty()) {
			outPath = String { explicitPath };
			return fs::FileExists(outPath);
		}
		String sdk;
		if (TryGetEnvironmentVariable("VULKAN_SDK"_s, sdk)) {
			String candidate = fs::CombinePath({ sdk, "bin"_s, "glslangValidator"_s });
			if (fs::FileExists(candidate)) {
				outPath = Death::move(candidate);
				return true;
			}
		}
		// Left as a bare name for posix_spawnp to resolve against PATH
		outPath = "glslangValidator"_s;
		return true;
	}

	bool CompileSpirvWithGlslang(const String& glslangPath, const String& source, bool vertexStage,
		SmallVectorImpl<std::uint32_t>& spirv, String& log)
	{
		spirv.clear();
		log = {};
		static unsigned counter = 0;
		String base = MakeTemporaryPathPrefix("sc_vk"_s, counter++);
		if (base.empty()) {
			log = "cannot resolve the temporary directory"_s;
			return false;
		}
		String inputPath = base + (vertexStage ? ".vert"_s : ".frag"_s);
		String spirvPath = base + ".spv"_s;
		String logPath = base + ".log"_s;

		if (!WriteStringToFile(inputPath, source)) {
			log = "cannot write the temporary shader input";
			return false;
		}

		SmallVector<String, 0> argv { InPlaceInit, { glslangPath, "-V"_s, "--target-env"_s, "vulkan1.0"_s, "-S"_s, (vertexStage ? "vert"_s : "frag"_s), inputPath, "-o"_s, spirvPath } };
		int exitCode = -1;
		const bool ran = RunProcessCaptured(argv, logPath, exitCode);
		{
			String logContent;
			if (ReadFileToString(logPath, logContent)) {
				log = Death::move(logContent);
			}
		}
		bool ok = ran && (exitCode == 0);
		if (ok) {
			String bytes;
			if (ReadFileToString(spirvPath, bytes) && bytes.size() >= 4 && (bytes.size() % 4) == 0) {
				std::size_t words = bytes.size() / 4;
				spirv.resize(words);
				std::memcpy(spirv.data(), bytes.data(), words * 4);
				if (spirv.empty() || spirv[0] != 0x07230203u) { // SPIR-V magic number
					spirv.clear();
					ok = false;
					if (log.empty()) {
						log = "glslang output is not a SPIR-V module";
					}
				}
			} else {
				ok = false;
				if (log.empty()) {
					log = "glslang produced no SPIR-V output";
				}
			}
		}
		fs::RemoveFile(inputPath);
		fs::RemoveFile(spirvPath);
		fs::RemoveFile(logPath);
		return ok && !spirv.empty();
	}
#endif

	/** Collapses a multi-line compiler log to a single line for the summary table */
	String FirstLine(StringView s)
	{
		std::size_t nl = 0;
		while (nl < s.size() && s[nl] != '\n' && s[nl] != '\r') {
			nl++;
		}
		return String { s.prefix(nl) };
	}

	/**
		Emits the VS + PS HLSL of every program variant across all @p inputPaths and compiles each stage via
		D3DCompile (vs_4_0 / ps_4_0 with the same flags the embedded DXBC and the D3D11 backend's runtime
		compilation use). Prints a pass/fail table and a summary. Tool-only validation surface.
	*/
	int RunHlslCheck(char** inputPaths, int inputCount)
	{
		String loadError;
		if (!LoadD3DCompiler(loadError)) {
			std::fprintf(stderr, "error: %s\n", loadError.data());
			return 1;
		}

		std::size_t stagesTotal = 0, stagesPassed = 0;
		std::size_t variantsTotal = 0, variantsPassed = 0;
		SmallVector<String, 0> failures; // detailed "prefix STAGE: <reason>" lines

		for (int fi = 0; fi < inputCount; fi++) {
			const char* inputPath = inputPaths[fi];
			SmallVector<ShaderDocument, 0> documents;
			SmallVector<ProgramReflection, 0> programs;
			String errorMsg;
			if (!LoadProgramsForFile(inputPath, documents, programs, errorMsg)) {
				std::fprintf(stderr, "%s: error: %s\n", inputPath, errorMsg.data());
				return 1;
			}
			for (const ProgramReflection& program : programs) {
				const String& progName = program.Document->ProgramName;
				for (const VariantReflection& v : program.Variants) {
					String prefix = (v.Name.empty() ? progName : String(progName + "_" + v.Name));
					variantsTotal++;
					bool bothOk = true;
					String rowStatus;
					for (std::int32_t stage = 0; stage < 2; stage++) {
						bool vertexStage = (stage == 0);
						const char* stageName = (vertexStage ? "VS" : "PS");
						const char* entry = (vertexStage ? "VSMain" : "PSMain");
						const char* target = (vertexStage ? "vs_4_0" : "ps_4_0");
						stagesTotal++;

						String source = ShaderParser::BuildStageSource(*program.Document, vertexStage, v.Define);
						String hlsl;
						Diagnostic diag;
						bool ok;
						String reason;
						if (!HlslEmitter::Transform(source, vertexStage, v.Reflection, hlsl, diag)) {
							ok = false;
							reason = diag.Message;
						} else {
							String log;
							ok = CompileHlsl(hlsl, entry, target, log);
							if (!ok) {
								reason = "D3DCompile: " + FirstLine(log);
							}
						}
						if (ok) {
							stagesPassed++;
							rowStatus += String(" ") + stageName + "=PASS";
						} else {
							bothOk = false;
							rowStatus += String(" ") + stageName + "=FAIL";
							failures.push_back(prefix + " " + stageName + ": " + reason);
						}
					}
					if (bothOk) {
						variantsPassed++;
					}
					std::fprintf(stdout, "  %-42s%s\n", prefix.data(), rowStatus.data());
				}
			}
		}

		std::fprintf(stdout, "\n[HlslCheck] %zu/%zu program-variants compiled both stages; %zu/%zu stages compiled\n",
			variantsPassed, variantsTotal, stagesPassed, stagesTotal);
		if (!failures.empty()) {
			std::fprintf(stdout, "Failures (%zu):\n", failures.size());
			for (const String& f : failures) {
				std::fprintf(stdout, "  %s\n", f.data());
			}
		}
		return 0;
	}

	/**
		Emits the VS + FS Vulkan-flavored GLSL of every program variant across all @p inputPaths and compiles
		each stage to SPIR-V via glslangValidator (Vulkan 1.0 target). Prints a pass/fail table and a summary.
		Tool-only validation surface mirroring --hlsl-check. Optional leading "--glslang <path>" overrides the
		compiler discovery. Exits non-zero when glslang cannot be located.
	*/
	int RunSpirvCheck(char** args, int count)
	{
		StringView glslangOverride;
		int start = 0;
		while (start < count && StringView(args[start]) == "--glslang") {
			if (start + 1 >= count) {
				std::fprintf(stderr, "error: --glslang requires a path argument\n");
				return 2;
			}
			glslangOverride = args[start + 1];
			start += 2;
		}
		if (start >= count) {
			std::fprintf(stderr, "error: --spirv-check requires at least one input .shader\n");
			return 2;
		}

		String glslang;
		if (!LocateGlslang(glslangOverride, glslang)) {
			std::fprintf(stderr, "error: glslang not found, cannot produce SPIR-V.\n"
								 "       Install the Vulkan SDK (sets VULKAN_SDK), put glslangValidator on PATH, or pass --glslang <path>.\n");
			return 1;
		}
		std::fprintf(stdout, "[SpirvCheck] using glslang: %s\n", glslang.data());

		std::size_t stagesTotal = 0, stagesPassed = 0;
		std::size_t variantsTotal = 0, variantsPassed = 0;
		SmallVector<String, 0> failures;

		for (int fi = start; fi < count; fi++) {
			const char* inputPath = args[fi];
			SmallVector<ShaderDocument, 0> documents;
			SmallVector<ProgramReflection, 0> programs;
			String errorMsg;
			if (!LoadProgramsForFile(inputPath, documents, programs, errorMsg)) {
				std::fprintf(stderr, "%s: error: %s\n", inputPath, errorMsg.data());
				return 1;
			}
			for (const ProgramReflection& program : programs) {
				const String& progName = program.Document->ProgramName;
				for (const VariantReflection& v : program.Variants) {
					String prefix = (v.Name.empty() ? progName : String(progName + "_" + v.Name));
					variantsTotal++;
					bool bothOk = true;
					String rowStatus;
					for (std::int32_t stage = 0; stage < 2; stage++) {
						bool vertexStage = (stage == 0);
						const char* stageName = (vertexStage ? "VS" : "FS");
						stagesTotal++;

						String source = ShaderParser::BuildStageSource(*program.Document, vertexStage, v.Define);
						String vulkanGlsl;
						Diagnostic diag;
						bool ok;
						String reason;
						if (!VulkanGlslEmitter::Transform(source, vertexStage, v.Reflection, vulkanGlsl, diag)) {
							ok = false;
							reason = diag.Message;
						} else {
							SmallVector<std::uint32_t, 0> spirv;
							String log;
							ok = CompileSpirvWithGlslang(glslang, vulkanGlsl, vertexStage, spirv, log);
							if (!ok) {
								reason = "glslang: " + FirstLine(log);
							}
						}
						if (ok) {
							stagesPassed++;
							rowStatus += String(" ") + stageName + "=PASS";
						} else {
							bothOk = false;
							rowStatus += String(" ") + stageName + "=FAIL";
							failures.push_back(prefix + " " + stageName + ": " + reason);
						}
					}
					if (bothOk) {
						variantsPassed++;
					}
					std::fprintf(stdout, "  %-42s%s\n", prefix.data(), rowStatus.data());
				}
			}
		}

		std::fprintf(stdout, "\n[SpirvCheck] %zu/%zu program-variants compiled both stages; %zu/%zu stages compiled\n",
			variantsPassed, variantsTotal, stagesPassed, stagesTotal);
		if (!failures.empty()) {
			std::fprintf(stdout, "Failures (%zu):\n", failures.size());
			for (const String& f : failures) {
				std::fprintf(stdout, "  %s\n", f.data());
			}
		}
		return 0;
	}

	// ── Whole-Directory Regeneration (--generate-all) ────────────────────────────────────────────

	/**
		Options of the --generate-all driver, which produces every committed artifact under
		"src/Shaders/Generated" in one process, from one enumeration of the shader directory. Doing it
		here rather than from a shell script is what makes a regeneration all-or-nothing: a partial run
		(some aggregate written from an older shader list, one header emitted without SPIR-V) cannot leave
		the committed set internally inconsistent, and the file order no longer depends on the shell's
		locale-sensitive sorting.
	*/
	struct GenerateAllOptions
	{
		String ShadersDirectory; // Empty: auto-detected from the executable, then the working directory
		String OutputDirectory; // Empty: "<ShadersDirectory>/Generated"
		const char* GlslangOverride = nullptr;
		bool Check = false;
		bool NoDxbc = false;
		bool GlOnly = false;
	};

	/**
		Finds the "src/Shaders" directory by walking up from the executable (the tool normally lives in
		"zig-out/bin") and then from the working directory. Any build layout
		this does not cover is served by an explicit --shaders-dir.
	*/
	bool AutoDetectShadersDirectory(String& outPath)
	{
		const String starts[] = {
			fs::GetDirectoryName(fs::GetExecutablePath()),
			fs::GetWorkingDirectory()
		};
		for (const String& start : starts) {
			String directory = start;
			for (std::int32_t level = 0; level < 10 && !directory.empty(); level++) {
				String candidates[] = {
					fs::CombinePath({ directory, "src"_s, "Shaders"_s }),
					fs::CombinePath(directory, "Shaders"_s)
				};
				for (String& candidate : candidates) {
					if (fs::DirectoryExists(candidate)) {
						outPath = Death::move(candidate);
						return true;
					}
				}
				directory = fs::GetDirectoryName(directory);
			}
		}
		return false;
	}

	/**
		Builds the umbrella header: one include per generated program header plus the per-namespace
		AllPrograms[] index arrays, so runtime code has a single include and can enumerate the programs.
		A canvas_item file with a "batched" directive contributes two program symbols from one header.
	*/
	String BuildUmbrellaHeader(const SmallVectorImpl<String>& includeStems,
		const SmallVectorImpl<String>& jazz2Programs, const SmallVectorImpl<String>& ncinePrograms)
	{
		String out;
		out += "// Generated by ShaderCompiler (--generate-all). Do not edit manually.\n";
		out += "#pragma once\n\n";
		for (const String& stem : includeStems) {
			out += "#include \"" + stem + ".h\"\n";
		}
		out += "\n";
		// The generated shader data namespaces carry no public API and are excluded from the API
		// documentation (Doxygen defines `DOXYGEN_GENERATING_OUTPUT`), keeping this header out of it
		out += "#ifndef DOXYGEN_GENERATING_OUTPUT\n";
		const struct
		{
			const char* Namespace;
			const char* Comment;
			const SmallVectorImpl<String>* Programs;
		} sections[] = {
			{ "Jazz2::ShadersGen", "All generated Jazz2 shader programs, sorted by name", &jazz2Programs },
			{ "nCine::ShadersGen", "All generated nCine default shader programs, sorted by name", &ncinePrograms }
		};
		for (const auto& section : sections) {
			out += "namespace ";
			out += section.Namespace;
			out += "\n{\n\t// ";
			out += section.Comment;
			out += "\n\tinline constexpr const ShaderCompiler::Program* AllPrograms[] = {\n";
			for (const String& name : *section.Programs) {
				out += "\t\t&" + name + ",\n";
			}
			out += "\t};\n}\n";
			if (&section == &sections[0]) {
				out += "\n";
			}
		}
		out += "#endif\n";
		return out;
	}

	/**
		Writes the whole artifact set into @p outputDirectory and appends the name of every file it wrote to
		@p writtenNames. Split out of RunGenerateAll so that the caller owns the temporary directory of the
		staleness guard and can clean it up on every path.
	*/
	/**
		Wraps the bodies collected in BackendArtifacts into one complete aggregate header

		@p guard is the `WITH_RHI_*` macro the whole file hides behind, so a build without that backend
		compiles none of it. @p bodies pairs each namespace with the declarations belonging to it, in the
		order the shaders were enumerated - the same order the per-shader headers were written in.
	*/
	String BuildBackendAggregate(StringView what, StringView guard,
		const SmallVectorImpl<std::pair<String, String>>& bodies)
	{
		String output;
		output += "// Generated by ShaderCompiler (--generate-all). Do not edit manually.\n";
		output += "//\n";
		output += "// " + String { what } + " stage artifacts for every shader, kept apart from the per-shader headers because\n";
		output += "// producing them needs a compiler that is not available everywhere. A regeneration on a machine\n";
		output += "// without it leaves this file alone rather than replacing its contents with nulls.\n";
		output += "#pragma once\n";
		output += "\n";
		output += "#include \"ShaderCompilerTypes.h\"\n";
		output += "\n";
		output += "#if defined(" + String { guard } + ")\n";
		output += "#ifndef DOXYGEN_GENERATING_OUTPUT\n";
		for (const auto& body : bodies) {
			output += "\n";
			output += "namespace " + body.first + "\n";
			output += "{\n";
			output += body.second;
			output += "}\n";
		}
		output += "\n";
		output += "#endif\n";
		output += "#endif\n";
		return output;
	}

	int GenerateAllArtifacts(StringView shadersDirectory, const SmallVectorImpl<String>& shaderNames,
		StringView outputDirectory, SpirvCompileFn& compileSpirv, DxbcCompileFn& compileDxbc,
		SmallVectorImpl<String>& writtenNames, bool glOnly)
	{
		// The shared reflection types every generated header includes
		{
			String path = fs::CombinePath(outputDirectory, "ShaderCompilerTypes.h"_s);
			if (!WriteStringToFile(path, Emitter::BuildTypesHeader())) {
				std::fprintf(stderr, "error: cannot write output file \"%s\"\n", path.data());
				return 1;
			}
			writtenNames.emplace_back("ShaderCompilerTypes.h"_s);
		}

		// One header per shader; "Default*.shader" are the nCine default programs, everything else is Jazz2
		SmallVector<String, 0> includeStems, jazz2Programs, ncinePrograms;
		// The Direct3D 11, Vulkan and Metal stage artifacts of every shader, collected here and written as one
		// aggregate each below - see BackendArtifacts for why they do not go into the per-shader headers
		SmallVector<std::pair<String, String>, 0> d3d11Bodies, vulkanBodies, metalBodies;
		for (const String& shaderName : shaderNames) {
			StringView stem = fs::GetFileNameWithoutExtension(shaderName);
			const bool isDefault = stem.hasPrefix("Default"_s);
			const char* ns = (isDefault ? "nCine::ShadersGen" : "Jazz2::ShadersGen");
			String inputPath = fs::CombinePath(shadersDirectory, shaderName);

			SmallVector<ShaderDocument, 0> documents;
			SmallVector<ProgramReflection, 0> programs;
			String errorMsg;
			std::int32_t errorLine = 0;
			if (!LoadProgramsForFile(inputPath.data(), documents, programs, errorMsg, /*strictTextureUnits*/ true, &errorLine)) {
				std::fprintf(stderr, "%s:%d: error: %s\n", inputPath.data(), errorLine, errorMsg.data());
				return 1;
			}

			String output;
			BackendArtifacts artifacts;
			Diagnostic diag;
			if (!Emitter::EmitHeader(programs, ns, inputPath, compileSpirv, compileDxbc, output, artifacts, diag)) {
				std::fprintf(stderr, "%s:%d: error: %s\n", inputPath.data(), diag.Line, diag.Message.data());
				return 1;
			}
			d3d11Bodies.emplace_back(String { ns }, Death::move(artifacts.D3d11));
			vulkanBodies.emplace_back(String { ns }, Death::move(artifacts.Vulkan));
			metalBodies.emplace_back(String { ns }, Death::move(artifacts.Metal));
			String headerName = stem + ".h"_s;
			String headerPath = fs::CombinePath(outputDirectory, headerName);
			if (!WriteStringToFile(headerPath, output)) {
				std::fprintf(stderr, "error: cannot write output file \"%s\"\n", headerPath.data());
				return 1;
			}

			writtenNames.push_back(headerName);
			includeStems.emplace_back(stem);
			for (const ProgramReflection& program : programs) {
				(isDefault ? ncinePrograms : jazz2Programs).push_back(program.Document->ProgramName);
			}
			std::fprintf(stdout, "ok: %s -> %s [%s]\n", shaderName.data(), headerName.data(), ns);
		}

		// Emit the optional desktop backend aggregates.
		if (!glOnly) {
			// Whether the compiler WORKS, not whether a callback was installed. LocateGlslang() is happy to
			// hand back a bare "glslangValidator" for PATH to resolve, which it then does not - so every
			// all nulls. Probe with a trivial shader before replacing a generated aggregate.
			SmallVector<std::uint32_t, 0> probeWords;
			SmallVector<std::uint8_t, 0> probeBytes;
			String probeLog;
			const bool spirvWorks = compileSpirv &&
				compileSpirv("#version 450\nvoid main() { gl_Position = vec4(0.0); }"_s, true, probeWords, probeLog) &&
				!probeWords.empty();
			const bool dxbcWorks = compileDxbc &&
				compileDxbc("float4 VSMain() : SV_Position { return 0; }"_s, true, probeBytes, probeLog) &&
				!probeBytes.empty();

			const struct
			{
				const char* What;
				const char* Guard;
				const char* FileName;
				const SmallVectorImpl<std::pair<String, String>>* Bodies;
				bool Rebuildable;
				const char* Missing;
			} aggregates[] = {
				{ "Direct3D 11", "WITH_RHI_D3D11", "D3d11GeneratedShaders.h", &d3d11Bodies,
					dxbcWorks, "no working DXBC compiler (D3DCompile is Windows-only)" },
				{ "Vulkan", "WITH_RHI_VULKAN", "VulkanGeneratedShaders.h", &vulkanBodies,
					spirvWorks, "no working glslang" },
				// MSL is compiled on the Mac at load time, so the aggregate is plain source and every machine
				// can (and does) rebuild it
				{ "Metal", "WITH_RHI_METAL", "MetalGeneratedShaders.h", &metalBodies,
					true, "" }
			};
			for (const auto& aggregate : aggregates) {
				String path = fs::CombinePath(outputDirectory, aggregate.FileName);
				// An aggregate that does not exist yet is written even with nothing to put in it: the
				// per-shader headers include it unconditionally under the backend's guard, so a missing file
				// is a build error rather than a missing optimisation. A null one at least links, and the
				// backend reports the empty program itself. Only an EXISTING file is protected.
				if (!aggregate.Rebuildable && fs::FileExists(path)) {
					std::fprintf(stdout, "skipped: %s (%s)\n", aggregate.FileName, aggregate.Missing);
					continue;
				}
				if (!WriteStringToFile(path, BuildBackendAggregate(aggregate.What, aggregate.Guard, *aggregate.Bodies))) {
					std::fprintf(stderr, "error: cannot write output file \"%s\"\n", path.data());
					return 1;
				}
				writtenNames.emplace_back(aggregate.FileName);
				std::fprintf(stdout, "ok: %s stage artifacts -> %s\n", aggregate.What, aggregate.FileName);
			}
		}

		// The umbrella header (includes sorted by name, index arrays in shader order)
		std::sort(includeStems.begin(), includeStems.end(), [](const String& a, const String& b) {
			return std::strcmp(a.data(), b.data()) < 0;
		});
		{
			String path = fs::CombinePath(outputDirectory, "ShadersGen.h"_s);
			if (!WriteStringToFile(path, BuildUmbrellaHeader(includeStems, jazz2Programs, ncinePrograms))) {
				std::fprintf(stderr, "error: cannot write output file \"%s\"\n", path.data());
				return 1;
			}
			writtenNames.emplace_back("ShadersGen.h"_s);
			std::fprintf(stdout, "ok: umbrella -> ShadersGen.h (%zu Jazz2 + %zu nCine programs)\n",
				jazz2Programs.size(), ncinePrograms.size());
		}

		return 0;
	}

	int RunGenerateAll(const GenerateAllOptions& options)
	{
		String shadersDirectory = options.ShadersDirectory;
		if (shadersDirectory.empty() && !AutoDetectShadersDirectory(shadersDirectory)) {
			std::fprintf(stderr, "error: cannot locate the shader directory - pass --shaders-dir <dir>\n");
			return 1;
		}
		if (!fs::DirectoryExists(shadersDirectory)) {
			std::fprintf(stderr, "error: shader directory \"%s\" does not exist\n", shadersDirectory.data());
			return 1;
		}
		String committedDirectory = (options.OutputDirectory.empty()
				? fs::CombinePath(shadersDirectory, "Generated"_s)
				: options.OutputDirectory);

		SmallVector<String, 0> shaderNames;
		if (!ListFilesInDirectory(shadersDirectory, ".shader"_s, shaderNames) || shaderNames.empty()) {
			std::fprintf(stderr, "error: no .shader files found in \"%s\"\n", shadersDirectory.data());
			return 1;
		}

		// SPIR-V is embedded when a glslang is around; without it the fields stay null and the headers still
		// build (see LocateGlslang). The repo-local fallback is looked for next to "<shaders>/../..".
		SpirvCompileFn compileSpirv;
		{
			String glslang;
			StringView override = (options.GlslangOverride != nullptr ? StringView { options.GlslangOverride } : StringView {});
			if (!options.GlOnly && LocateGlslang(override, glslang, fs::GetDirectoryName(fs::GetDirectoryName(shadersDirectory)))) {
				std::fprintf(stdout, "using glslang: %s\n", glslang.data());
				compileSpirv = [glslang](StringView vulkanGlsl, bool vertexStage, SmallVectorImpl<std::uint32_t>& spirv, String& log) {
					return CompileSpirvWithGlslang(glslang, String { vulkanGlsl }, vertexStage, spirv, log);
				};
			} else if (!options.GlOnly && !override.empty()) {
				std::fprintf(stderr, "warning: glslangValidator not found at \"%s\" - Vulkan SPIR-V will be omitted\n",
					options.GlslangOverride);
			} else if (!options.GlOnly) {
				std::fprintf(stderr, "warning: glslangValidator not found - Vulkan SPIR-V will be omitted "
									 "(install the Vulkan SDK for the Vulkan backend)\n");
			}
		}

		DxbcCompileFn compileDxbc;
		if (options.GlOnly) {
			// OpenGL/WebGL use the generated GLSL directly.
		} else if (options.NoDxbc) {
			std::fprintf(stdout, "DXBC embedding disabled (--no-dxbc) - HLSL sources will be embedded instead\n");
		} else {
			String loadError;
			if (LoadD3DCompiler(loadError)) {
				compileDxbc = [](StringView hlsl, bool vertexStage, SmallVectorImpl<std::uint8_t>& dxbc, String& log) {
					bool ok = CompileHlslToDxbc(String { hlsl }, vertexStage ? "VSMain" : "PSMain",
						vertexStage ? "vs_4_0" : "ps_4_0", dxbc, log);
					if (!ok) {
						std::fprintf(stderr, "warning: D3DCompile (%s) failed - embedding the HLSL source instead: %s\n",
							vertexStage ? "vs_4_0" : "ps_4_0", FirstLine(log).data());
					}
					return ok;
				};
			}
#if defined(DEATH_TARGET_WINDOWS)
			else {
				std::fprintf(stderr, "warning: %s - DXBC will be omitted (HLSL sources embedded instead)\n", loadError.data());
			}
#endif
		}

		// --check never touches the tree: everything is generated into a temporary directory and compared
		String outputDirectory = committedDirectory;
		String temporaryDirectory;
		if (options.Check) {
			if (!CreateTemporaryDirectory(temporaryDirectory)) {
				std::fprintf(stderr, "error: cannot create a temporary directory for --check\n");
				return 1;
			}
			outputDirectory = temporaryDirectory;
			if (!compileSpirv) {
				std::fprintf(stderr, "warning: --check without glslang - committed headers with embedded SPIR-V will be reported stale\n");
			}
			if (options.NoDxbc) {
				std::fprintf(stderr, "warning: --check with --no-dxbc - committed headers with embedded DXBC will be reported stale\n");
			}
		} else if (!fs::CreateDirectories(outputDirectory)) {
			std::fprintf(stderr, "error: cannot create output directory \"%s\"\n", outputDirectory.data());
			return 1;
		}

		SmallVector<String, 0> writtenNames;
		int result = GenerateAllArtifacts(shadersDirectory, shaderNames, outputDirectory, compileSpirv,
			compileDxbc, writtenNames, options.GlOnly);

		if (!options.Check) {
			if (result == 0) {
				std::fprintf(stdout, "All %zu shaders generated successfully.\n", shaderNames.size());
			}
			return result;
		}

		// Byte-compare the fresh artifacts against the committed ones; missing and extra files count as stale
		SmallVector<String, 0> stale;
		if (result == 0) {
			for (const String& name : writtenNames) {
				String fresh = fs::CombinePath(outputDirectory, name);
				String committed = fs::CombinePath(committedDirectory, name);
				if (!fs::FileExists(committed)) {
					stale.push_back(name + " (missing from Generated)"_s);
				} else if (!FilesHaveEqualContent(fresh, committed)) {
					stale.push_back(name);
				}
			}
			SmallVector<String, 0> committedNames;
			ListFilesInDirectory(committedDirectory, ".h"_s, committedNames);
			for (const String& name : committedNames) {
				const bool generated = std::any_of(writtenNames.begin(), writtenNames.end(),
					[&name](const String& written) {
						return written == name;
					});
				if (!generated) {
					stale.push_back(name + " (committed but no longer generated)"_s);
				}
			}
		}

		for (const String& name : writtenNames) {
			String path = fs::CombinePath(outputDirectory, name);
			fs::RemoveFile(path);
		}
		fs::RemoveDirectoryRecursive(outputDirectory);

		if (result != 0) {
			return result;
		}
		if (!stale.empty()) {
			std::fprintf(stderr, "error: %zu generated header(s) are STALE - re-run --generate-all and commit:\n", stale.size());
			for (const String& name : stale) {
				std::fprintf(stderr, "  %s\n", name.data());
			}
			return 1;
		}
		std::fprintf(stdout, "All %zu shaders verified up to date.\n", shaderNames.size());
		return 0;
	}
}

int main(int argc, char* argv[])
{
	const char* inputPath = nullptr;
	const char* outputPath = nullptr;
	String ns = "ShaderArtifacts";
	bool checkOnly = false;
	bool essl100Check = false;

	// Standalone mode: write the shared reflection-types header and exit
	if (argc == 3 && StringView(argv[1]) == "--emit-types") {
		if (!WriteStringToFile(argv[2], Emitter::BuildTypesHeader())) {
			std::fprintf(stderr, "error: cannot write types header \"%s\"\n", argv[2]);
			return 1;
		}
		return 0;
	}

	// Standalone mode: regenerate every committed artifact of "src/Shaders/Generated" in one run - the
	// shared types, the per-shader headers, the umbrella and the five aggregates. Usage:
	//   ShaderCompiler --generate-all [--shaders-dir <dir>] [--out-dir <dir>] [--check] [--no-dxbc] [--glslang <path>]
	if (argc >= 2 && StringView(argv[1]) == "--generate-all") {
		GenerateAllOptions options;
		for (int i = 2; i < argc; i++) {
			StringView arg = argv[i];
			if (arg == "--shaders-dir" || arg == "--out-dir" || arg == "--glslang") {
				if (i + 1 >= argc) {
					std::fprintf(stderr, "error: %s requires a path argument\n", arg.data());
					return 2;
				}
				const char* value = argv[++i];
				if (arg == "--shaders-dir") {
					options.ShadersDirectory = value;
				} else if (arg == "--out-dir") {
					options.OutputDirectory = value;
				} else {
					options.GlslangOverride = value;
				}
			} else if (arg == "--gl-only") {
				options.GlOnly = true;
			} else if (arg == "--check") {
				options.Check = true;
			} else if (arg == "--no-dxbc") {
				options.NoDxbc = true;
			} else {
				std::fprintf(stderr, "error: unknown --generate-all option \"%s\"\n", arg.data());
				PrintUsage();
				return 2;
			}
		}
		return RunGenerateAll(options);
	}

	// Standalone mode: emit the VS+PS HLSL of every variant across the input shaders and compile each via
	// d3dcompiler_47's D3DCompile, printing a pass/fail table. Usage:
	//   ShaderCompiler --hlsl-check <input1.shader> [input2.shader ...]
	if (argc >= 2 && StringView(argv[1]) == "--hlsl-check") {
		if (argc < 3) {
			std::fprintf(stderr, "error: --hlsl-check requires at least one input .shader\n");
			return 2;
		}
		return RunHlslCheck(&argv[2], argc - 2);
	}

	// Standalone mode: emit the VS+FS Vulkan GLSL of every variant across the input shaders and compile each to
	// SPIR-V via glslangValidator, printing a pass/fail table. Usage:
	//   ShaderCompiler --spirv-check [--glslang <path>] <input1.shader> [input2.shader ...]
	if (argc >= 2 && StringView(argv[1]) == "--spirv-check") {
		if (argc < 3) {
			std::fprintf(stderr, "error: --spirv-check requires at least one input .shader\n");
			return 2;
		}
		return RunSpirvCheck(&argv[2], argc - 2);
	}

	bool hlslDump = false;
	bool vulkanDump = false;
	bool mslDump = false;
	bool noDxbc = false;
	const char* glslangOverride = nullptr;
	for (int i = 1; i < argc; i++) {
		StringView arg = argv[i];
		if (arg == "-o") {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "error: -o requires a path argument\n");
				return 2;
			}
			outputPath = argv[++i];
		} else if (arg == "-n") {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "error: -n requires a namespace argument\n");
				return 2;
			}
			ns = argv[++i];
		} else if (arg == "--check") {
			checkOnly = true;
		} else if (arg == "--essl100-check") {
			essl100Check = true;
		} else if (arg == "--hlsl") {
			hlslDump = true;
		} else if (arg == "--no-dxbc") {
			noDxbc = true;
		} else if (arg == "--vulkan") {
			vulkanDump = true;
		} else if (arg == "--msl") {
			mslDump = true;
		} else if (arg == "--glslang") {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "error: --glslang requires a path argument\n");
				return 2;
			}
			glslangOverride = argv[++i];
		} else if (arg == "--target") {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "error: --target requires an argument (only: essl100)\n");
				return 2;
			}
			StringView target = argv[++i];
			if (target != "essl100") {
				std::fprintf(stderr, "error: unknown --target \"%s\" (only: essl100)\n", target.data());
				return 2;
			}
			essl100Check = true;
		} else if (arg == "--help" || arg == "-h" || arg == "/?") {
			PrintUsage();
			return 0;
		} else if (!arg.empty() && arg[0] == '-') {
			std::fprintf(stderr, "error: unknown option \"%s\"\n", arg.data());
			PrintUsage();
			return 2;
		} else if (inputPath == nullptr) {
			inputPath = argv[i];
		} else {
			std::fprintf(stderr, "error: multiple input files are not supported\n");
			return 2;
		}
	}

	if (inputPath == nullptr || (!checkOnly && !essl100Check && !hlslDump && !vulkanDump && !mslDump && outputPath == nullptr)) {
		PrintUsage();
		return 2;
	}
	if (!IsValidNamespace(ns)) {
		std::fprintf(stderr, "error: invalid namespace \"%s\"\n", ns.data());
		return 2;
	}

	String content;
	if (!ReadFileToString(inputPath, content)) {
		std::fprintf(stderr, "error: cannot read input file \"%s\"\n", inputPath);
		return 1;
	}

	{
		String includeError;
		FileReader reader = [](StringView path, String& out) {
			return ReadFileToString(path, out);
		};
		if (!ShaderParser::ExpandIncludes(content, ShaderParser::DirectoryOf(inputPath), reader, 0, includeError)) {
			std::fprintf(stderr, "%s: error: %s\n", inputPath, includeError.data());
			return 1;
		}
	}

	// Custom-mode files produce one document; canvas_item files may add the "batched" twin program
	Diagnostic diag;
	SmallVector<ShaderDocument, 0> documents;
	if (!ShaderParser::ParseDocuments(content, documents, diag)) {
		return ReportError(inputPath, diag);
	}

	SmallVector<ProgramReflection, 0> programs;
	programs.reserve(documents.size());
	for (const ShaderDocument& document : documents) {
		ProgramReflection program;
		program.Document = &document;

		// The unnamed base variant (Name "", always Variants[0]) plus one additional entry
		// per declared variant (a combination is declared as a variant of its own)
		program.Variants.emplace_back();
		for (const VariantDirective& variant : document.Variants) {
			VariantReflection v;
			v.Name = variant.Name;
			v.Define = variant.Define;
			program.Variants.push_back(Death::move(v));
		}

		for (VariantReflection& v : program.Variants) {
			StageReflection vertex, fragment;
			if (!ReflectVariantStage(document, true, v.Define, vertex, diag) ||
				!ReflectVariantStage(document, false, v.Define, fragment, diag)) {
				return ReportError(inputPath, diag);
			}
			if (!GlslReflector::MergeStages(vertex, fragment, v.Reflection, diag)) {
				return ReportError(inputPath, diag);
			}
		}

		// Apply "texture_unit(N)" hint unit assignments across all variants
		for (const TextureDirective& directive : document.Textures) {
			bool found = false;
			for (VariantReflection& v : program.Variants) {
				for (TextureInfo& t : v.Reflection.Textures) {
					if (t.Name == directive.Name) {
						t.Unit = directive.Unit;
						found = true;
					}
				}
			}
			if (!found) {
				diag.Message = "texture unit assignment \"" + directive.Name + "\" does not match any sampler uniform";
				diag.Line = directive.Line;
				return ReportError(inputPath, diag);
			}
		}

		programs.push_back(Death::move(program));
	}

	if (essl100Check) {
		String dump = BuildEssl100Dump(programs);
		std::fwrite(dump.data(), 1, dump.size(), stdout);
		return 0;
	}

	if (hlslDump) {
		String dump = BuildHlslDump(programs);
		std::fwrite(dump.data(), 1, dump.size(), stdout);
		return 0;
	}

	if (vulkanDump) {
		String dump = BuildVulkanDump(programs);
		std::fwrite(dump.data(), 1, dump.size(), stdout);
		return 0;
	}

	if (mslDump) {
		String dump = BuildMslDump(programs);
		std::fwrite(dump.data(), 1, dump.size(), stdout);
		return 0;
	}

	if (checkOnly) {
		String dump;
		for (const ProgramReflection& program : programs) {
			dump += Emitter::BuildCheckDump(*program.Document, program.Variants);
		}
		std::fwrite(dump.data(), 1, dump.size(), stdout);
		return 0;
	}

	// Locate glslang (explicit --glslang, else VULKAN_SDK / PATH) and, when found, embed offline-compiled
	// SPIR-V per stage. When it is unavailable the callback stays empty and the VkVsSpirv/VkFsSpirv fields
	// are emitted as nullptr/0, so the headers still build (the Vulkan backend is then not buildable).
	SpirvCompileFn compileSpirv;
	{
		String glslang;
		if (LocateGlslang(glslangOverride != nullptr ? StringView(glslangOverride) : StringView {}, glslang)) {
			compileSpirv = [glslang](StringView vulkanGlsl, bool vertexStage, SmallVectorImpl<std::uint32_t>& spirv, String& log) {
				return CompileSpirvWithGlslang(glslang, String { vulkanGlsl }, vertexStage, spirv, log);
			};
		}
	}

	// Load d3dcompiler_47 (ships with every Windows) and, when available, embed offline-compiled DXBC
	// bytecode per stage INSTEAD of the HLSL text — the D3D11 backend (desktop and UWP/Xbox) then creates
	// its shader objects straight from the blobs, with no runtime D3DCompile and no on-disk cache. When it
	// is unavailable (non-Windows generation) or --no-dxbc is given, the HLSL sources are embedded as
	// before and the backend falls back to runtime compilation.
	DxbcCompileFn compileDxbc;
	if (!noDxbc) {
		String loadError;
		if (LoadD3DCompiler(loadError)) {
			compileDxbc = [](StringView hlsl, bool vertexStage, SmallVectorImpl<std::uint8_t>& dxbc, String& log) {
				bool ok = CompileHlslToDxbc(String { hlsl }, vertexStage ? "VSMain" : "PSMain",
					vertexStage ? "vs_4_0" : "ps_4_0", dxbc, log);
				if (!ok) {
					// A lowered stage that fails D3DCompile falls back to embedding its HLSL source (the
					// runtime compile will fail the same way, so surface it here; --hlsl-check has details)
					std::fprintf(stderr, "warning: D3DCompile (%s) failed - embedding the HLSL source instead: %s\n",
						vertexStage ? "vs_4_0" : "ps_4_0", FirstLine(log).data());
				}
				return ok;
			};
		}
#if defined(DEATH_TARGET_WINDOWS)
		else {
			std::fprintf(stderr, "warning: %s - DXBC will be omitted (HLSL sources embedded instead)\n", loadError.data());
		}
#endif
	}

	String output;
	BackendArtifacts artifacts;
	if (!Emitter::EmitHeader(programs, ns, inputPath, compileSpirv, compileDxbc, output, artifacts, diag)) {
		return ReportError(inputPath, diag);
	}
	if (!WriteStringToFile(outputPath, output)) {
		std::fprintf(stderr, "error: cannot write output file \"%s\"\n", outputPath);
		return 1;
	}
	// This mode emits ONE shader, so it cannot write the aggregates - they hold every shader's artifacts
	// and rewriting them from one input would drop the rest. The header it just wrote references symbols
	// the committed aggregates already define, so it only links against a matching pair; regenerating a
	// shader whose stage artifacts changed means running --generate-all on a machine that can rebuild them.
	if (!artifacts.D3d11.empty() || !artifacts.Vulkan.empty() || !artifacts.Metal.empty()) {
		std::fprintf(stdout, "note: Direct3D 11 / Vulkan / Metal stage artifacts were not written "
							 "(they live in the aggregates, which only --generate-all produces)\n");
	}
	return 0;
}
