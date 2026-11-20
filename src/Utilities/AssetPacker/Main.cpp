// AssetPacker - converts original Jazz Jackrabbit 2 data into the layout a given platform loads.
//
// Conversion is performed ahead of time. Every target loads the resulting packaged assets.
// This tool is also used by build pipelines to prepare data for deployment.

#include "ContentFilter.h"
#include "FontPacker.h"
#include "FormatVerifier.h"
#include "LevelText.h"
#include "ModuleConverter.h"

#include "../../Main.h"
#include "../../Jazz2/ContentFileTypes.h"
#include "../../Jazz2/Compatibility/AssetConverter.h"
#include "../../Jazz2/Compatibility/J2vRecompressor.h"
#include "../../Jazz2/Compatibility/JJ2Anims.h"
#include "../../Jazz2/EventType.h"
#include "../../nCine/Base/Algorithms.h"

#include <Containers/Array.h>
#include <Containers/DateTime.h>
#include <Containers/SmallVector.h>
#include <Containers/String.h>
#include <Containers/StringUtils.h>
#include <Containers/StringConcatenable.h>
#include <Core/Logger.h>
#include <IO/FileSystem.h>
#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(DEATH_TARGET_EMSCRIPTEN)
#include <emscripten.h>

/** Version of the game this tool was built for, the page shows it when a conversion starts (see Web/assetpacker-worker.js) */
extern "C" EMSCRIPTEN_KEEPALIVE const char* GetGameVersion()
{
	return NCINE_VERSION;
}
#endif

using namespace Death::Containers;
using namespace Death::Containers::Literals;
using namespace Death::IO;
using namespace Death::Trace;
using namespace Jazz2;

#define NCINE_VERSION_s DEATH_PASTE(NCINE_VERSION, _s)
#define NCINE_APP_NAME_s DEATH_PASTE(NCINE_APP_NAME, _s)

namespace
{

	class ConsoleSink : public ITraceSink
	{
	protected:
		void OnTraceFlushed() override
		{
			std::fflush(stdout);
			std::fflush(stderr);
		}

		void OnTraceReceived(TraceLevel level, std::uint64_t timestamp, StringView threadId,
			StringView functionName, StringView content) override
		{
			static_cast<void>(timestamp);
			static_cast<void>(threadId);
			static_cast<void>(functionName);

			FILE* target = (level >= TraceLevel::Error ? stderr : stdout);
			if (level == TraceLevel::Warning) {
				std::fputs("Warning: ", target);
			} else if (level >= TraceLevel::Error) {
				std::fputs("Error: ", target);
			}
			std::fwrite(content.data(), 1, content.size(), target);
			std::fputc('\n', target);
			std::fflush(target);
		}
	};

	/** @brief What the output directory is going to be loaded by */
	;

	/** @brief How the cinematics end up in the output */
	enum class VideoHandling
	{
		Copy,
		Recompress
	};

	/** @brief What the tool was asked to do */
	enum class Command
	{
		/** @brief Convert the original game data, which is what the tool exists for */
		Convert,
		/** @brief Pack an authored bitmap font into the file the game loads */
		PackFont,
		/** @brief Unpack a bitmap font back into the form it is authored in */
		UnpackFont,
		/** @brief Replace the palette indices of an image with the colors they stand for */
		ApplyPalette,
		/** @brief Resolve the colors of an image back to palette indices */
		ToIndices,
		/** @brief Re-encode one cinematic into the container the game plays, optionally downscaling it */
		RecompressVideo,
		/** @brief Translate one of the game's Galaxy Music System modules into a FastTracker II module */
		ConvertMusic,
		/** @brief Check that every level and tile set in a directory is written back unchanged after being read, see @ref AssetPacker::FormatVerifier */
		VerifyFormats,
		/** @brief Write a level as text, for version control, see @ref AssetPacker::LevelText */
		LevelToText,
		/** @brief Make a level of its text again, see @ref AssetPacker::LevelText */
		TextToLevel,
		/** @brief Attack the readers of levels and tile sets with mutated files, see @ref AssetPacker::FormatVerifier::FuzzDirectory() */
		FuzzFormats
	};

	struct Options
	{
		Command Action = Command::Convert;
		String SourcePath, TargetPath, SourceOverride, ContentOverride, ProfileName;
		bool VideoDownscaleSet = false;
		std::int32_t VideoDownscale = 1;
		VideoHandling Videos = VideoHandling::Copy;
		bool AllVideos = false, OriginalsOnly = false, SharewareOnly = false, SkipNonEpisodeLevels = false;
		AssetPacker::FormatVerifier::FuzzOptions Fuzz;
		bool SweepTruncated = false;
	};

