#include "MainApplication.h"
#include "IAppEventHandler.h"
#include "Input/IInputManager.h"
#include "Threading/Thread.h"
#include "../Main.h"

#include <IO/FileSystem.h>

#if (defined(WITH_SDL2) || defined(WITH_SDL3))
#include "Backends/SdlGfxDevice.h"
#include "Backends/SdlInputManager.h"
#elif defined(WITH_GLFW)
#include "Backends/GlfwGfxDevice.h"
#include "Backends/GlfwInputManager.h"
#elif defined(WITH_QT5)
#include "Backends/Qt5GfxDevice.h"
#include "Backends/Qt5InputManager.h"
#endif

// For resizing the swap chain when the window size changes (the call is uniform across the backends;
// it is a no-op on the OpenGL and software backends, which have no backend-owned swap chain)
#include "Graphics/RHI/Rhi.h"

#if defined(DEATH_TARGET_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#elif defined(DEATH_TARGET_UNIX)
#include <pwd.h>
#include <unistd.h>
#elif defined(DEATH_TARGET_WINDOWS)
#include <timeapi.h>
#include <Utf8.h>
#endif

#include "Input/IInputEventHandler.h"

#include <Utf8.h>

#include "tracy.h"

using namespace Death;
using namespace Death::Containers::Literals;
using namespace Death::IO;
#if defined(WITH_SDL2) || defined(WITH_SDL3) || defined(WITH_GLFW) || defined(WITH_QT5)
using namespace nCine::Backends;
#endif

#if defined(DEATH_TARGET_WINDOWS)
#include <shellapi.h>
#include <unknwn.h>

// {4ce576fa-83dc-4F88-951c-9d0782b4e376}
static const GUID CLSID_UIHostNoLaunch = { 0x4CE576FA, 0x83DC, 0x4f88, { 0x95, 0x1C, 0x9D, 0x07, 0x82, 0xB4, 0xE3, 0x76 } };
// {37c994e7_432b_4834_a2f7_dce1f13b834b}
static const GUID IID_ITipInvocation = { 0x37c994e7, 0x432b, 0x4834, { 0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b } };

struct ITipInvocation : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0;
};

static DWORD GetTabTipPathFromRegistry(wchar_t* dstPath, DWORD dstSize)
{
	HKEY hKey;
	if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\Classes\\CLSID\\{054AAE20-4BEA-4347-8A35-64A533254A9D}\\LocalServer32"), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
		return 0;
	}

	DWORD type, cbData;
	if (::RegQueryValueEx(hKey, NULL, NULL, &type, NULL, &cbData) != ERROR_SUCCESS) {
		::RegCloseKey(hKey);
		return 0;
	}

	if ((type != REG_SZ && type != REG_EXPAND_SZ) || dstSize <= (cbData / sizeof(wchar_t))) {
		::RegCloseKey(hKey);
		return 0;
	}

	if (::RegQueryValueEx(hKey, NULL, NULL, NULL, reinterpret_cast<LPBYTE>(dstPath), &cbData) != ERROR_SUCCESS) {
		::RegCloseKey(hKey);
		return 0;
	}

	::RegCloseKey(hKey);
	return DWORD(wcsnlen(dstPath, dstSize));
}
#endif

namespace nCine
{

	Application& theApplication()
	{
		static MainApplication instance;
		return instance;
	}

