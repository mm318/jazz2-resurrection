#include "PreferencesCache.h"
#include "ContentResolver.h"
#include "LevelHandler.h"
#include "Input/ControlScheme.h"
#include "UI/DiscordRpcClient.h"

#include "../nCine/Application.h"
#include "../nCine/I18n.h"
#include "../nCine/Base/FrameStatistics.h"
#include "../nCine/Base/FrameTimer.h"
#include "../nCine/Base/Random.h"
#include "../nCine/Graphics/RHI/RhiFwd.h" // RHI_LOW_POWER_GPU (a header macro, not a build define)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <Containers/SmallVector.h>
#include <Containers/StringConcatenable.h>
#include <Containers/StringStl.h>
#include <Containers/StringUtils.h>
#include <Cpu.h>
#include <Environment.h>
#include <IO/FileSystem.h>
#include <IO/MemoryStream.h>
#include <IO/Compression/DeflateStream.h>

using namespace Death::Containers::Literals;
using namespace Death::IO;
using namespace Death::IO::Compression;
using namespace nCine;

namespace Jazz2
{
	bool PreferencesCache::FirstRun = false;
#if defined(DEATH_TARGET_EMSCRIPTEN)
	bool PreferencesCache::IsStandalone = false;
#endif
	UnlockableEpisodes PreferencesCache::UnlockedEpisodes = UnlockableEpisodes::None;
	RescaleMode PreferencesCache::ActiveRescaleMode = RescaleMode::None;
	bool PreferencesCache::EnableFullscreen = false;
	std::int32_t PreferencesCache::MaxFps = PreferencesCache::UseVsync;
#if defined(WITH_PHYSICS_PROBE)
	bool PreferencesCache::PhysicsProbe = false;
#endif
	PerformanceMetricsLevel PreferencesCache::PerformanceMetrics = PerformanceMetricsLevel::Off;
	bool PreferencesCache::KeepAspectRatioInCinematics = false;
	bool PreferencesCache::ShowPlayerTrails = true;
	bool PreferencesCache::ShowMinimap = true;
	bool PreferencesCache::LowWaterQuality = false;
	bool PreferencesCache::UnalignedViewport = false;
	bool PreferencesCache::PreferVerticalSplitscreen = false;
	bool PreferencesCache::PreferZoomOut = true;
	// Forced off rather than merely defaulted off wherever its condition holds, because it is measurably
	// unaffordable there and has no menu item left to turn it back on (see GraphicsOptionsSection). Load()
	// puts it back to this.
#if defined(RHI_LOW_POWER_GPU)
	// Dithering is a dependent second texture sample over the whole background, which the shader compiles
	// out on a low-power build anyway - leaving the value on would only pick the Dither variant of the
	// background programs and compile a second copy of identical code (see TileMap and MainMenu)
	DitheringScope PreferencesCache::Dithering = DitheringScope::None;
#else
	DitheringScope PreferencesCache::Dithering = DitheringScope::Layers;
#endif
	bool PreferencesCache::BlurEffects = true;
	std::uint8_t PreferencesCache::LightingResolutionPercent = 100;
	std::uint8_t PreferencesCache::RenderingResolutionPercent = 100;
	static constexpr ParticleQuality DefaultParticleQuality = ParticleQuality::High;
	// Named rather than written twice, because Load() has to fall back to the SAME value when the file
	// carries something out of range - a hardcoded `High` there would quietly undo the console default
	// above on exactly the machines it exists for, and survive every later Save()
	ParticleQuality PreferencesCache::Particles = DefaultParticleQuality;
	bool PreferencesCache::EnableReforgedGameplay = true;
	bool PreferencesCache::EnableReforgedCamera = true;
	bool PreferencesCache::EnableReforgedHUD = true;
	bool PreferencesCache::EnableReforgedMainMenu = true;
	bool PreferencesCache::EnableContinuousJump = true;
	bool PreferencesCache::EnableLedgeClimb = true;
	WeaponWheelStyle PreferencesCache::WeaponWheel = WeaponWheelStyle::EnabledWithAmmoCount;
	bool PreferencesCache::ShowAllWeaponWheelSlots = true;
	bool PreferencesCache::SwitchToNewWeapon = true;
	// The web build has a backend but needs a local bridge running, so it stays off until asked for
#if defined(NCINE_HAS_RGB_LIGHTS) && !defined(DEATH_TARGET_EMSCRIPTEN)
	bool PreferencesCache::EnableRgbLights = true;
#else
	bool PreferencesCache::EnableRgbLights = false;
#endif
	bool PreferencesCache::AllowUnsignedScripts = true;

	bool PreferencesCache::TutorialCompleted = false;
	bool PreferencesCache::ResumeOnStart = false;
	bool PreferencesCache::AllowCheats = false;
	bool PreferencesCache::AllowCheatsLives = false;
	bool PreferencesCache::AllowCheatsUnlock = false;
	EpisodeEndOverwriteMode PreferencesCache::OverwriteEpisodeEnd = EpisodeEndOverwriteMode::Always;
	char PreferencesCache::Language[6] {};
	float PreferencesCache::MasterVolume = 0.7f;
	float PreferencesCache::SfxVolume = 0.8f;
	float PreferencesCache::MusicVolume = 0.4f;
	std::int32_t PreferencesCache::AudioSampleRate = 0;
	bool PreferencesCache::ToggleRunAction = false;
	bool PreferencesCache::SwapJumpAndChangeWeaponInMenu = false;
	GamepadType PreferencesCache::GamepadButtonLabels = GamepadType::Xbox;
	std::uint8_t PreferencesCache::GamepadRumble = 1;
	bool PreferencesCache::PlayStationExtendedSupport = false;
	bool PreferencesCache::EnableTouchJoystick = false;
	TouchButtonLayout PreferencesCache::TouchButtons[(std::size_t)TouchButtonSlot::Count] = {};
	std::uint16_t PreferencesCache::SafeArea[(std::size_t)SafeAreaEdge::Count] = {};
	Uuid PreferencesCache::UniquePlayerID;
	Uuid PreferencesCache::UniqueServerID;
	String PreferencesCache::PlayerName;
	std::uint32_t PreferencesCache::PlayerFurColor = 0;
	PlayerColorMode PreferencesCache::PlayerColors = PlayerColorMode::AllLocalPlayers;
	bool PreferencesCache::EnableDiscordIntegration = true;

	String PreferencesCache::_configPath;
	HashMap<String, EpisodeContinuationState> PreferencesCache::_episodeEnd;
	HashMap<String, EpisodeContinuationStateWithLevel> PreferencesCache::_episodeContinue;

	void PreferencesCache::ResetTouchButtons()
	{
		// Default positions derived from HUD constants × (HUD::DefaultRef = 360):
		//	DpadSize=0.37 → 133px, DpadLeft=0.02 → 7px, DpadBottom=0.1 → 36px
		//	ButtonSize=0.172 → 62px, SmallButtonSize=0.098 → 35px
		TouchButtons[(std::size_t)TouchButtonSlot::Dpad] = { Vector2f(7.0f, 36.0f), 1.0f, TouchButtonAnchor::BottomLeft };
		TouchButtons[(std::size_t)TouchButtonSlot::Fire] = { Vector2f(138.0f, 14.0f), 1.0f, TouchButtonAnchor::BottomRight };
		TouchButtons[(std::size_t)TouchButtonSlot::Jump] = { Vector2f(69.0f, 43.0f), 1.0f, TouchButtonAnchor::BottomRight };
		TouchButtons[(std::size_t)TouchButtonSlot::Run] = { Vector2f(0.0f, 58.0f), 1.0f, TouchButtonAnchor::BottomRight };
		TouchButtons[(std::size_t)TouchButtonSlot::ChangeWeapon] = { Vector2f(66.0f, 115.0f), 1.0f, TouchButtonAnchor::BottomRight };
		TouchButtons[(std::size_t)TouchButtonSlot::Menu] = { Vector2f(7.0f, 7.0f), 1.0f, TouchButtonAnchor::TopRight };
		TouchButtons[(std::size_t)TouchButtonSlot::Console] = { Vector2f(7.0f, 7.0f), 1.0f, TouchButtonAnchor::TopLeft };
	}