	struct SourceLayout
	{
		/** @brief Directory holding `Anims.j2a` and the rest of the original files */
		String OriginalsPath;
		/** @brief The game's own content directory, if one was found beside the originals or named explicitly */
		String ContentPath;
	};

	bool TryParseCommand(StringView value, Command& command)
	{
		if (value == "convert"_s) {
			command = Command::Convert;
		} else if (value == "pack-font"_s) {
			command = Command::PackFont;
		} else if (value == "unpack-font"_s) {
			command = Command::UnpackFont;
		} else if (value == "apply-palette"_s) {
			command = Command::ApplyPalette;
		} else if (value == "to-indices"_s) {
			command = Command::ToIndices;
		} else if (value == "recompress-video"_s) {
			command = Command::RecompressVideo;
		} else if (value == "convert-music"_s) {
			command = Command::ConvertMusic;
		} else if (value == "verify-formats"_s) {
			command = Command::VerifyFormats;
		} else if (value == "level-to-text"_s) {
			command = Command::LevelToText;
		} else if (value == "text-to-level"_s) {
			command = Command::TextToLevel;
		} else if (value == "fuzz-formats"_s) {
			command = Command::FuzzFormats;
		} else {
			return false;
		}
		return true;
	}

	void PrintUsage()
	{
		LOGI("Usage: AssetPacker <command> <source> <target> [options]");
		LOGI("  convert <original-game directory> <target directory>");
		LOGI("    --source=<dir> --content=<dir> --target=desktop|web");
		LOGI("    --originals-only --shareware-only --skip-non-episode-levels --all-videos");
		LOGI("    --video-downscale=N (1-4, default 1)");
		LOGI("  pack-font <image.png> <font> | unpack-font <font> <image.png>");
		LOGI("  apply-palette <source.png> <target.png> | to-indices <source.png> <target.png>");
		LOGI("  recompress-video <source.j2v> <target.j2v> [--video-downscale=N]");
		LOGI("  convert-music <source.j2b> <target.xm>");
		LOGI("  verify-formats <directory> [--sweep-truncated]");
		LOGI("  level-to-text <level.j2l> <level.txt> | text-to-level <level.txt> <level.j2l>");
		LOGI("  fuzz-formats <directory> [findings directory] [--iterations=N] [--seed=N]");
		LOGI("    [--timeout=seconds] [--memory-limit=MB] [--focus-level-texts]");
	}

	/** @brief Works out everything the profile named by `--target=` implies */
	bool ResolveProfile(Options& options)
	{
		if (!options.ProfileName.empty() && options.ProfileName != "desktop"_s && options.ProfileName != "web"_s && options.ProfileName != "emscripten"_s) {
			LOGE("Unsupported target: {}. Use desktop or web", options.ProfileName);
			return false;
		}
		options.Videos = options.VideoDownscaleSet ? VideoHandling::Recompress : VideoHandling::Copy;
		return true;
	}

