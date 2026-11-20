#pragma once

#include "ConversionProgress.h"
#include "JJ2Version.h"

#include <Containers/Function.h>
#include <Containers/SmallVector.h>
#include <Containers/String.h>
#include <Containers/StringView.h>
#include "AssetWriter.h"

using namespace Death::Containers;
using namespace Death::Containers::Literals;

namespace Jazz2::Compatibility
{
	/**
		@brief Converts original Jazz Jackrabbit 2 data into the formats the game loads

		Conversion is performed ahead of time by the AssetPacker tool. Everything is expressed in terms of an input and an output directory, so it does
		not depend on the resource cache (and through it, on the renderer).
	*/
	class AssetConverter
	{
	public:
		/** @brief Outcome of converting the source assets */
		enum class Result
		{
			Success, /**< Everything was converted */
			CannotWriteTarget, /**< The converted assets could not be written */
			UnsupportedVersion /**< The provided Jazz Jackrabbit 2 version is not supported */
		};

		/**
			@brief What is converted out of the source directory, and what of it is carried over

			Which levels are wanted

			A source directory is usually an installation that has collected levels from everywhere, and most of
			them are not needed for the packaged game. Both of these are off by default, so the whole directory is
			converted unless something asks otherwise.
		*/
		struct ConversionOptions
		{
			/**
				@brief Convert only what the original game and its expansions shipped

				The episodes are recognised by name and every level reachable from one of them is followed, so an
				episode does not lose a level just because it is not in the table of known ones. Levels that
				belong to no episode are matched against a list of the ones the original game came with - see
				`StockStandaloneLevels` in the implementation, and @ref SkippedLevels if it needs correcting.
			*/
			bool OriginalsOnly = false;
			/**
				@brief Convert only what the Shareware Demo came with

				A subset of @ref OriginalsOnly - its one episode and the three multiplayer levels that shipped
				alongside - and it wins if both are set. The custom-levels entry is still kept, so a build that
				lets the player bring their own data (see `ImportSection`) does not lose the menu item for it.

				Note that this cannot narrow the sprite and sound package: they are shared across every episode, so
				what ends up in there is decided by which `Anims.j2a` the source directory has.
			*/
			bool SharewareOnly = false;
			/** @brief Skip levels that belong to no episode (the ones that would land in `Episodes/unknown`) */
			bool SkipNonEpisodeLevels = false;
			/**
				@brief Copy the music the converted levels ask for into a `Music` subdirectory

				For a target whose output *is* the content tree the game loads, which is where it looks for music.

			*/
			bool CopyUsedMusic = false;
			/** @brief Receives the name of every level that was skipped, so a caller can report them */
			SmallVectorImpl<String>* SkippedLevels = nullptr;
		};

		/**
			@brief How the levels of a level pack are converted, and what came out of it, see @ref ConvertLevelPack()

			Levels of no episode go to the episode @ref EpisodePrefix, levels of an episode `e` of the pack to
			`<EpisodePrefix>_e`, so the links between them stay inside the pack.
		*/
		struct LevelPackConversion
		{
			/** @brief A converted level */
			struct Level
			{
				/** @brief Name to load the level by, `episode/level` */
				String Name;
				/** @brief Name shown to the player */
				String DisplayName;
			};

			/** @brief A converted episode */
			struct Episode
			{
				/** @brief Name of the episode */
				String Name;
				/** @brief Name shown to the player */
				String DisplayName;
				/** @brief First level of the episode, without the episode */
				String FirstLevel;
			};

			/** @brief Episode of the levels of no episode, e.g. `+1234` */
			String EpisodePrefix;
			/** @brief Maps a tile set name, lowercase without extension, to the path written into the levels */
			Function<String(StringView)> TileSetPath;
			/** @brief Maps a music file name, lowercase with extension, to the name written into the levels */
			Function<String(StringView)> MusicPath;
			/** @brief Whether the scripts of the levels are copied next to them */
			bool CopyScripts = true;

			/** @brief Levels that were converted */
			SmallVector<Level, 0> Levels;
			/** @brief Episodes that were converted */
			SmallVector<Episode, 0> Episodes;
			/** @brief Original names of the tile sets the levels use, they're converted by the caller */
			SmallVector<String, 0> UsedTileSets;
			/** @brief Original names of the music files the levels use */
			SmallVector<String, 0> UsedMusic;
		};

		/** @brief Converts Anims.j2a and optional Data.j2d into individual runtime files */
		static Result ConvertSourceAssets(StringView animsPath, StringView sourcePath, StringView targetPath,
			JJ2Version& version, ConversionProgress progress = {});
		/** @brief Converts the same assets using the supplied directory or package writer */
		static Result ConvertSourceAssets(StringView animsPath, StringView sourcePath,
			AssetWriter& output, JJ2Version& version, ConversionProgress progress = {});

		/** @brief Converts every episode, level and used tileset into the output directory */
		static void ConvertLevels(StringView sourcePath, StringView targetPath, bool recreateAll, ConversionProgress progress = {});
		/** @brief Converts the episodes, levels and used tilesets that @p options allow into the output directory */
		static void ConvertLevels(StringView sourcePath, StringView targetPath, bool recreateAll, const ConversionOptions& options, ConversionProgress progress = {});
		/**
		 * @brief Converts the episodes and levels of a level pack into the `Episodes` directory of the output directory
		 *
		 * Every level gets a preview. The tile sets aren't converted, only listed in @ref LevelPackConversion::UsedTileSets.
		 */
		static void ConvertLevelPack(StringView sourcePath, StringView targetPath, LevelPackConversion& pack, ConversionProgress progress = {});

	private:
		static void ConvertLevelsInternal(StringView sourcePath, StringView targetPath, bool recreateAll, const ConversionOptions& options,
			LevelPackConversion* pack, ConversionProgress progress);
	};
}