	void PreferencesCache::ResetSafeArea()
	{
		for (std::size_t i = 0; i < (std::size_t)SafeAreaEdge::Count; i++) {
			SafeArea[i] = 0;
		}
	}

	bool PreferencesCache::IsSafeAreaEnabled()
	{
		for (std::size_t i = 0; i < (std::size_t)SafeAreaEdge::Count; i++) {
			if (SafeArea[i] != 0) {
				return true;
			}
		}
		return false;
	}

	float PreferencesCache::GetSafeAreaInset(SafeAreaEdge edge, Vector2i viewSize)
	{
		std::uint16_t value = SafeArea[(std::size_t)edge];
		if (value == 0) {
			return 0.0f;
		}

		// The left and right edges are a fraction of the width, the top and bottom ones of the height, so a
		// configuration written on one display holds its proportions on another of a different aspect ratio
		float extent = (edge == SafeAreaEdge::Left || edge == SafeAreaEdge::Right ? (float)viewSize.X : (float)viewSize.Y);
		return std::round(extent * (float)value * 0.001f);
	}

	Rectf PreferencesCache::ApplySafeArea(const Rectf& bounds, Vector2i viewSize)
	{
		if (!IsSafeAreaEnabled()) {
			return bounds;
		}

		float left = GetSafeAreaInset(SafeAreaEdge::Left, viewSize);
		float top = GetSafeAreaInset(SafeAreaEdge::Top, viewSize);
		float right = GetSafeAreaInset(SafeAreaEdge::Right, viewSize);
		float bottom = GetSafeAreaInset(SafeAreaEdge::Bottom, viewSize);

		Rectf result = Rectf(bounds.X + left, bounds.Y + top, bounds.W - left - right, bounds.H - top - bottom);

		// A rectangle that was already narrow (a split-screen viewport, or what a large touch layout left of
		// the view) can be narrower than the insets want to take off it, and a negative size would send
		// right-aligned elements off to the wrong side of the screen. Keep it degenerate but valid instead.
		if (result.W < 0.0f) {
			result.X = bounds.X + bounds.W * 0.5f;
			result.W = 0.0f;
		}
		if (result.H < 0.0f) {
			result.Y = bounds.Y + bounds.H * 0.5f;
			result.H = 0.0f;
		}
		return result;
	}

	static void ReadEpisodeContinuationState(Stream& s, EpisodeContinuationState& state)
	{
		state.Flags = EpisodeContinuationFlags(s.ReadValue<std::uint8_t>());
		state.DifficultyAndPlayerType = s.ReadValue<std::uint8_t>();
		state.Lives = s.ReadValue<std::uint8_t>();
		state.Unused1 = s.ReadValue<std::uint8_t>();
		state.Score = s.ReadValueAsLE<std::int32_t>();
		state.Unused2 = s.ReadValueAsLE<std::uint16_t>();
		state.ElapsedMilliseconds = s.ReadValueAsLE<std::uint64_t>();
		for (std::size_t i = 0; i < arraySize(state.Gems); i++) {
			state.Gems[i] = s.ReadValueAsLE<std::int32_t>();
		}
		for (std::size_t i = 0; i < arraySize(state.Ammo); i++) {
			state.Ammo[i] = s.ReadValueAsLE<std::uint16_t>();
		}
		for (std::size_t i = 0; i < arraySize(state.WeaponUpgrades); i++) {
			state.WeaponUpgrades[i] = s.ReadValue<std::uint8_t>();
		}
	}

	static void WriteEpisodeContinuationState(Stream& s, const EpisodeContinuationState& state)
	{
		s.WriteValue<std::uint8_t>(std::uint8_t(state.Flags));
		s.WriteValue<std::uint8_t>(state.DifficultyAndPlayerType);
		s.WriteValue<std::uint8_t>(state.Lives);
		s.WriteValue<std::uint8_t>(state.Unused1);
		s.WriteValueAsLE<std::int32_t>(state.Score);
		s.WriteValueAsLE<std::uint16_t>(state.Unused2);
		s.WriteValueAsLE<std::uint64_t>(state.ElapsedMilliseconds);
		for (std::size_t i = 0; i < arraySize(state.Gems); i++) {
			s.WriteValueAsLE<std::int32_t>(state.Gems[i]);
		}
		for (std::size_t i = 0; i < arraySize(state.Ammo); i++) {
			s.WriteValueAsLE<std::uint16_t>(state.Ammo[i]);
		}
		for (std::size_t i = 0; i < arraySize(state.WeaponUpgrades); i++) {
			s.WriteValue<std::uint8_t>(state.WeaponUpgrades[i]);
		}
	}