	bool ParseOptions(ArrayView<const StringView> args, Options& options)
	{
		std::size_t firstArgument = 1;
		if (args.size() > 1 && TryParseCommand(args[1], options.Action)) {
			firstArgument = 2;
		}

		for (std::size_t i = firstArgument; i < args.size(); i++) {
			StringView arg = args[i];
			if (arg.hasPrefix("--target="_s)) {
				options.ProfileName = arg.exceptPrefix("--target="_s);
			} else if (arg.hasPrefix("--source="_s)) {
				options.SourceOverride = arg.exceptPrefix("--source="_s);
			} else if (arg.hasPrefix("--content="_s)) {
				options.ContentOverride = arg.exceptPrefix("--content="_s);
			} else if (arg.hasPrefix("--video-downscale="_s)) {
				options.VideoDownscale = std::atoi(String(arg.exceptPrefix("--video-downscale="_s)).data());
				if (options.VideoDownscale < 1 || options.VideoDownscale > 4) {
					LOGE("Video downscale must be between 1 and 4");
					return false;
				}
				options.VideoDownscaleSet = true;
			} else if (arg == "--originals-only"_s) {
				options.OriginalsOnly = true;
			} else if (arg == "--shareware-only"_s) {
				options.SharewareOnly = true;
			} else if (arg == "--all-videos"_s) {
				options.AllVideos = true;
			} else if (arg == "--skip-non-episode-levels"_s) {
				options.SkipNonEpisodeLevels = true;
			} else if (arg.hasPrefix("--iterations="_s)) {
				options.Fuzz.Iterations = std::max<std::int32_t>(std::atoi(String(arg.exceptPrefix("--iterations="_s)).data()), 1);
			} else if (arg.hasPrefix("--seed="_s)) {
				options.Fuzz.Seed = std::uint64_t(std::strtoull(String(arg.exceptPrefix("--seed="_s)).data(), nullptr, 10));
			} else if (arg.hasPrefix("--timeout="_s)) {
				options.Fuzz.TimeoutSeconds = std::max<std::int32_t>(std::atoi(String(arg.exceptPrefix("--timeout="_s)).data()), 1);
			} else if (arg.hasPrefix("--memory-limit="_s)) {
				options.Fuzz.MemoryLimitMegabytes = std::max<std::int32_t>(std::atoi(String(arg.exceptPrefix("--memory-limit="_s)).data()), 64);
			} else if (arg == "--focus-level-texts"_s) {
				options.Fuzz.FocusLevelTexts = true;
			} else if (arg == "--sweep-truncated"_s) {
				options.SweepTruncated = true;
			} else if (arg == "--help"_s || arg == "-h"_s) {
				return false;
			} else if (arg.hasPrefix("--"_s)) {
				LOGE("Unknown option: {}", arg);
				return false;
			} else if (options.SourcePath.empty()) {
				options.SourcePath = arg;
			} else if (options.TargetPath.empty()) {
				options.TargetPath = arg;
			} else {
				LOGE("Unexpected argument \"{}\"", arg);
				return false;
			}
		}

		// `--source=` names exactly what the positional source argument does, so with it the target is the only
		// positional left and a lone one is that target. Giving both is a mistake rather than an override,
		// because there is no sensible reading in which the tool converts two different source directories.
		if (options.Action == Command::Convert && !options.SourceOverride.empty()) {
			if (options.TargetPath.empty()) {
				options.TargetPath = Death::move(options.SourcePath);
				options.SourcePath = String {};
			} else if (!options.SourcePath.empty()) {
				LOGE("A source directory and \"--source=\" were both given, which name the same thing");
				return false;
			}
		}

		if (!ResolveProfile(options)) {
			return false;
		}

		if (options.Action == Command::Convert) {
			return !options.TargetPath.empty() && (!options.SourcePath.empty() || !options.SourceOverride.empty());
		}

		if (options.Action == Command::VerifyFormats) {
			return !options.SourcePath.empty() && options.TargetPath.empty();
		}
		if (options.Action == Command::FuzzFormats) {
			if (options.TargetPath.empty()) {
				options.TargetPath = "FuzzFindings"_s;
			}
			return !options.SourcePath.empty();
		}
		// The only command that rewrites what it is given, so it is also the only one whose target is optional

		return !options.SourcePath.empty() && !options.TargetPath.empty();
	}

	/** @brief Locates `Anims.j2a`, or the shareware `AnimsSw.j2a`, in the specified directory */
	String FindAnimsFile(StringView path)
	{
		String result = fs::FindPathCaseInsensitive(fs::CombinePath(path, "Anims.j2a"_s));
		if (!fs::IsReadableFile(result)) {
			result = fs::FindPathCaseInsensitive(fs::CombinePath(path, "AnimsSw.j2a"_s));
		}
		return result;
	}

	/** @brief Works out whether the source is a directory of original files or a whole game installation */
	SourceLayout ResolveSourceLayout(StringView sourcePath)
	{
		SourceLayout layout;

		// An installation keeps the original files one level down, so that is where to look first - a directory
		// that has them at the top is taken as they are
		String nested = fs::FindPathCaseInsensitive(fs::CombinePath(sourcePath, "Source"_s));
		if (fs::DirectoryExists(nested) && fs::IsReadableFile(FindAnimsFile(nested))) {
			layout.OriginalsPath = Death::move(nested);

			String content = fs::FindPathCaseInsensitive(fs::CombinePath(sourcePath, "assets"_s));
			if (fs::DirectoryExists(content)) {
				layout.ContentPath = Death::move(content);
			}
		} else {
			layout.OriginalsPath = sourcePath;
		}

		return layout;
	}