	int MainApplication::Run(CreateAppEventHandlerDelegate createAppEventHandler, int argc, NativeArgument* argv)
	{
		if (createAppEventHandler == nullptr) {
			return EXIT_FAILURE;
		}

		MainApplication& app = static_cast<MainApplication&>(theApplication());

#if defined(DEATH_TRACE)
		// The sink goes up before any of the platform bring-up below, so all of it can just use LOG*. It is
		// idempotent, so the later PreInitCommon() call stays harmless. The remaining printf()s below write
		// to a channel the sink cannot reach yet, or halt the console.
		app.InitializeTrace();
#endif

#if defined(DEATH_TARGET_WINDOWS)
		// Force set current directory, so everything is loaded correctly, because it's not usually intended
		wchar_t workingDir[fs::MaxPathLength];
		DWORD workingDirLength = ::GetModuleFileNameW(NULL, workingDir, (DWORD)arraySize(workingDir));
		if (workingDirLength > 0) {
			wchar_t* lastSlash = wcsrchr(workingDir, L'\\');
			if (lastSlash == nullptr) {
				lastSlash = wcsrchr(workingDir, L'/');
			}
			if (lastSlash != nullptr) {
				lastSlash++;
				workingDirLength = (DWORD)(lastSlash - workingDir);
				*lastSlash = '\0';
				if (::SetCurrentDirectoryW(workingDir)) {
					LOGI("Using working directory: \"{}\"", Utf8::FromUtf16(workingDir, workingDirLength));
				} else {
					LOGE("Failed to change working directory with error 0x{:.8x}", ::GetLastError());
				}
			}
		}

		timeBeginPeriod(1);
#endif

		app.Init(createAppEventHandler, argc, argv);

#if defined(DEATH_TARGET_EMSCRIPTEN)
		// A zero frame rate selects requestAnimationFrame. This call yields to
		// the browser, so loop timing must not be set during graphics initialization.
		emscripten_set_main_loop(MainApplication::EmscriptenStep, 0, 1);
#else
		while (!app._shouldQuit) {
			app.ProcessStep();
		}
#endif

		app.ShutdownCommon();

#if defined(DEATH_TARGET_WINDOWS)
		timeEndPeriod(1);
#endif
		return EXIT_SUCCESS;
	}

	void MainApplication::Quit()
	{
		Application::Quit();

#if defined(DEATH_TARGET_EMSCRIPTEN)
		// `window.close()` usually works only in PWA/standalone environment
		// clang-format off: embedded JavaScript
		EM_ASM({
			window.close();
		});
		// clang-format on
#endif
	}

	bool MainApplication::EnablePlayStationExtendedSupport(bool enable)
	{
#if defined(WITH_SDL3)
		// SDL3 merged the per-controller HIDAPI rumble hints into SDL_HINT_JOYSTICK_ENHANCED_REPORTS
		SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, enable ? "1" : "0");
		return true;
#elif defined(WITH_SDL2)
		SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, enable ? "1" : "0");
		// SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE inherits value from SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE
		//SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, enable ? "1" : "0");
		return true;
#else
		return false;