	void PreferencesCache::Initialize(AppConfiguration& config)
	{
		bool resetConfig = false;

#if defined(DEATH_TARGET_EMSCRIPTEN)
		auto configDir = "/Persistent"_s;
		fs::MountAsPersistent(configDir);
		_configPath = "/Persistent/Jazz2.config"_s;

		for (int32_t i = 0; i < config.GetArgc(); i++) {
			auto arg = config.GetArgv(i);
			if (arg == "/reset-config"_s) {
				resetConfig = true;
			}
		}
#else
		_configPath = "Jazz2.config"_s;
		bool overrideConfigPath = false;

		for (std::int32_t i = 0; i < config.GetArgc(); i++) {
			auto arg = config.GetArgv(i);
			if (arg == "/config"_s) {
				if (i + 1 < config.GetArgc()) {
					_configPath = config.GetArgv(i + 1);
					overrideConfigPath = true;
					i++;
				}
			} else if (arg == "/reset-config"_s) {
				resetConfig = true;
			}
		}

		// A portable config next to the executable wins over the per-user path chosen below. The Dreamcast,
		// the PlayStation 2 and the Nintendo 64 don't look for one: their whole tree is on the disc or ROM
		// they booted from, so it could never be written back - and a miss there costs the drive a seek and
		// a retry on every boot.
		const bool hasPortableConfig = fs::IsReadableFile(_configPath);

		// If config path is not overriden and portable config doesn't exist, use common path for current user
		if (!overrideConfigPath && !hasPortableConfig) {
#if defined(DEATH_TARGET_UNIX) && defined(NCINE_PACKAGED_CONTENT_PATH)
			_configPath = fs::CombinePath(fs::GetSavePath(NCINE_LINUX_PACKAGE), "Jazz2.config"_s);
#else
			_configPath = fs::CombinePath(fs::GetSavePath("Jazz² Resurrection"_s), "Jazz2.config"_s);
#endif
		}

		auto configDir = fs::GetDirectoryName(_configPath);

		// DEATH_TRACE_LOG_PATH overrides default log path, and on some platforms
		// (Apple, Unix, Windows) it also forces tracing to the file even without
		// using any command-line argument
#if defined(DEATH_TRACE)
#if defined(DEATH_TARGET_APPLE) || defined(DEATH_TARGET_UNIX) || defined(DEATH_TARGET_WINDOWS)
		DEATH_UNUSED bool logFileSpecified = false;
		for (std::int32_t i = 0; i < config.GetArgc(); i++) {
			auto arg = config.GetArgv(i);
			if (arg == "/log:file"_s || arg.hasPrefix("/log:file:"_s)) {
				fs::CreateDirectories(configDir);
				if (arg.size() > "/log:file:"_s.size()) {
					// A path the user named is the one file they asked for, so it is not archived into a
					// "<name>.gz" beside it the way the default log is
					theApplication().AttachTraceTarget(fs::CombinePath(configDir, arg.exceptPrefix("/log:file:"_s)), false);
					logFileSpecified = true;
				}
#if !defined(DEATH_TRACE_LOG_PATH)
				else {
					theApplication().AttachTraceTarget(fs::CombinePath(configDir, "Jazz2.log"_s));
					logFileSpecified = true;
				}
#endif
			}
#if defined(DEATH_TARGET_WINDOWS)
			else if (arg == "/log"_s) {
				theApplication().AttachTraceTarget(Application::ConsoleTarget);
			}
#endif
		}
#if defined(DEATH_TRACE_LOG_PATH)
		if (!logFileSpecified) {
			fs::CreateDirectories(configDir);
			theApplication().AttachTraceTarget(fs::CombinePath(configDir, DEATH_TRACE_LOG_PATH));
		}
#endif
#endif
#endif
#endif

		ControlScheme::Reset();

		// Version of a config file written by an older release, so a copy of it can be kept before
		// the next Save() rewrites it in the current format
		DEATH_UNUSED std::uint8_t upgradedFromVersion = 0;

		// Try to read config file
		if (!resetConfig) {
			auto s = fs::Open(_configPath, FileAccess::Read);
			if (s->GetSize() > 18) {
				std::uint64_t signature = s->ReadValueAsLE<std::uint64_t>();
				std::uint8_t fileType = s->ReadValue<std::uint8_t>();
				std::uint8_t version = s->ReadValue<std::uint8_t>();

				if (signature == 0x2095A59FF0BFBBEF && fileType == ContentFileType::Config && version <= FileVersion) {
					if (version < FileVersion) {
						upgradedFromVersion = version;
					}

					if (version == 1) {
						// Version 1 included compressedSize and decompressedSize, it's not needed anymore
						/*std::int32_t compressedSize =*/s->ReadValue<std::int32_t>();
						/*std::int32_t uncompressedSize =*/s->ReadValue<std::int32_t>();
					}

					DeflateStream uc(*s);

					BoolOptions boolOptions = (BoolOptions)uc.ReadValueAsLE<std::uint64_t>();

#if !defined(DEATH_TARGET_EMSCRIPTEN)
					EnableFullscreen = ((boolOptions & BoolOptions::EnableFullscreen) == BoolOptions::EnableFullscreen);
#endif
					if ((boolOptions & BoolOptions::ShowPerformanceMetrics) == BoolOptions::ShowPerformanceMetrics) {
						PerformanceMetrics = ((boolOptions & BoolOptions::ShowDetailedPerformanceMetrics) == BoolOptions::ShowDetailedPerformanceMetrics
								? PerformanceMetricsLevel::Detailed
								: PerformanceMetricsLevel::Basic);
					} else {
						PerformanceMetrics = PerformanceMetricsLevel::Off;
					}
					KeepAspectRatioInCinematics = ((boolOptions & BoolOptions::KeepAspectRatioInCinematics) == BoolOptions::KeepAspectRatioInCinematics);
					ShowPlayerTrails = ((boolOptions & BoolOptions::ShowPlayerTrails) == BoolOptions::ShowPlayerTrails);
					LowWaterQuality = ((boolOptions & BoolOptions::LowWaterQuality) == BoolOptions::LowWaterQuality);
					UnalignedViewport = ((boolOptions & BoolOptions::UnalignedViewport) == BoolOptions::UnalignedViewport);
					PreferVerticalSplitscreen = ((boolOptions & BoolOptions::PreferVerticalSplitscreen) == BoolOptions::PreferVerticalSplitscreen);
					PreferZoomOut = ((boolOptions & BoolOptions::PreferZoomOut) == BoolOptions::PreferZoomOut);
					Dithering = DitheringScope((std::uint64_t(boolOptions) >> DitheringShift) & 0x03);
					if (Dithering == DitheringScope::None && (boolOptions & BoolOptions::Reserved) == BoolOptions::Reserved) {
						// Dithering of the background layers of older versions
						Dithering = DitheringScope::Layers;
					}
					EnableReforgedGameplay = ((boolOptions & BoolOptions::EnableReforgedGameplay) == BoolOptions::EnableReforgedGameplay);
					EnableLedgeClimb = ((boolOptions & BoolOptions::EnableLedgeClimb) == BoolOptions::EnableLedgeClimb);
					WeaponWheel = ((boolOptions & BoolOptions::EnableWeaponWheel) == BoolOptions::EnableWeaponWheel ? WeaponWheelStyle::Enabled : WeaponWheelStyle::Disabled);
					EnableRgbLights = ((boolOptions & BoolOptions::EnableRgbLights) == BoolOptions::EnableRgbLights);
					AllowUnsignedScripts = ((boolOptions & BoolOptions::AllowUnsignedScripts) == BoolOptions::AllowUnsignedScripts);
					ToggleRunAction = ((boolOptions & BoolOptions::ToggleRunAction) == BoolOptions::ToggleRunAction);
					SwapJumpAndChangeWeaponInMenu = ((boolOptions & BoolOptions::SwapJumpAndChangeWeaponInMenu) == BoolOptions::SwapJumpAndChangeWeaponInMenu);
					EnableDiscordIntegration = ((boolOptions & BoolOptions::EnableDiscordIntegration) == BoolOptions::EnableDiscordIntegration);
					TutorialCompleted = ((boolOptions & BoolOptions::TutorialCompleted) == BoolOptions::TutorialCompleted);
					ResumeOnStart = ((boolOptions & BoolOptions::ResumeOnStart) == BoolOptions::ResumeOnStart);

					if (version >= 3) {
						// These 2 new options needs to be enabled by default
						EnableReforgedHUD = ((boolOptions & BoolOptions::EnableReforgedHUD) == BoolOptions::EnableReforgedHUD);
						EnableReforgedMainMenu = ((boolOptions & BoolOptions::EnableReforgedMainMenu) == BoolOptions::EnableReforgedMainMenu);
					}

					if (version >= 17) {
						EnableReforgedCamera = ((boolOptions & BoolOptions::EnableReforgedCamera) == BoolOptions::EnableReforgedCamera);
					} else {
						// The camera used to follow the gameplay setting, so a configuration written before the
						// split keeps doing that rather than silently changing how the view behaves
						EnableReforgedCamera = EnableReforgedGameplay;
					}

					if (WeaponWheel != WeaponWheelStyle::Disabled && (boolOptions & BoolOptions::ShowWeaponWheelAmmoCount) == BoolOptions::ShowWeaponWheelAmmoCount) {
						WeaponWheel = WeaponWheelStyle::EnabledWithAmmoCount;
					}
					ShowAllWeaponWheelSlots = ((boolOptions & BoolOptions::ShowAllWeaponWheelSlots) == BoolOptions::ShowAllWeaponWheelSlots);

					if ((boolOptions & BoolOptions::SetLanguage) == BoolOptions::SetLanguage) {
						uc.Read(Language, sizeof(Language));
					} else {
						std::memset(Language, 0, sizeof(Language));
					}

					// Bitmask of unlocked episodes, used only if compiled with SHAREWARE_DEMO_ONLY
					UnlockedEpisodes = (UnlockableEpisodes)uc.ReadValueAsLE<std::uint32_t>();

					ActiveRescaleMode = (RescaleMode)uc.ReadValue<std::uint8_t>();

					MasterVolume = uc.ReadValue<std::uint8_t>() / 255.0f;
					SfxVolume = uc.ReadValue<std::uint8_t>() / 255.0f;
					MusicVolume = uc.ReadValue<std::uint8_t>() / 255.0f;

					// v14+: Removed the 4 legacy TouchPadding bytes - convert old format on load
					/*Vector2f legacyLeftPadding, legacyRightPadding;*/
					if (version < 14) {
						/*constexpr float TouchPaddingMultiplier = 0.003f;
						legacyLeftPadding.X  = std::round(uc.ReadValue<std::int8_t>() / (TouchPaddingMultiplier * INT8_MAX));
						legacyLeftPadding.Y  = std::round(uc.ReadValue<std::int8_t>() / (TouchPaddingMultiplier * INT8_MAX));
						legacyRightPadding.X = std::round(uc.ReadValue<std::int8_t>() / (TouchPaddingMultiplier * INT8_MAX));
						legacyRightPadding.Y = std::round(uc.ReadValue<std::int8_t>() / (TouchPaddingMultiplier * INT8_MAX));*/
						uc.ReadValue<std::int32_t>();
					}

					if (version >= 5) {
						GamepadButtonLabels = (GamepadType)uc.ReadValue<std::uint8_t>();
					}

					if (version >= 6) {
						GamepadRumble = uc.ReadValue<std::uint8_t>();
					}

					if (version >= 7) {
						AllowCheats = ((boolOptions & BoolOptions::AllowCheats) == BoolOptions::AllowCheats);
						PlayStationExtendedSupport = ((boolOptions & BoolOptions::PlayStationExtendedSupport) == BoolOptions::PlayStationExtendedSupport);
						SwitchToNewWeapon = ((boolOptions & BoolOptions::SwitchToNewWeapon) == BoolOptions::SwitchToNewWeapon);
						OverwriteEpisodeEnd = (EpisodeEndOverwriteMode)uc.ReadValue<std::uint8_t>();
					}

					if (version >= 10) {
						uc.Read(UniquePlayerID, sizeof(UniquePlayerID));
						std::uint32_t playerNameLength = uc.ReadVariableUint32();
						PlayerName = String(NoInit, playerNameLength);
						uc.Read(PlayerName.data(), playerNameLength);
					} else {
						// Generate a new UUID when upgrading from older version
						Random().Uuid(UniquePlayerID);
					}

					if (version >= 11) {
						uc.Read(UniqueServerID, sizeof(UniqueServerID));
					} else {
						// Generate a new UUID when upgrading from older version
						Random().Uuid(UniqueServerID);
					}

					if (version >= 12) {
						EnableContinuousJump = ((boolOptions & BoolOptions::EnableContinuousJump) == BoolOptions::EnableContinuousJump);
					}

					if (version >= 13) {
						// These 2 new options needs to be enabled by default
						BlurEffects = ((boolOptions & BoolOptions::BlurEffects) == BoolOptions::BlurEffects);
						LightingResolutionPercent = std::clamp(uc.ReadValue<std::uint8_t>(), std::uint8_t(10), std::uint8_t(100));
					}

					// Forced off for any version, there's no menu item to turn it off again on these GPUs
#if defined(RHI_LOW_POWER_GPU)
					Dithering = DitheringScope::None;
#endif

					// Touch button per-slot configuration (v14+)
					EnableTouchJoystick = ((boolOptions & BoolOptions::EnableTouchJoystick) == BoolOptions::EnableTouchJoystick);
					if (version >= 14) {
						for (std::size_t i = 0; i < (std::size_t)TouchButtonSlot::Count; i++) {
							TouchButtons[i].EdgeOffset.X = (float)uc.ReadValueAsLE<std::int16_t>();
							TouchButtons[i].EdgeOffset.Y = (float)uc.ReadValueAsLE<std::int16_t>();
							std::uint8_t scaleByte = uc.ReadValue<std::uint8_t>();
							TouchButtons[i].Scale = 0.5f + (scaleByte / 255.0f) * 2.5f;
							TouchButtons[i].Anchor = (TouchButtonAnchor)uc.ReadValue<std::uint8_t>();
						}
					} else {
						// Convert from legacy left/right padding to per-button layout
						ResetTouchButtons();
						/*TouchButtons[(std::size_t)TouchButtonSlot::Dpad].EdgeOffset.X += legacyLeftPadding.X;
						TouchButtons[(std::size_t)TouchButtonSlot::Dpad].EdgeOffset.Y -= legacyLeftPadding.Y;
						for (std::size_t i = (std::size_t)TouchButtonSlot::Fire; i <= (std::size_t)TouchButtonSlot::ChangeWeapon; i++) {
							TouchButtons[i].EdgeOffset.X -= legacyRightPadding.X;
							TouchButtons[i].EdgeOffset.Y -= legacyRightPadding.Y;
						}*/
					}

					// Controls
					if (version >= 4) {
						auto mappings = ControlScheme::GetAllMappings();

						bool shouldResetBecauseOfOldVersion = (version < 9);
						std::uint32_t playerCount = uc.ReadValue<std::uint8_t>();
						std::uint32_t controlMappingCount = uc.ReadValue<std::uint8_t>();
						for (std::uint32_t i = 0; i < playerCount; i++) {
							for (std::uint32_t j = 0; j < controlMappingCount; j++) {
								std::uint8_t targetCount = uc.ReadValue<std::uint8_t>();
								if (!shouldResetBecauseOfOldVersion && i < ControlScheme::MaxSupportedPlayers && j < (std::uint32_t)PlayerAction::Count) {
									auto& mapping = mappings[i * (std::uint32_t)PlayerAction::Count + j];
									mapping.Targets.clear();

									for (std::uint32_t k = 0; k < targetCount; k++) {
										MappingTarget target = { uc.ReadValueAsLE<std::uint32_t>() };
										mapping.Targets.push_back(target);
									}
								} else {
									uc.Seek(targetCount * sizeof(std::uint32_t), SeekOrigin::Current);
								}
							}
						}

						// Reset primary Menu action, because it's hardcoded
						auto& menuMapping = mappings[(std::uint32_t)PlayerAction::Menu];
						if (menuMapping.Targets.empty()) {
							mappings[(std::int32_t)PlayerAction::Menu].Targets.push_back(ControlScheme::CreateTarget(Keys::Escape));
						}
					} else {
						// Skip old control mapping definitions
						std::uint8_t controlMappingCount = uc.ReadValue<std::uint8_t>();
						uc.Seek(controlMappingCount * sizeof(std::uint32_t), SeekOrigin::Current);
					}

					// Episode End
					std::uint16_t episodeEndSize = uc.ReadValueAsLE<std::uint16_t>();
					std::uint16_t episodeEndCount = uc.ReadValueAsLE<std::uint16_t>();

					for (std::uint32_t i = 0; i < episodeEndCount; i++) {
						std::uint8_t nameLength = uc.ReadValue<std::uint8_t>();
						String episodeName { NoInit, nameLength };
						uc.Read(episodeName.data(), nameLength);

						EpisodeContinuationState state = {};
						if (episodeEndSize == sizeof(EpisodeContinuationState)) {
							ReadEpisodeContinuationState(uc, state);
						} else {
							// Struct has different size, so it's better to skip it
							uc.Seek(episodeEndSize, SeekOrigin::Current);
							state.Flags = EpisodeContinuationFlags::IsCompleted;
						}

						_episodeEnd.emplace(Death::move(episodeName), Death::move(state));
					}

					// Episode Continue
					std::uint16_t episodeContinueSize = uc.ReadValueAsLE<std::uint16_t>();
					std::uint16_t episodeContinueCount = uc.ReadValueAsLE<std::uint16_t>();

					for (std::uint32_t i = 0; i < episodeContinueCount; i++) {
						std::uint8_t nameLength = uc.ReadValue<std::uint8_t>();
						String episodeName { NoInit, nameLength };
						uc.Read(episodeName.data(), nameLength);

						if (episodeContinueSize == sizeof(EpisodeContinuationState)) {
							EpisodeContinuationStateWithLevel stateWithLevel = {};
							nameLength = uc.ReadValue<std::uint8_t>();
							stateWithLevel.LevelName = String(NoInit, nameLength);
							uc.Read(stateWithLevel.LevelName.data(), nameLength);

							ReadEpisodeContinuationState(uc, stateWithLevel.State);
							_episodeContinue.emplace(Death::move(episodeName), Death::move(stateWithLevel));
						} else {
							// Struct has different size, so it's better to skip it
							nameLength = uc.ReadValue<std::uint8_t>();
							uc.Seek(nameLength + episodeContinueSize, SeekOrigin::Current);
						}
					}

					if (version >= 15) {
						ShowMinimap = ((boolOptions & BoolOptions::ShowMinimap) == BoolOptions::ShowMinimap);
						PlayerFurColor = uc.ReadValueAsLE<std::uint32_t>();
						PlayerColors = (PlayerColorMode)uc.ReadValue<std::uint8_t>();
					}

					if (version >= 16) {
						RenderingResolutionPercent = std::clamp(uc.ReadValue<std::uint8_t>(), std::uint8_t(25), std::uint8_t(100));
						std::uint8_t particles = uc.ReadValue<std::uint8_t>();
						Particles = (particles <= (std::uint8_t)ParticleQuality::Ultra ? (ParticleQuality)particles : DefaultParticleQuality);
						// `0` keeps the platform's default; anything outside what a mixer can sensibly run at is a corrupted value
						std::int32_t sampleRate = uc.ReadValueAsLE<std::uint16_t>();
						if (sampleRate == 0 || (sampleRate >= 8000 && sampleRate <= 48000)) {
							AudioSampleRate = sampleRate;
						}
					}

					if (version >= 19) {
						for (std::size_t i = 0; i < (std::size_t)SafeAreaEdge::Count; i++) {
							SafeArea[i] = std::min<std::uint16_t>(uc.ReadValueAsLE<std::uint16_t>(), MaxSafeArea);
						}
					}
				} else {
					// The file is too new or corrupted
					resetConfig = true;
				}
			} else {
				// The file doesn't exist
				resetConfig = true;
			}
		}

		// Done once the stream above is closed. Not overwritten, so the copy stays the one written by the last
		// release of that version. Skipped on a memory card (or the EEPROM), which has room for a single file.
		if (upgradedFromVersion != 0) {
			StringView configPath = _configPath;
			bool isMemoryCard = configPath.hasPrefix("mc0:"_s) || configPath.hasPrefix("mc1:"_s) ||
				configPath.hasPrefix("mca:"_s) || configPath.hasPrefix("mcb:"_s);
			if (!isMemoryCard) {
				String backupPath = format("{}.v{}.bak", configPath, std::int32_t(upgradedFromVersion));
				if (fs::Copy(configPath, backupPath, false)) {
					LOGI("Config file upgraded from version {}, backup saved to \"{}\"", std::int32_t(upgradedFromVersion), backupPath);
				}

				// Only the two most recent backups are kept, ordered by the version that wrote them
				constexpr std::size_t MaxConfigBackups = 2;
				String backupPrefix = fs::GetFileName(configPath) + ".v"_s;
				StringView backupDir = fs::GetDirectoryName(backupPath);
				SmallVector<std::pair<std::int32_t, String>, 4> backups;
				for (auto item : fs::Directory(backupDir.empty() ? "."_s : backupDir, fs::EnumerationOptions::SkipDirectories)) {
					StringView fileName = fs::GetFileName(item);
					if (fileName.size() <= backupPrefix.size() + ".bak"_s.size() ||
						!fileName.hasPrefix(backupPrefix) || !fileName.hasSuffix(".bak"_s)) {
						continue;
					}
					StringView number = fileName.exceptPrefix(backupPrefix).exceptSuffix(".bak"_s);
					if (number.size() > 3) {
						continue;
					}
					std::int32_t backupVersion = 0;
					for (char c : number) {
						if (c < '0' || c > '9') {
							backupVersion = -1;
							break;
						}
						backupVersion = backupVersion * 10 + (c - '0');
					}
					if (backupVersion > 0) {
						backups.emplace_back(backupVersion, String(item));
					}
				}
				if (backups.size() > MaxConfigBackups) {
					std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) {
						return a.first > b.first;
					});
					for (std::size_t i = MaxConfigBackups; i < backups.size(); i++) {
						if (fs::RemoveFile(backups[i].second)) {
							LOGI("Removed old config backup \"{}\"", backups[i].second);
						}
					}
				}
			}
		}

		if (resetConfig) {
			// Config file doesn't exist or reset is requested
			FirstRun = true;
			ResetTouchButtons();
			Random().Uuid(UniquePlayerID);
			Random().Uuid(UniqueServerID);
			PlayerName = GetEffectivePlayerName();
			TryLoadPreferredLanguage();

			fs::CreateDirectories(configDir);

#if defined(DEATH_TARGET_UNIX)
			StringView isSteamDeck = ::getenv("SteamDeck");
			if (isSteamDeck == "1"_s) {
				GamepadButtonLabels = GamepadType::Steam;
			}
#elif defined(DEATH_TARGET_WINDOWS)
			wchar_t envSteamDeck[2] = {};
			DWORD envLength = ::GetEnvironmentVariable(L"SteamDeck", envSteamDeck, 2);
			if (envLength == 1 && envSteamDeck[0] == L'1') {
				GamepadButtonLabels = GamepadType::Steam;
			}
#endif
		}

		// Override some settings by command-line arguments
		for (std::int32_t i = 0; i < config.GetArgc(); i++) {
			auto arg = config.GetArgv(i);
			if (arg == "/cheats"_s) {
				AllowCheats = true;
			} else if (arg == "/cheats-lives"_s) {
				AllowCheatsLives = true;
			} else if (arg == "/cheats-unlock"_s) {
				AllowCheatsUnlock = true;
			} else if (arg == "/fullscreen"_s) {
				EnableFullscreen = true;
			} else if (arg == "/windowed"_s) {
				EnableFullscreen = false;
			} else if (arg == "/no-vsync"_s) {
				// V-Sync can be turned off only with command-line parameter
				if (MaxFps == UseVsync) {
					MaxFps = UnlimitedFps;
				}
			} else if (arg.hasPrefix("/max-fps:"_s)) {
				// Max. FPS can be set only with command-line parameter
				char* end;
				unsigned long paramValue = strtoul(arg.exceptPrefix("/max-fps:"_s).data(), &end, 10);
				if (paramValue > 0) {
					// Floored at the rate the time multiplier is still bounded for - below it `MaxTimeMult`
					// stops keeping up and every trajectory silently runs in slow motion. Taken from
					// FrameTimer rather than written out again, so raising that floor to tighten collision
					// accuracy can't leave the command line accepting a rate it no longer covers.
					MaxFps = std::max<unsigned long>(paramValue, (unsigned long)FrameTimer::LowestFrameRate);
				}
			}
#if defined(WITH_PHYSICS_PROBE)
			else if (arg == "/physics-probe"_s) {
				// Takes over the first player and drives it through a fixed list of movement scenarios,
				// logging one line per tick, so the result can be compared against the same run in the
				// original game (see Tests/PhysicsProbe.h). Only useful together with /log:file: and a
				// pinned /max-fps:.
				PhysicsProbe = true;
			}
#endif
#if !defined(DEATH_TARGET_EMSCRIPTEN)
			else if (arg == "/gpu-workaround"_s) {
				if (i + 1 < config.GetArgc() && config.GetArgv(i + 1) == "fixed-batch-size"_s) {
					config.FixedBatchSize = 10;
				}
			}
#endif
			else if (arg == "/no-rgb"_s) {
				EnableRgbLights = false;
			} else if (arg == "/no-rescale"_s) {
				ActiveRescaleMode = RescaleMode::None;
			} else if (arg == "/mute"_s) {
				MasterVolume = 0.0f;
			} else if (arg == "/reset-controls"_s) {
				ControlScheme::Reset();
			}
#if defined(DEATH_TARGET_EMSCRIPTEN)
			else if (arg == "/standalone"_s) {
				IsStandalone = true;
			}
#endif
		}
	}

	void PreferencesCache::Save()
	{
		// `FirstRun` is true only if config file doesn't exist yet
		FirstRun = false;

		// A memory card's (or the EEPROM's) mount point always exists and has no subdirectories to create
		fs::CreateDirectories(fs::GetDirectoryName(_configPath));

		auto so = fs::Open(_configPath, FileAccess::Write);
		if (!so->IsValid()) {
			return;
		}

		so->WriteValueAsLE<std::uint64_t>(0x2095A59FF0BFBBEF);
		so->WriteValue<std::uint8_t>(ContentFileType::Config);
		so->WriteValue<std::uint8_t>(FileVersion);

		DeflateWriter co(*so);

		BoolOptions boolOptions = BoolOptions::None;
		if (EnableFullscreen)
			boolOptions |= BoolOptions::EnableFullscreen;
		if (PerformanceMetrics != PerformanceMetricsLevel::Off)
			boolOptions |= BoolOptions::ShowPerformanceMetrics;
		if (PerformanceMetrics == PerformanceMetricsLevel::Detailed)
			boolOptions |= BoolOptions::ShowDetailedPerformanceMetrics;
		if (KeepAspectRatioInCinematics)
			boolOptions |= BoolOptions::KeepAspectRatioInCinematics;
		if (ShowPlayerTrails)
			boolOptions |= BoolOptions::ShowPlayerTrails;
		if (LowWaterQuality)
			boolOptions |= BoolOptions::LowWaterQuality;
		if (UnalignedViewport)
			boolOptions |= BoolOptions::UnalignedViewport;
		if (PreferVerticalSplitscreen)
			boolOptions |= BoolOptions::PreferVerticalSplitscreen;
		if (PreferZoomOut)
			boolOptions |= BoolOptions::PreferZoomOut;
		boolOptions |= BoolOptions((std::uint64_t(Dithering) & 0x03) << DitheringShift);
		if (EnableReforgedGameplay)
			boolOptions |= BoolOptions::EnableReforgedGameplay;
		if (EnableReforgedCamera)
			boolOptions |= BoolOptions::EnableReforgedCamera;
		if (EnableLedgeClimb)
			boolOptions |= BoolOptions::EnableLedgeClimb;
		if (WeaponWheel != WeaponWheelStyle::Disabled)
			boolOptions |= BoolOptions::EnableWeaponWheel;
		if (WeaponWheel == WeaponWheelStyle::EnabledWithAmmoCount)
			boolOptions |= BoolOptions::ShowWeaponWheelAmmoCount;
		if (ShowAllWeaponWheelSlots)
			boolOptions |= BoolOptions::ShowAllWeaponWheelSlots;
		if (EnableRgbLights)
			boolOptions |= BoolOptions::EnableRgbLights;
		if (AllowUnsignedScripts)
			boolOptions |= BoolOptions::AllowUnsignedScripts;
		if (ToggleRunAction)
			boolOptions |= BoolOptions::ToggleRunAction;
		if (SwapJumpAndChangeWeaponInMenu)
			boolOptions |= BoolOptions::SwapJumpAndChangeWeaponInMenu;
		if (EnableDiscordIntegration)
			boolOptions |= BoolOptions::EnableDiscordIntegration;
		if (TutorialCompleted)
			boolOptions |= BoolOptions::TutorialCompleted;
		if (Language[0] != '\0')
			boolOptions |= BoolOptions::SetLanguage;
		if (ResumeOnStart)
			boolOptions |= BoolOptions::ResumeOnStart;
		if (EnableReforgedHUD)
			boolOptions |= BoolOptions::EnableReforgedHUD;
		if (EnableReforgedMainMenu)
			boolOptions |= BoolOptions::EnableReforgedMainMenu;
		if (AllowCheats)
			boolOptions |= BoolOptions::AllowCheats;
		if (PlayStationExtendedSupport)
			boolOptions |= BoolOptions::PlayStationExtendedSupport;
		if (SwitchToNewWeapon)
			boolOptions |= BoolOptions::SwitchToNewWeapon;
		if (EnableContinuousJump)
			boolOptions |= BoolOptions::EnableContinuousJump;
		if (BlurEffects)
			boolOptions |= BoolOptions::BlurEffects;
		if (EnableTouchJoystick)
			boolOptions |= BoolOptions::EnableTouchJoystick;
		if (ShowMinimap)
			boolOptions |= BoolOptions::ShowMinimap;
		co.WriteValueAsLE<std::uint64_t>(std::uint64_t(boolOptions));

		if (Language[0] != '\0') {
			co.Write(Language, sizeof(Language));
		}

		// Bitmask of unlocked episodes, used only if compiled with SHAREWARE_DEMO_ONLY
		co.WriteValueAsLE<std::uint32_t>(std::uint32_t(UnlockedEpisodes));

		co.WriteValue<std::uint8_t>(std::uint8_t(ActiveRescaleMode));

		co.WriteValue<std::uint8_t>(std::uint8_t(MasterVolume * 255.0f));
		co.WriteValue<std::uint8_t>(std::uint8_t(SfxVolume * 255.0f));
		co.WriteValue<std::uint8_t>(std::uint8_t(MusicVolume * 255.0f));

		// v14+: No legacy TouchPadding bytes here

		co.WriteValue<std::uint8_t>(std::uint8_t(GamepadButtonLabels));
		co.WriteValue<std::uint8_t>(GamepadRumble);
		co.WriteValue<std::uint8_t>(std::uint8_t(OverwriteEpisodeEnd));

		co.Write(UniquePlayerID, sizeof(UniquePlayerID));
		co.WriteVariableUint32(std::uint32_t(PlayerName.size()));
		co.Write(PlayerName.data(), std::int64_t(PlayerName.size()));

		co.Write(UniqueServerID, sizeof(UniqueServerID));

		co.WriteValue<std::uint8_t>(LightingResolutionPercent);

		// Per-button touch layout (v14+)
		for (std::size_t i = 0; i < (std::size_t)TouchButtonSlot::Count; i++) {
			co.WriteValueAsLE<std::int16_t>(std::int16_t(TouchButtons[i].EdgeOffset.X));
			co.WriteValueAsLE<std::int16_t>(std::int16_t(TouchButtons[i].EdgeOffset.Y));
			co.WriteValue<std::uint8_t>(std::uint8_t(std::clamp((TouchButtons[i].Scale - 0.5f) / 2.5f, 0.0f, 1.0f) * 255.0f));
			co.WriteValue<std::uint8_t>(std::uint8_t(TouchButtons[i].Anchor));
		}

		// Controls
		co.WriteValue<std::uint8_t>(std::uint8_t(ControlScheme::MaxSupportedPlayers));
		co.WriteValue<std::uint8_t>(std::uint8_t(PlayerAction::Count));
		for (std::int32_t i = 0; i < ControlScheme::MaxSupportedPlayers; i++) {
			auto mappings = ControlScheme::GetMappings(i);
			for (std::uint32_t j = 0; j < mappings.size(); j++) {
				const auto& mapping = mappings[j];
				std::uint8_t targetCount = (std::uint8_t)mapping.Targets.size();
				co.WriteValue<std::uint8_t>(targetCount);
				for (std::uint32_t k = 0; k < targetCount; k++) {
					co.WriteValueAsLE<std::uint32_t>(mapping.Targets[k].Data);
				}
			}
		}

		// Episode End
		co.WriteValueAsLE<std::uint16_t>(sizeof(EpisodeContinuationState));
		co.WriteValueAsLE<std::uint16_t>(std::uint16_t(_episodeEnd.size()));

		for (auto& pair : _episodeEnd) {
			co.WriteValue<std::uint8_t>((std::uint8_t)pair.first.size());
			co.Write(pair.first.data(), (std::int64_t)pair.first.size());
			WriteEpisodeContinuationState(co, pair.second);
		}

		// Episode Continue
		co.WriteValueAsLE<std::uint16_t>(sizeof(EpisodeContinuationState));
		co.WriteValueAsLE<std::uint16_t>(std::uint16_t(_episodeContinue.size()));

		for (auto& pair : _episodeContinue) {
			co.WriteValue<std::uint8_t>((std::uint8_t)pair.first.size());
			co.Write(pair.first.data(), (std::int64_t)pair.first.size());

			co.WriteValue<std::uint8_t>((std::uint8_t)pair.second.LevelName.size());
			co.Write(pair.second.LevelName.data(), (std::int64_t)pair.second.LevelName.size());
			WriteEpisodeContinuationState(co, pair.second.State);
		}

		// Player character recolor and recolor scope (v15+)
		co.WriteValueAsLE<std::uint32_t>(PlayerFurColor);
		co.WriteValue<std::uint8_t>((std::uint8_t)PlayerColors);

		// Rendering resolution, particle quality and audio sample rate (v16+)
		co.WriteValue<std::uint8_t>(RenderingResolutionPercent);
		co.WriteValue<std::uint8_t>((std::uint8_t)Particles);
		co.WriteValueAsLE<std::uint16_t>((std::uint16_t)AudioSampleRate);

		// Safe area insets (v19+), a word an edge because the range reaches past what a byte of tenths holds
		for (std::size_t i = 0; i < (std::size_t)SafeAreaEdge::Count; i++) {
			co.WriteValueAsLE<std::uint16_t>(SafeArea[i]);
		}

		co.Dispose();
		so->Dispose();

#if defined(DEATH_TARGET_EMSCRIPTEN)
		fs::SyncToPersistent();
#endif
	}

	void PreferencesCache::ApplyPerformanceMetrics()
	{
		// The frame rate of the basic level comes from the frame timer, which runs anyway; everything else the
		// detailed level shows costs a few timer reads per frame, so it is collected only while it is on screen
		FrameStatistics::SetEnabled(PerformanceMetrics == PerformanceMetricsLevel::Detailed);
	}

	StringView PreferencesCache::GetDirectory()
	{
		return fs::GetDirectoryName(_configPath);
	}

	String PreferencesCache::GetDeviceID()
	{
#if defined(DEATH_TARGET_X86)
		std::int32_t arch = 1;
		Cpu::Features cpuFeatures = Cpu::runtimeFeatures();
		if (cpuFeatures & Cpu::Avx) {
			arch |= 0x400;
		}
		if (cpuFeatures & Cpu::Avx2) {
			arch |= 0x800;
		}
		if (cpuFeatures & Cpu::Avx512f) {
			arch |= 0x1000;
		}
#elif defined(DEATH_TARGET_ARM)
		std::int32_t arch = 2;
		Cpu::Features cpuFeatures = Cpu::runtimeFeatures();
		if (cpuFeatures & Cpu::Neon) {
			arch |= 0x2000;
		}
#elif defined(DEATH_TARGET_POWERPC)
		std::int32_t arch = 3;
#elif defined(DEATH_TARGET_RISCV)
		std::int32_t arch = 5;
#elif defined(DEATH_TARGET_MIPS)
		std::int32_t arch = 6;
#elif defined(DEATH_TARGET_WASM)
		std::int32_t arch = 4;
		Cpu::Features cpuFeatures = Cpu::runtimeFeatures();
		if (cpuFeatures & Cpu::Simd128) {
			arch |= 0x4000;
		}
#else
		std::int32_t arch = 0;
#endif
#if defined(DEATH_TARGET_32BIT)
		arch |= 0x100;
#endif
#if defined(DEATH_TARGET_BIG_ENDIAN)
		arch |= 0x200;
#endif
#if defined(DEATH_TARGET_CYGWIN)
		arch |= 0x200000;
#endif
#if defined(DEATH_TARGET_MINGW)
		arch |= 0x400000;
#endif

		String hostName = Application::GetDeviceHostname();
		StringView systemSuffix = {};

#if defined(DEATH_TARGET_APPLE)
		String appleVersion = Environment::GetAppleVersion();
		String deviceDesc = format("{}|macOS {}{}||5|{}", hostName, appleVersion, systemSuffix, arch);
#elif defined(DEATH_TARGET_EMSCRIPTEN)
		String deviceDesc = format("{}|WASM{}||8|{}", hostName, systemSuffix, arch);
#elif defined(DEATH_TARGET_UNIX)
#if defined(DEATH_TARGET_CLANG)
		arch |= 0x100000;
#endif

		String unixFlavor = Environment::GetUnixFlavor();
		String deviceDesc = format("{}|{}{}||4|{}", hostName, unixFlavor.empty() ? "Unix"_s : StringView(unixFlavor), systemSuffix, arch);
#elif defined(DEATH_TARGET_WINDOWS)
#if defined(DEATH_TARGET_CLANG)
		arch |= 0x100000;
#endif

		auto osVersion = Environment::WindowsVersion;
		bool isWine = Environment::IsWine();
		String deviceDesc = format(isWine ? "{}|Windows {}.{}.{} (Wine){}||3|{}" : "{}|Windows {}.{}.{}{}||3|{}", hostName,
			std::int32_t((osVersion >> 48) & 0xffffu), std::int32_t((osVersion >> 32) & 0xffffu), std::int32_t(osVersion & 0xffffffffu), systemSuffix, arch);
#else
		String deviceDesc = format("{}|{}|||", hostName, systemSuffix.trimmed());
#endif
		return toBase64Url(deviceDesc.begin(), deviceDesc.end());
	}

	String PreferencesCache::GetEffectivePlayerName()
	{
		// Discord display name has the highest priority, then the player name set in the preferences, and finally the system username

		String playerName;
#if defined(DEATH_TARGET_WINDOWS) || defined(DEATH_TARGET_UNIX)
		if (PreferencesCache::EnableDiscordIntegration && UI::DiscordRpcClient::Get().IsSupported()) {
			playerName = UI::DiscordRpcClient::Get().GetUserDisplayName();
		}
#endif
		if (playerName.empty()) {
			playerName = PreferencesCache::PlayerName;
			if (playerName.empty()) {
				playerName = theApplication().GetUserName();
				if (playerName.empty()) {
					// Fallback to "Player XXXX" using the last 4 hex digits of the unique player ID
					playerName = format("Player {:.2x}{:.2x}",
						PreferencesCache::UniquePlayerID[14], PreferencesCache::UniquePlayerID[15]);
				}
			}
		}

		return playerName;
	}

	EpisodeContinuationState* PreferencesCache::GetEpisodeEnd(StringView episodeName, bool createIfNotFound)
	{
		auto it = _episodeEnd.find(String::nullTerminatedView(episodeName));
		if (it == _episodeEnd.end()) {
			// An episode is keyed by its name, which is an identifier rather than a path - and the same episode
			// can be spelled in two cases, because one caller has it from a file name and another from inside a
			// ".j2e" file (see EpisodeSelectSection). The exact lookup above is the fast path; entries are one
			// per episode, so the fallback that ignores case can simply walk them.
			for (auto& pair : _episodeEnd) {
				if (StringUtils::equalsIgnoreCase(pair.first, episodeName)) {
					return &pair.second;
				}
			}
			if (createIfNotFound) {
				return &_episodeEnd.emplace(String(episodeName), EpisodeContinuationState()).first->second;
			} else {
				return nullptr;
			}
		}

		return &it->second;
	}

	EpisodeContinuationStateWithLevel* PreferencesCache::GetEpisodeContinue(StringView episodeName, bool createIfNotFound)
	{
		auto it = _episodeContinue.find(String::nullTerminatedView(episodeName));
		if (it == _episodeContinue.end()) {
			// Keyed without case as well, see GetEpisodeEnd() above
			for (auto& pair : _episodeContinue) {
				if (StringUtils::equalsIgnoreCase(pair.first, episodeName)) {
					return &pair.second;
				}
			}
			if (createIfNotFound) {
				return &_episodeContinue.emplace(String(episodeName), EpisodeContinuationStateWithLevel()).first->second;
			} else {
				return nullptr;
			}
		}

		return &it->second;
	}

	void PreferencesCache::RemoveEpisodeContinue(StringView episodeName)
	{
		if (episodeName.empty() || StringUtils::equalsIgnoreCase(episodeName, "unknown"_s)) {
			return;
		}

		if (_episodeContinue.erase(String::nullTerminatedView(episodeName)) == 0) {
			// The entry may have been stored under a different case, see GetEpisodeEnd() above
			for (auto it = _episodeContinue.begin(); it != _episodeContinue.end(); ++it) {
				if (StringUtils::equalsIgnoreCase(it->first, episodeName)) {
					_episodeContinue.erase(it);
					break;
				}
			}
		}
	}

	void PreferencesCache::RemoveEpisodeEnd(StringView episodeName)
	{
		if (episodeName.empty()) {
			return;
		}

		if (_episodeEnd.erase(String::nullTerminatedView(episodeName)) == 0) {
			for (auto it = _episodeEnd.begin(); it != _episodeEnd.end(); ++it) {
				if (StringUtils::equalsIgnoreCase(it->first, episodeName)) {
					_episodeEnd.erase(it);
					break;
				}
			}
		}
	}