	/**
		@brief Copies the game's own content, adding to whatever is already at the target

		What @ref AssetPacker::ContentFilter leaves out for the target isn't copied.

		@param packedDirectories	Entries of the top level that go into the package instead, so aren't copied either
		@param hasEditor			Whether the build the content is for has the level editor
		@param isTopLevel			Whether @p sourcePath is the root of the content
	*/
	bool CopyContentDirectory(StringView sourcePath, StringView targetPath, ArrayView<const StringView> packedDirectories,
		bool hasEditor, bool isTopLevel = true)
	{
		if (!fs::CreateDirectories(targetPath)) {
			return false;
		}

		bool success = true;
		for (auto item : fs::Directory(sourcePath)) {
			StringView itemName = fs::GetFileName(item);
			const bool isDirectory = fs::DirectoryExists(item);
			if (AssetPacker::ContentFilter::IsLeftOut(itemName, isDirectory, isTopLevel, hasEditor)) {
				continue;
			}
			bool packed = false;
			for (StringView packedDirectory : packedDirectories) {
				if (itemName == packedDirectory) {
					packed = true;
					break;
				}
			}
			if (packed) {
				continue;
			}

			String targetItem = fs::CombinePath(targetPath, itemName);
			if (isDirectory) {
				success &= CopyContentDirectory(item, targetItem, {}, hasEditor, false);
				continue;
			}

			// Original game files may be read-only. The staged copies must remain writable for conversion.
			if ((fs::FileExists(targetItem) && !fs::AddPermissions(targetItem, fs::Permission::Write)) ||
				!fs::Copy(item, targetItem) || !fs::AddPermissions(targetItem, fs::Permission::Write)) {
				LOGW("Cannot copy \"{}\" to \"{}\"", item, targetItem);
				success = false;
			}
		}
		return success;
	}

}

namespace
{
	/** @brief Converts the original game data the options name, which is what the tool exists for */
	bool ConvertGameData(const Options& options)
	{
		// `--source=` and the positional argument name the same thing, so both go through the same resolution
		// and a whole game installation is recognized either way
		StringView sourcePath = (options.SourceOverride.empty()
				? StringView(options.SourcePath)
				: StringView(options.SourceOverride));
		if (!fs::DirectoryExists(sourcePath)) {
			LOGE("Source directory \"{}\" does not exist", sourcePath);
			return false;
		}

		SourceLayout layout = ResolveSourceLayout(sourcePath);
		String animsPath = FindAnimsFile(layout.OriginalsPath);
		if (!fs::IsReadableFile(animsPath)) {
			LOGE("Cannot find \"Anims.j2a\" in \"{}\" or in its \"Source\" subdirectory. Make sure a supported Jazz Jackrabbit 2 version is present there.", sourcePath);
			return false;
		}

		// An explicit content directory wins over one found beside the originals, which is what allows the two
		// halves to be kept where they already are - a checkout's "assets" beside a copy of the original game
		if (!options.ContentOverride.empty()) {
			if (!fs::DirectoryExists(options.ContentOverride)) {
				LOGE("Content directory \"{}\" does not exist", options.ContentOverride);
				return false;
			}
			layout.ContentPath = options.ContentOverride;
		}

		// Producing a tree that is loaded as it is and has none of the game's own content is not an error
		// anywhere downstream - it converts, it writes, and the result cannot draw so much as a menu - so it is
		// worth saying out loud here, which is the only place that knows both halves were expected
		if (layout.ContentPath.empty()) {
			LOGW("No game content directory was found beside the original files, so the output will have no fonts, "
				 "no \"Metadata\" and no translations. Pass \"--content=<dir>\" to point at the game's own \"assets\".");
		}

		String outputPath = options.TargetPath;
		if (!fs::CreateDirectories(outputPath))
			return false;
		if (!layout.ContentPath.empty() && !CopyContentDirectory(layout.ContentPath, outputPath, {}, true))
			return false;
		Compatibility::AssetWriter output(outputPath);
		if (!output.IsValid())
			return false;
		Compatibility::JJ2Version version;
		auto result = Compatibility::AssetConverter::ConvertSourceAssets(animsPath, layout.OriginalsPath, output, version);
		if (result != Compatibility::AssetConverter::Result::Success) {
			LOGE("Cannot convert source assets");
			return false;
		}
		SmallVector<String, 0> skippedLevels;
		Compatibility::AssetConverter::ConversionOptions conversionOptions;
		conversionOptions.OriginalsOnly = options.OriginalsOnly;
		conversionOptions.SharewareOnly = options.SharewareOnly;
		conversionOptions.SkipNonEpisodeLevels = options.SkipNonEpisodeLevels;
		// Only music used by the selected levels, menus and cinematics is deployed.
		conversionOptions.CopyUsedMusic = true;
		conversionOptions.SkippedLevels = &skippedLevels;
		Compatibility::AssetConverter::ConvertLevels(layout.OriginalsPath, outputPath, true, conversionOptions);

		if (!skippedLevels.empty()) {
			// Listed rather than only counted, because the list of levels the original game shipped is maintained
			// by hand and this is how a name missing from it shows up
			LOGI("{} levels were skipped:", skippedLevels.size());
			for (String& levelName : skippedLevels) {
				LOGI("  {}", levelName);
			}
		}

		String cinematicsPath = fs::CombinePath(outputPath, "Cinematics"_s);
		if (!fs::CreateDirectories(cinematicsPath))
			return false;
		static const StringView names[] = { "Intro"_s, "Ending"_s, "Logo"_s };
		for (std::size_t i = 0; i < (options.AllVideos ? 3 : 2); i++) {
			String source = fs::FindPathCaseInsensitive(fs::CombinePath(layout.OriginalsPath, String(names[i] + ".j2v"_s)));
			if (!fs::IsReadableFile(source))
				continue;
			String target = fs::CombinePath(cinematicsPath, StringUtils::lowercase(names[i] + ".j2v"_s));
			bool success = options.Videos == VideoHandling::Recompress ? Compatibility::J2vRecompressor::Recompress(source, target, options.VideoDownscale) : fs::Copy(source, target);
			if (!success) {
				LOGE("Cannot prepare cinematic {}", source);
				return false;
			}
		}

		return true;
	}

