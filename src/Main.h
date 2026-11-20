#pragma once

// Set default name and version if not provided by the build
/** @brief Application name */
#if !defined(NCINE_APP)
#define NCINE_APP "jazz2"
#endif
/** @brief Application full name */
#if !defined(NCINE_APP_NAME)
#define NCINE_APP_NAME "Jazz² Resurrection"
#endif
/** @brief Application version */
#if !defined(NCINE_VERSION)
#define NCINE_VERSION "3.9.0"
#endif

/**
	@brief Application multiplayer protocol version

	Decides whether a client and a server can play together, independently of @ref NCINE_VERSION. Bump it
	whenever the wire changes incompatibly --- packet types, property types, field layouts or the meaning of a
	field --- and leave it alone for releases that don't touch the wire, so those can still play together.

	It is compared in full, patch included, so a bump of any component is enough to separate two builds and the
	number is free to move independently of the release it ships in. Only ever set here: it is deliberately not
	derived from the build or from Git, so two builds of the same wire format always agree on it.

	A server accepts the clients from @ref NCINE_PROTOCOL_VERSION_MIN to @ref NCINE_PROTOCOL_VERSION_MAX, which
	is only this version unless set otherwise.
*/
#if !defined(NCINE_PROTOCOL_VERSION)
#define NCINE_PROTOCOL_VERSION "3.9.0"
#endif
/**
	@brief Oldest client multiplayer protocol version the server accepts

	Together with @ref NCINE_PROTOCOL_VERSION_MAX it bounds the protocol versions of the clients a server lets
	in, which is checked when a client authenticates. The server announces the range in local discovery, in the
	public server list and in its answer to the authentication, so the server list can tell which servers the
	local client fits, and the client can leave a server whose answer carries no range (a server older than the
	range, which let it in by a looser check of its own).

	Lower it only when the server still talks to the older clients correctly, i.e. the wire changed since then
	only in ways they don't notice, or the server handles those clients differently. Both bounds are inclusive.
*/
#if !defined(NCINE_PROTOCOL_VERSION_MIN)
#define NCINE_PROTOCOL_VERSION_MIN NCINE_PROTOCOL_VERSION
#endif
/**
	@brief Newest client multiplayer protocol version the server accepts

	See @ref NCINE_PROTOCOL_VERSION_MIN. Raise it above @ref NCINE_PROTOCOL_VERSION only for a newer protocol
	version already known to stay compatible with this one.
*/
#if !defined(NCINE_PROTOCOL_VERSION_MAX)
#define NCINE_PROTOCOL_VERSION_MAX NCINE_PROTOCOL_VERSION
#endif

/** @brief Application build year */
#if !defined(NCINE_BUILD_YEAR)
#define NCINE_BUILD_YEAR "2026"
#endif
/** @brief Application package name on Linux */
#if !defined(NCINE_LINUX_PACKAGE)
#define NCINE_LINUX_PACKAGE NCINE_APP_NAME
#endif

// Prefer local version of shared libraries in the Zig build
#if defined(ZIG_BUILD) && defined(__has_include)
#if __has_include("../Shared/Common.h")
#define __HAS_LOCAL_COMMON
#endif
#endif
#ifdef __HAS_LOCAL_COMMON
#include "../Shared/Common.h"
#include "../Shared/Asserts.h"
#else
#include <Common.h>
#include <Asserts.h>
#endif

#include <stdlib.h>

/** @brief Install prefix on Unix systems, usually `"/usr/local"` */
#if (!defined(NCINE_INSTALL_PREFIX) && defined(DEATH_TARGET_UNIX)) || defined(DOXYGEN_GENERATING_OUTPUT)
#define NCINE_INSTALL_PREFIX "/usr/local"
#endif

// Check platform-specific capabilities
/** @brief Whether the current platform supports a gamepad rumble, see @relativeref{nCine,IInputManager::JoystickRumble()} */
#if defined(WITH_SDL2) || defined(WITH_SDL3) || defined(DOXYGEN_GENERATING_OUTPUT)
#define NCINE_HAS_GAMEPAD_RUMBLE
#endif

#define NCINE_HAS_KEYBOARD
/** @brief Whether the current platform has non-fullscreen windows */
#define NCINE_HAS_WINDOWS

#if defined(DEATH_TARGET_EMSCRIPTEN) || defined(DEATH_TARGET_WINDOWS) || defined(DEATH_TARGET_UNIX) || defined(DOXYGEN_GENERATING_OUTPUT)
#define NCINE_HAS_TOUCH_CONTROLS
#endif
/**
	@brief Whether the current platform can drive the RGB lighting of connected devices

	Only Windows (through the Razer Chroma™ SDK) and the web build (through a local bridge) actually do so;
	the other desktop platforms are included because that is where such a device is plugged in, and where
	@relativeref{Jazz2::Input,RgbLights} could gain a backend without the option having to reappear.
*/
#if defined(DEATH_TARGET_WINDOWS) || defined(DEATH_TARGET_EMSCRIPTEN) || defined(DEATH_TARGET_UNIX) || defined(DEATH_TARGET_APPLE) || defined(DOXYGEN_GENERATING_OUTPUT)
#define NCINE_HAS_RGB_LIGHTS
#endif

#if !(defined(DEATH_TARGET_EMSCRIPTEN))
#define NCINE_HAS_WRITABLE_CACHE
#endif

#define NCINE_HAS_RESUMABLE_STATE

/** @brief Function name */
#if defined(__DEATH_CURRENT_FUNCTION)
#define NCINE_CURRENT_FUNCTION __DEATH_CURRENT_FUNCTION
#else
#define NCINE_CURRENT_FUNCTION ""
#endif

#ifndef DOXYGEN_GENERATING_OUTPUT

// Return assert macros
#define RETURN_ASSERT(x)                   \
	do {                                   \
		if DEATH_UNLIKELY (!(x)) {         \
			LOGE("RETURN_ASSERT(" #x ")"); \
			return;                        \
		}                                  \
	} while (false)

// Return false assert macros
#define RETURNF_ASSERT(x)                   \
	do {                                    \
		if DEATH_UNLIKELY (!(x)) {          \
			LOGE("RETURNF_ASSERT(" #x ")"); \
			return false;                   \
		}                                   \
	} while (false)

// Fatal assert macros
#define FATAL_ASSERT(x)                   \
	do {                                  \
		if DEATH_UNLIKELY (!(x)) {        \
			LOGF("FATAL_ASSERT(" #x ")"); \
			DEATH_ASSERT_BREAK();         \
		}                                 \
	} while (false)

#define FATAL_ASSERT_MSG(x, fmt, ...) \
	do {                              \
		if DEATH_UNLIKELY (!(x)) {    \
			LOGF(fmt, ##__VA_ARGS__); \
			DEATH_ASSERT_BREAK();     \
		}                             \
	} while (false)

#endif