#endif
	}

	String MainApplication::GetUserName()
	{
#if defined(DEATH_TARGET_WINDOWS)
		wchar_t userName[64];
		DWORD userNameLength = (DWORD)arraySize(userName);
		if (::GetUserName(userName, &userNameLength) && userNameLength > 0) {
			return Utf8::FromUtf16(userName);
		}
#elif defined(DEATH_TARGET_APPLE)
		StringView userName = ::getenv("USER");
		if (!userName.empty()) {
			return userName;
		}
#elif defined(DEATH_TARGET_UNIX)
		struct passwd* pw = ::getpwuid(::getuid());
		if (pw != nullptr) {
			StringView userName = pw->pw_gecos; // Display name
			if (!userName.empty()) {
				return userName;
			}
			userName = pw->pw_name; // Plain name
			if (!userName.empty()) {
				return userName;
			}
		}

		StringView userName = ::getenv("USER");
		if (!userName.empty()) {
			return userName;
		}
#endif
		return {};
	}

	bool MainApplication::OpenUrl(StringView url)
	{
		if (!url.empty()) {
#if defined(DEATH_TARGET_EMSCRIPTEN)
			// clang-format off: embedded JavaScript
			EM_ASM({
				var url = UTF8ToString($0, $1);
				if (url) window.open(url, '_blank'); }, url.data(), url.size());
			// clang-format on
			return true;
#elif (defined(WITH_SDL2) || defined(WITH_SDL3))
#if SDL_VERSION_ATLEAST(2, 0, 14)
			return SDL_OpenURL(String::nullTerminatedView(url).data()) == 0;
#endif
#endif
		}

		// TODO: Not implemented in GLFW
		return false;
	}

	bool MainApplication::CanShowScreenKeyboard()
	{
#if defined(DEATH_TARGET_WINDOWS)
		return true;
#else
		return false;
#endif
	}

	bool MainApplication::IsScreenKeyboardVisible()
	{
#if defined(DEATH_TARGET_WINDOWS)
		HWND hwnd = ::FindWindowEx(NULL, NULL, L"IPTip_Main_Window", NULL);
		return (hwnd != NULL && ::IsWindowVisible(hwnd));
#else
		return false;
#endif
	}

	std::int32_t MainApplication::GetScreenKeyboardInset()
	{
#if defined(DEATH_TARGET_WINDOWS)
		HWND tip = ::FindWindowEx(NULL, NULL, L"IPTip_Main_Window", NULL);
		HWND window = ::GetActiveWindow();
		RECT tipRect, clientRect;
		if (tip == NULL || window == NULL || !::IsWindowVisible(tip) || !::GetWindowRect(tip, &tipRect) || !::GetClientRect(window, &clientRect)) {
			return 0;
		}
		POINT topLeft = { clientRect.left, clientRect.top };
		POINT bottomRight = { clientRect.right, clientRect.bottom };
		::ClientToScreen(window, &topLeft);
		::ClientToScreen(window, &bottomRight);
		if (tipRect.left >= bottomRight.x || tipRect.right <= topLeft.x || tipRect.top >= bottomRight.y || tipRect.bottom <= topLeft.y) {
			return 0;
		}
		const LONG covered = bottomRight.y - (tipRect.top > topLeft.y ? tipRect.top : topLeft.y);
		const LONG clientHeight = bottomRight.y - topLeft.y;
		if (covered <= 0 || clientHeight <= 0) {
			return 0;
		}
		// The drawable may be larger than the client area with DPI scaling
		return std::int32_t(std::int64_t(covered) * _gfxDevice->GetDrawableHeight() / clientHeight);
#else
		return 0;
#endif
	}

	bool MainApplication::ToggleScreenKeyboard()
	{
#if defined(DEATH_TARGET_WINDOWS)
		if (HideScreenKeyboard()) {
			return true;
		}

		return ShowScreenKeyboard();
#else
		return false;
#endif
	}

	bool MainApplication::ShowScreenKeyboard(Containers::StringView initialText, Containers::Function<void(Containers::StringView)>&& onCompleted)
	{
#if defined(DEATH_TARGET_WINDOWS)
		if (::FindWindowEx(NULL, NULL, L"IPTip_Main_Window", NULL) != NULL) {
			// IID_ITipInvocation is supported only on Windows 10 and later
			ITipInvocation* tip;
			if (::CoCreateInstance(CLSID_UIHostNoLaunch, nullptr,
					CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, IID_ITipInvocation, (void**)&tip) == S_OK) {
				HRESULT hr = tip->Toggle(::GetDesktopWindow());
				tip->Release();
				return SUCCEEDED(hr);
			}
		}

		// Create the process directly if the above fails on Windows 7
#if defined(DEATH_TARGET_32BIT)
		PVOID redirectOldValue = nullptr;
		BOOL redirectSuccess = ::Wow64DisableWow64FsRedirection(&redirectOldValue);
#endif
		bool success = false;
		wchar_t rawPath[MAX_PATH];
		if (GetTabTipPathFromRegistry(rawPath, DWORD(arraySize(rawPath))) == 0) {
			wcscpy_s(rawPath, L"\"%CommonProgramFiles%\\Microsoft Shared\\Ink\\TabTip.exe\"");
		}
		wchar_t path[MAX_PATH];
		DWORD pathLength = ::ExpandEnvironmentStringsW(rawPath, path, DWORD(arraySize(path)));
		if (pathLength > 0 && pathLength < DWORD(arraySize(path))) {
			HINSTANCE hinst = ::ShellExecuteW(NULL, L"open", path,
				nullptr, nullptr, SW_SHOWNORMAL);
			success = ((INT_PTR)hinst > 32);
		}
#if defined(DEATH_TARGET_32BIT)
		if (redirectSuccess) {
			::Wow64RevertWow64FsRedirection(redirectOldValue);
		}
#endif
		return success;
#else
		return false;
#endif
	}

	bool MainApplication::HideScreenKeyboard()
	{
#if defined(DEATH_TARGET_WINDOWS)
		HWND hwnd = ::FindWindowEx(NULL, NULL, L"IPTip_Main_Window", NULL);
		if (hwnd != NULL && ::IsWindowVisible(hwnd)) {
			// IID_ITipInvocation is supported only on Windows 10 and later
			ITipInvocation* tip;
			if (::CoCreateInstance(CLSID_UIHostNoLaunch, nullptr,
					CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, IID_ITipInvocation, (void**)&tip) == S_OK) {
				HRESULT hr = tip->Toggle(::GetDesktopWindow());
				tip->Release();
				return SUCCEEDED(hr);
			}

			// Close the window if the above fails on Windows 7
			::PostMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
			return true;
		}
#endif

		return false;
	}

	void MainApplication::Init(CreateAppEventHandlerDelegate createAppEventHandler, int argc, NativeArgument* argv)
	{
		ZoneScopedC(0x81A861);
#if defined(NCINE_PROFILING)
		_profileStartTime = TimeStamp::Now();
#endif
		_wasSuspended = ShouldSuspend();

		// Only `OnPreInit()` can modify the application configuration
		if (argc > 1) {
#if defined(DEATH_TARGET_WINDOWS)
			_appCfg._argv = Array<String>(ValueInit, argc - 1);
			for (std::int32_t i = 1; i < argc; i++) {
				_appCfg._argv[i - 1] = Utf8::FromUtf16(argv[i]);
			}
#elif defined(DEATH_TARGET_APPLE)
			// Cocoa supports -Key value options which set the user defaults key "Key" to the value "value",
			// skip them here, because they are handled internally by the operating system
			std::int32_t count = 0;
			std::int32_t i = 1;
			while (i < argc) {
				const char* arg = argv[i];
				if (arg[0] == '-' && arg[1] == 'N' && arg[2] == 'S') {
					i += 2; // Skip key and value
				} else {
					count++;
					i++;
				}
			}

			if (count > 0) {
				_appCfg._argv = Array<StringView>(ValueInit, count);
				count = 0;
				i = 1;
				while (i < argc) {
					const char* arg = argv[i];
					if (arg[0] == '-' && arg[1] == 'N' && arg[2] == 'S') {
						i += 2; // Skip key and value
					} else {
						_appCfg._argv[count++] = arg;
						i++;
					}
				}
			}
#else
			_appCfg._argv = Array<StringView>(ValueInit, argc - 1);
			for (std::int32_t i = 1; i < argc; i++) {
				_appCfg._argv[i - 1] = argv[i];
			}
#endif
		}

		PreInitCommon(createAppEventHandler());

		if (_shouldQuit) {
			// If the app was quit from OnPreInit(), skip further initialization
			return;
		}

#if defined(DEATH_DEBUG)
#define INIT_MESSAGE_SUFFIX " in debug configuration"
#else
#define INIT_MESSAGE_SUFFIX ""
#endif

// The graphics backend the build was configured with; the no-graphics path below deliberately names none
#if defined(WITH_GLFW)
#define INIT_MESSAGE_BACKEND " (GLFW)"
#elif defined(WITH_SDL3)
#define INIT_MESSAGE_BACKEND " (SDL3)"
#elif defined(WITH_SDL2)
#define INIT_MESSAGE_BACKEND " (SDL2)"
#else
#define INIT_MESSAGE_BACKEND ""
#endif

// Consoles name the hardware revision they are running on as well (see GetConsoleModelName())
#define LOG_INIT_MESSAGE(backend) LOGI(NCINE_APP_NAME " v" NCINE_VERSION backend " initializing" INIT_MESSAGE_SUFFIX "...")

		if (_appCfg.WithGraphics) {
			LOG_INIT_MESSAGE(INIT_MESSAGE_BACKEND);

			LOGB("Initializing graphics device and input manager...");

			// Graphics device should always be created before the input manager
			IGfxDevice::ContextInfo contextInfo(_appCfg);
			const DisplayMode::VSync vSyncMode = (_appCfg.WithVSync ? DisplayMode::VSync::Enabled : DisplayMode::VSync::Disabled);
#if defined(RHI_USE_FB16) && defined(WITH_RHI_GL)
			// 16-bit screen framebuffer (`NCINE_RHI_USE_FB16`): ask the window system for a 5/6/5 visual with no
			// destination alpha, halving the memory and the present bandwidth of the default framebuffer. The
			// render targets follow through Texture::ColorTargetFormat. The depth/stencil request is unchanged -
			// the 2D pipeline never allocates them for the screen anyway, and a driver is free to pick a deeper
			// buffer than asked for, so this is a request rather than a guarantee.
			DisplayMode displayMode(5, 6, 5, 0, 24, 8, DisplayMode::DoubleBuffering::Enabled, vSyncMode);
#else
			DisplayMode displayMode(8, 8, 8, 8, 24, 8, DisplayMode::DoubleBuffering::Enabled, vSyncMode);
#endif

			const IGfxDevice::WindowMode windowMode(_appCfg.Resolution.X, _appCfg.Resolution.Y, _appCfg.WindowPosition.X,
				_appCfg.WindowPosition.Y, _appCfg.Fullscreen, _appCfg.Resizable, _appCfg.WindowScaling);

#if (defined(WITH_SDL2) || defined(WITH_SDL3))
			_gfxDevice = std::make_unique<SdlGfxDevice>(windowMode, contextInfo, displayMode);
			_inputManager = std::make_unique<SdlInputManager>();
#elif defined(WITH_GLFW)
			_gfxDevice = std::make_unique<GlfwGfxDevice>(windowMode, contextInfo, displayMode);
			_inputManager = std::make_unique<GlfwInputManager>();
#elif defined(WITH_QT5)
			FATAL_ASSERT_MSG(_qt5Widget, "The Qt5 widget has not been assigned");
			_gfxDevice = std::make_unique<Qt5GfxDevice>(windowMode, contextInfo, displayMode, *_qt5Widget);
			_inputManager = std::make_unique<Qt5InputManager>(*_qt5Widget);
#endif
			_gfxDevice->SetWindowTitle(_appCfg.WindowTitle.data());
			if (!_appCfg.WindowIconFilename.empty()) {
				String windowIconFilePath = fs::CombinePath(GetDataPath(), _appCfg.WindowIconFilename);
				if (fs::IsReadableFile(windowIconFilePath)) {
					_gfxDevice->SetWindowIcon(windowIconFilePath);
				}
			}
		} else {
			LOG_INIT_MESSAGE("");

			_gfxDevice = std::make_unique<NullGfxDevice>();
			_inputManager = std::make_unique<NullInputManager>();
		}

#if defined(NCINE_PROFILING)
		_timings[(std::int32_t)Timings::PreInit] = _profileStartTime.GetSecondsSince();
#endif
#if !defined(WITH_QT5)
		// Common initialization on Qt5 is performed later, when OpenGL can be used
		InitCommon();
#endif
	}

	void MainApplication::ProcessStep()
	{
		if (_appCfg.WithGraphics) {
#if defined(WITH_GLFW) || (defined(WITH_SDL2) || defined(WITH_SDL3))
			ProcessEvents();
#elif defined(WITH_QT5GAMEPAD)
			static_cast<Qt5InputManager&>(*_inputManager).UpdateJoystickStates();
#endif
		}

		const bool suspended = ShouldSuspend();
		if (_wasSuspended != suspended) {
			if (suspended) {
				Suspend();
			} else {
				Resume();
			}
			_wasSuspended = suspended;
		}

		if (!suspended) {
			Step();
		}
	}

