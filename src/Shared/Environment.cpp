#include "Environment.h"

#if defined(DEATH_TARGET_EMSCRIPTEN)
#include <emscripten.h>
#elif defined(DEATH_TARGET_WINDOWS)
#pragma comment(lib, "psapi")
#
#include <psapi.h>
#include <strsafe.h>
#elif defined(DEATH_TARGET_APPLE) || defined(DEATH_TARGET_UNIX)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#if defined(DEATH_TARGET_APPLE)
#include <sys/sysctl.h>
#endif
#endif

namespace Death
{
	namespace Environment
	{
		//###==##====#=====--==~--~=~- --- -- -  -  -   -

		static ElevationState _currentElevation = ElevationState::Unknown;

		ElevationState GetCurrentElevation() noexcept
		{
			if (_currentElevation != ElevationState::Unknown) {
				return _currentElevation;
			}

#if defined(DEATH_TARGET_WINDOWS)
			if (Environment::IsWindowsVista()) {
				TOKEN_ELEVATION elevation;
				DWORD returnLength;
				if (::GetTokenInformation(::GetCurrentProcessToken(), TokenElevation, &elevation, sizeof(elevation), &returnLength) && returnLength == sizeof(elevation)) {
					_currentElevation = (elevation.TokenIsElevated ? ElevationState::Full : ElevationState::Limited);
				}
			} else {
				_currentElevation = ElevationState::Full;
			}
#elif defined(DEATH_TARGET_APPLE) || defined(DEATH_TARGET_UNIX)
			uid_t uid = getuid(), euid = geteuid();
			// Check for "suid-root" or "root" as effective user
			_currentElevation = (uid < 0 || uid != euid || euid == 0 ? ElevationState::Full : ElevationState::Limited);
#endif
			return _currentElevation;
		}

#if defined(DEATH_TARGET_EMSCRIPTEN)
		bool IsEmbedded() noexcept
		{
			// clang-format off: embedded JavaScript
			return EM_ASM_INT({
				return window.self !== window.top;
			});
			// clang-format on
		}
#endif

		bool IsSandboxed() noexcept
		{
#if defined(DEATH_TARGET_EMSCRIPTEN)
			return true;
#elif defined(DEATH_TARGET_APPLE)
			return std::getenv("APP_SANDBOX_CONTAINER_ID");
#else
			return false;
#endif
		}

#if defined(DEATH_TARGET_APPLE)
		Containers::String GetAppleVersion()
		{
			FILE* fp = ::fopen("/System/Library/CoreServices/SystemVersion.plist", "r");
			if (fp == nullptr) {
				return {};
			}

			Containers::String result;

			// Look for the line containing the macOS version information
			char line[128];
			while (::fgets(line, sizeof(line), fp) != nullptr) {
				if (strstr(line, "<key>ProductVersion</key>") != nullptr) {
					break;
				}
			}

			// Read the macOS version from the corresponding value tag
			if (::fgets(line, sizeof(line), fp) != nullptr) {
				char* versionStart = strchr(line, '>');
				if (versionStart != nullptr) {
					char* versionEnd = strchr(versionStart, '<');
					if (versionEnd != nullptr) {
						result = Containers::String(versionStart + 1, versionEnd - versionStart - 1);
					}
				}
			}

			::fclose(fp);

			return result;
		}

		Containers::String GetAppleDeviceModel()
		{
			// "hw.machine" is the device model on iOS ("iPhone14,5"), "hw.model" the one on macOS ("MacBookPro18,3")
			const char* key = "hw.model";
			char buffer[128];
			std::size_t size = sizeof(buffer);
			if (::sysctlbyname(key, buffer, &size, nullptr, 0) != 0 || size == 0) {
				return {};
			}
			return Containers::String(buffer, ::strnlen(buffer, size));
		}
#endif

#if defined(DEATH_TARGET_UNIX)
		Containers::String GetUnixFlavor()
		{
			FILE* fp = ::fopen("/etc/os-release", "r");
			if (fp == nullptr) {
				fp = ::fopen("/usr/lib/os-release", "r");
				if (fp == nullptr) {
					return {};
				}
			}

			Containers::String result;

			char* line = nullptr;
			std::size_t length = 0;
			ssize_t readChars;
			while ((readChars = ::getline(&line, &length, fp)) != -1) {
				if (strncmp(line, "PRETTY_NAME=", sizeof("PRETTY_NAME=") - 1) == 0) {
					char* versionStart = line + sizeof("PRETTY_NAME=") - 1;
					bool startsWithBackslash = false;
					if (versionStart[0] == '\\' && versionStart[1] == '"') {
						versionStart += 2;
						startsWithBackslash = true;
					} else if (versionStart[0] == '"') {
						versionStart += 1;
					}
					char* versionEnd = line + readChars - 1;
					while (versionStart <= versionEnd && (versionEnd[0] == '\0' || versionEnd[0] == '\r' || versionEnd[0] == '\n' || versionEnd[0] == '"')) {
						versionEnd--;
					}
					if (startsWithBackslash) {
						if (versionEnd[0] == '"' && versionEnd[-1] == '\\') {
							versionEnd -= 2;
						}
					} else {
						if (versionEnd[0] == '"') {
							versionEnd -= 1;
						}
					}

					char* versionShortEnd = versionEnd;
					while (true) {
						if (versionStart >= versionShortEnd) {
							versionShortEnd = nullptr;
							break;
						}
						if (versionShortEnd[0] == '(') {
							versionShortEnd--;
							while (versionStart < versionShortEnd && versionShortEnd[0] == ' ') {
								versionShortEnd--;
							}
							break;
						}
						versionShortEnd--;
					}

					if (versionShortEnd != nullptr) {
						versionEnd = versionShortEnd;
					}
					if (versionStart < versionEnd) {
						result = Containers::String(versionStart, versionEnd - versionStart + 1);
					}
					break;
				}
			}

			if (line != nullptr) {
				std::free(line);
			}
			::fclose(fp);

			return result;
		}
#endif

#if defined(DEATH_TARGET_WINDOWS)
		static std::uint64_t GetWindowsVersion()
		{
			using _RtlGetNtVersionNumbers = void(WINAPI*)(LPDWORD major, LPDWORD minor, LPDWORD build);

			HMODULE hNtdll = ::GetModuleHandle(L"ntdll.dll");
			if (hNtdll == nullptr) {
				return 0;
			}

			_RtlGetNtVersionNumbers RtlGetNtVersionNumbers = (_RtlGetNtVersionNumbers)::GetProcAddress(hNtdll, "RtlGetNtVersionNumbers");
			if (RtlGetNtVersionNumbers == nullptr) {
				return 0;
			}

			DWORD major, minor, build;
			RtlGetNtVersionNumbers(&major, &minor, &build);
			build &= ~0xF0000000;

			return (build & 0xFFFFFFFFull) | ((minor & 0xFFFFull) << 32) | ((major & 0xFFFFull) << 48);
		}

		const std::uint64_t WindowsVersion = GetWindowsVersion();

		bool IsWine() noexcept
		{
			HMODULE hNtdll = ::GetModuleHandle(L"ntdll.dll");
			return (hNtdll != nullptr && ::GetProcAddress(hNtdll, "wine_get_host_version") != nullptr);
		}
#endif

	}
}