#if defined(SHAREWARE_DEMO_ONLY)
	bool PreferencesCache::IsEpisodeUnlocked(StringView episodeName)
	{
		static const struct
		{
			StringView Name;
			UnlockableEpisodes Flag;
		} LockedEpisodes[] = {
			{ "prince"_s, UnlockableEpisodes::FormerlyAPrince },
			{ "rescue"_s, UnlockableEpisodes::JazzInTime },
			{ "flash"_s, UnlockableEpisodes::Flashback },
			{ "monk"_s, UnlockableEpisodes::FunkyMonkeys },
			{ "xmas98"_s, UnlockableEpisodes::ChristmasChronicles },
			{ "xmas99"_s, UnlockableEpisodes::ChristmasChronicles },
			{ "secretf"_s, UnlockableEpisodes::TheSecretFiles }
		};

		// The case doesn't matter, the same episode can be spelled differently, see GetEpisodeEnd() above
		for (const auto& episode : LockedEpisodes) {
			if (StringUtils::equalsIgnoreCase(episodeName, episode.Name)) {
				return ((UnlockedEpisodes & episode.Flag) != UnlockableEpisodes::None);
			}
		}
		return true;
	}
#endif

	void PreferencesCache::TryLoadPreferredLanguage()
	{
		auto& i18n = I18n::Get();
		auto& resolver = ContentResolver::Get();
		StringView userContentPath = resolver.GetUserContentPath();

		Array<String> languages = I18n::GetPreferredLanguages();
		for (String language : languages) {
			if (!language.empty() && language.size() < sizeof(PreferencesCache::Language)) {
				if (language == "en"_s) {
					break;
				}
				if ((!userContentPath.empty() && i18n.LoadFromFile(fs::CombinePath({ userContentPath, "Translations"_s, String(language + ".mo"_s) }))) ||
					i18n.LoadFromFile(fs::CombinePath({ resolver.GetContentPath(), "Translations"_s, String(language + ".mo"_s) }))) {
					std::memcpy(PreferencesCache::Language, language.data(), language.size());
					std::memset(PreferencesCache::Language + language.size(), '\0', sizeof(PreferencesCache::Language) - language.size());
					break;
				}
			}

			StringView baseLanguage = I18n::TryRemoveLanguageSpecifiers(language);
			if (baseLanguage != language && !baseLanguage.empty() && baseLanguage.size() < sizeof(PreferencesCache::Language)) {
				if (baseLanguage == "en"_s) {
					break;
				}
				if ((!userContentPath.empty() && i18n.LoadFromFile(fs::CombinePath({ userContentPath, "Translations"_s, String(baseLanguage + ".mo"_s) }))) ||
					i18n.LoadFromFile(fs::CombinePath({ resolver.GetContentPath(), "Translations"_s, String(baseLanguage + ".mo"_s) }))) {
					std::memcpy(PreferencesCache::Language, baseLanguage.data(), baseLanguage.size());
					std::memset(PreferencesCache::Language + baseLanguage.size(), '\0', sizeof(PreferencesCache::Language) - baseLanguage.size());
					break;
				}
			}
		}
	}
}