	/** Runs the tool over the already-decoded UTF-8 command line */
	int RunAssetPacker(ArrayView<const StringView> args)
	{
		ConsoleSink consoleSink;
		Trace::AttachSink(&consoleSink);

		Options options;
		if (!ParseOptions(args, options)) {
			PrintUsage();
			return 1;
		}

		// The asset-level commands work on single files and share nothing with the conversion below
		if (options.Action != Command::Convert) {
			bool success;
			switch (options.Action) {
				case Command::PackFont:
					success = AssetPacker::FontPacker::Pack(options.SourcePath, options.TargetPath);
					break;
				case Command::UnpackFont:
					success = AssetPacker::FontPacker::Unpack(options.SourcePath, options.TargetPath);
					break;
				case Command::ApplyPalette:
					success = AssetPacker::FontPacker::ApplyPalette(options.SourcePath, options.TargetPath);
					break;
				case Command::RecompressVideo:
					success = Compatibility::J2vRecompressor::Recompress(options.SourcePath, options.TargetPath, options.VideoDownscale);
					if (success) {
						LOGI("\"{}\" re-encoded to \"{}\" at 1/{} scale, {} bytes", options.SourcePath, options.TargetPath,
							options.VideoDownscale, fs::GetFileSize(options.TargetPath));
					}
					break;
				case Command::VerifyFormats:
					success = AssetPacker::FormatVerifier::VerifyDirectory(options.SourcePath, options.SweepTruncated);
					break;
				case Command::FuzzFormats:
					success = AssetPacker::FormatVerifier::FuzzDirectory(options.SourcePath, options.TargetPath, options.Fuzz);
					break;
				case Command::LevelToText:
					success = AssetPacker::LevelText::LevelToText(options.SourcePath, options.TargetPath);
					break;
				case Command::TextToLevel:
					success = AssetPacker::LevelText::TextToLevel(options.SourcePath, options.TargetPath);
					break;
				case Command::ConvertMusic:
					success = AssetPacker::ModuleConverter::ConvertJ2bToXm(options.SourcePath, options.TargetPath);
					if (success) {
						LOGI("\"{}\" converted to \"{}\", {} bytes", options.SourcePath, options.TargetPath, fs::GetFileSize(options.TargetPath));
					}
					break;
				default:
					success = AssetPacker::FontPacker::ConvertToIndices(options.SourcePath, options.TargetPath);
					break;
			}
			if (!success) {
				return 1;
			}
			LOGI("Done");
			return 0;
		}

		if (!ConvertGameData(options)) {
			return 1;
		}
		LOGI("Done");
		return 0;
	}
}

#if defined(DEATH_TARGET_WINDOWS)
/**
	Windows transcodes the narrow argv through the active code page, mangling every path it cannot
	represent, so the wide entry point is taken and the arguments are decoded to UTF-8 up front. The
	views are only taken once the strings are all in place, as String stores short ones inline.
*/
int wmain(std::int32_t argc, wchar_t** argv)
{
	SmallVector<String, 8> decoded;
	decoded.reserve(argc);
	for (std::int32_t i = 0; i < argc; i++) {
		decoded.push_back(Death::Utf8::FromUtf16(argv[i]));
	}

	SmallVector<StringView, 8> args;
	args.reserve(decoded.size());
	for (const String& arg : decoded) {
		args.push_back(arg);
	}
	return RunAssetPacker(args);
}
#else
int main(std::int32_t argc, char** argv)
{
	SmallVector<StringView, 8> args;
	args.reserve(argc);
	for (std::int32_t i = 0; i < argc; i++) {
		args.push_back(argv[i]);
	}
	return RunAssetPacker(args);
}
#endif