#if defined(WITH_SDL3)
	void MainApplication::ProcessEvents()
	{
		ZoneScoped;

		SDL_Event event;
#if !defined(DEATH_TARGET_EMSCRIPTEN)
		if (ShouldSuspend()) {
			SDL_WaitEvent(&event);
			SDL_PushEvent(&event);
			// Don't lose any events when resuming
			while (SDL_PollEvent(&event)) {
				SDL_PushEvent(&event);
			}
		}
#endif

		while (SDL_PollEvent(&event)) {
			// SDL3 flattened the SDL2 SDL_WINDOWEVENT/SDL_DISPLAYEVENT umbrellas into one event type per action,
			// so the sub-events are dispatched directly off event.type here instead of event.window.event
			switch (event.type) {
				case SDL_EVENT_QUIT:
					if (SdlInputManager::ShouldQuitOnRequest()) {
						_shouldQuit = true;
					}
					break;
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
					if (SdlGfxDevice::IsOwnWindow(event.window.windowID)) {
						SetFocus(true);
					}
					break;
				case SDL_EVENT_WINDOW_FOCUS_LOST:
					// The focus going over to an auxiliary window of the application isn't a loss of it
					if (SdlGfxDevice::IsOwnWindow(event.window.windowID)) {
						SDL_Window* focused = SDL_GetKeyboardFocus();
						SetFocus(focused != nullptr && SdlGfxDevice::IsOwnWindow(SDL_GetWindowID(focused)));
					}
					break;
				case SDL_EVENT_WINDOW_RESTORED:
				case SDL_EVENT_WINDOW_RESIZED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					if (SdlGfxDevice::IsMainWindow(event.window.windowID)) {
						SDL_Window* windowHandle = SDL_GetWindowFromID(event.window.windowID);
						int logicalWidth = 0, logicalHeight = 0;
						SDL_GetWindowSize(windowHandle, &logicalWidth, &logicalHeight);
						_gfxDevice->_width = logicalWidth;
						_gfxDevice->_height = logicalHeight;
						static_cast<SdlGfxDevice*>(_gfxDevice.get())->SyncFullscreenState();
						// Query the pixel size the way the active backend measures it, then resize the backend
						// swap chain to match (no-op on OpenGL / software); see the SDL2 branch for the rationale
						SdlGfxDevice::QueryDrawableSize(windowHandle, logicalWidth, logicalHeight,
							_gfxDevice->_drawableWidth, _gfxDevice->_drawableHeight);
						RHI::Device::ResizeSwapchain(_gfxDevice->_drawableWidth, _gfxDevice->_drawableHeight);
						ResizeScreenViewport(_gfxDevice->_drawableWidth, _gfxDevice->_drawableHeight);
					}
					break;
				case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
				case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
					// The confirmation of an asynchronous request can arrive after the resize it caused - it has to be
					// recorded here, or the next resize would take it for a change the window system made by itself
					if (SdlGfxDevice::IsMainWindow(event.window.windowID)) {
						static_cast<SdlGfxDevice*>(_gfxDevice.get())->SyncFullscreenState();
					}
					if (_appCfg.WithGraphics) {
						SdlInputManager::ParseEvent(event);
					}
					break;
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					// SDL only turns a close request into SDL_EVENT_QUIT when the closed window is the LAST one, which
					// stops being true as soon as another window exists (ImGui opens one per panel dragged out of the
					// main window), so the main window's close button is honored here. The event still falls through
					// to the input manager below, which is how ImGui closes a window of its own.
					if (SdlGfxDevice::IsMainWindow(event.window.windowID) && SdlInputManager::ShouldQuitOnRequest()) {
						_shouldQuit = true;
					}
					DEATH_FALLTHROUGH
				default:
					if (event.type >= SDL_EVENT_DISPLAY_FIRST && event.type <= SDL_EVENT_DISPLAY_LAST) {
						_gfxDevice->UpdateMonitors();
					} else if (_appCfg.WithGraphics) {
						SdlInputManager::ParseEvent(event);
					}
					break;
			}
		}
	}
