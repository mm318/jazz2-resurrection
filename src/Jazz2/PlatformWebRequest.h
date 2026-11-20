#pragma once

#include "../Main.h"

#include <IO/WebRequest.h>

namespace Jazz2
{
	/**
		@brief Applies whatever the platform needs on a freshly created web request before it is executed

		Nothing anywhere else, and nothing at all on a platform whose TLS stack can find its own trust store.
		The PS Vita cannot: VitaSDK's libcurl verifies against the firmware's own store under `vs0:`, a mount
		point an application's sandbox does not contain - opening it fails with `ENOENT`, not a permission
		error - and the TLS stack linked into the executable is not the system one that iTLS-Enso and friends
		patch, so nothing installed on the console supplies it either. Without this every HTTPS request fails
		to verify, which takes the server list, the update check and script web requests with it. The PSP is
		in the same position for a simpler reason: its libcurl is built on mbedTLS with a Unix default path
		(`/etc/ssl/certs/ca-certificates.crt`) that exists nowhere on a memory stick. The Nintendo 3DS is the
		PSP's case again: devkitPro's libcurl is built on mbedTLS with the same default path, and the console's
		own trust store is not something a homebrew title can read.

		The bundle therefore travels with the content (the CMake packaging fetches it at configure time) and
		is named here rather than inside @relativeref{Death::IO,WebRequest}: where a game keeps its files is
		not something that layer can know, and the handle it already exposes is enough to say so from outside.
	*/
	inline void ApplyPlatformWebRequestOptions(Death::IO::WebRequest& request)
	{
		static_cast<void>(request);
	}
}