#elif defined(WITH_SDL2)
	void MainApplication::ProcessEvents()
	{
		ZoneScoped;

		SDL_Event event;
#if !defined(DEATH_TARGET_EMSCRIPTEN)
		if (ShouldSuspend()) {
			SDL_WaitEvent(&event);
			SDL_PushEvent(&event);
			// Don't lose any events when resuming
			while (SDL_PollEvent(&event)) {
				SDL_PushEvent(&event);
			}
		}
#endif

		while (SDL_PollEvent(&event)) {
			switch (event.type) {
				case SDL_QUIT:
					if (SdlInputManager::ShouldQuitOnRequest()) {
						_shouldQuit = true;
					}
					break;
				case SDL_APP_TERMINATING:
					// iOS (and Android through SDL): the system is about to end the process, nothing follows this
					_shouldQuit = true;
					break;
				case SDL_APP_LOWMEMORY:
					LOGW("The system is running low on memory");
					break;
				case SDL_DISPLAYEVENT:
					_gfxDevice->UpdateMonitors();
					break;
				case SDL_WINDOWEVENT: {
					if (SdlGfxDevice::IsAuxiliaryWindow(event.window.windowID)) {
						// The focus going over between the windows of the application isn't a loss of it
						if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
							SetFocus(true);
						} else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
							SDL_Window* focused = SDL_GetKeyboardFocus();
							SetFocus(focused != nullptr && SdlGfxDevice::IsOwnWindow(SDL_GetWindowID(focused)));
						}
					} else if (SdlGfxDevice::IsMainWindow(event.window.windowID)) {
						switch (event.window.event) {
							case SDL_WINDOWEVENT_FOCUS_GAINED:
								SetFocus(true);
								break;
							case SDL_WINDOWEVENT_FOCUS_LOST: {
								SDL_Window* focused = SDL_GetKeyboardFocus();
								SetFocus(focused != nullptr && SdlGfxDevice::IsOwnWindow(SDL_GetWindowID(focused)));
								break;
							}
							case SDL_WINDOWEVENT_CLOSE:
								// SDL only turns a close into SDL_QUIT when the closed window is the LAST one, which
								// stops being true as soon as another window exists (ImGui opens one per panel
								// dragged out of the main window), so the main window's close button is honored here
								if (SdlInputManager::ShouldQuitOnRequest()) {
									_shouldQuit = true;
								}
								break;
							case SDL_WINDOWEVENT_RESTORED:
							case SDL_WINDOWEVENT_EXPOSED:
							case SDL_WINDOWEVENT_SIZE_CHANGED: {
								SDL_Window* windowHandle = SDL_GetWindowFromID(event.window.windowID);
								if (event.window.event != SDL_WINDOWEVENT_SIZE_CHANGED) {
									// A window started minimized is only exposed, so the size is checked here as well
									SDL_GetWindowSize(windowHandle, &event.window.data1, &event.window.data2);
									std::int32_t drawableWidth = 0, drawableHeight = 0;
									SdlGfxDevice::QueryDrawableSize(windowHandle, event.window.data1, event.window.data2, drawableWidth, drawableHeight);
									if (drawableWidth <= 0 || drawableHeight <= 0 ||
										(drawableWidth == _gfxDevice->_drawableWidth && drawableHeight == _gfxDevice->_drawableHeight)) {
										break;
									}
								}
								_gfxDevice->_width = event.window.data1;
								_gfxDevice->_height = event.window.data2;
								static_cast<SdlGfxDevice*>(_gfxDevice.get())->SyncFullscreenState();
								// Query the pixel size the way the active backend measures it, then resize the
								// backend swap chain to match (no-op on OpenGL / software). The explicit resize
								// is deterministic on Vulkan: some drivers never report OUT_OF_DATE for a
								// window/swap-chain size mismatch, so relying on the present path alone would
								// leave the swap chain stuck at the old size; ResizeSwapchain re-queries the
								// surface caps for the authoritative extent (this value is the hint/fallback).
								SdlGfxDevice::QueryDrawableSize(windowHandle, event.window.data1, event.window.data2,
									_gfxDevice->_drawableWidth, _gfxDevice->_drawableHeight);
								RHI::Device::ResizeSwapchain(_gfxDevice->_drawableWidth, _gfxDevice->_drawableHeight);
								ResizeScreenViewport(_gfxDevice->_drawableWidth, _gfxDevice->_drawableHeight);
								break;
							}
						}
					}
					// Unlike every other event type, these were not reaching the input manager at all - and ImGui
					// drives its platform windows off them (closing, moving, focusing, entering one), so a window
					// it owns could not even be closed. Nothing in the engine's own input handling reads them.
					if (_appCfg.WithGraphics) {
						SdlInputManager::ParseEvent(event);
					}
					break;
				}
				default:
					if (_appCfg.WithGraphics) {
						SdlInputManager::ParseEvent(event);
					}
					break;
			}
		}
	}
#elif defined(WITH_GLFW)
	void MainApplication::ProcessEvents()
	{
		// GLFW does not seem to correctly handle Emscripten focus and blur events
#if !defined(DEATH_TARGET_EMSCRIPTEN)
		SetFocus(GlfwInputManager::HasFocus());
#endif

		if (ShouldSuspend()) {
			glfwWaitEvents();
		} else {
			glfwPollEvents();
		}

		if (_appCfg.WithGraphics) {
			GlfwInputManager::UpdateJoystickStates();
		}
	}
#endif

#if defined(DEATH_TARGET_EMSCRIPTEN)
	void MainApplication::EmscriptenStep()
	{
		static_cast<MainApplication&>(theApplication()).ProcessStep();
	}
#endif
}
