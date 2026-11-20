#include "ContentFilter.h"

#include <Containers/String.h>
#include <Containers/StringUtils.h>
#include <IO/FileSystem.h>

using namespace Death::Containers::Literals;
using namespace Death::IO;

namespace Jazz2::AssetPacker
{
	bool ContentFilter::IsLeftOut(StringView name, bool isDirectory, bool isTopLevel, bool hasEditor)
	{
		if (isDirectory) {
			return (isTopLevel && !hasEditor && name == "Editor"_s);
		}
		// Deploy compiled translations and runtime assets only.
		String extension = StringUtils::lowercase(fs::GetExtension(name));
		return (extension == "po"_s || extension == "zig"_s || extension == "cjs"_s);
	}
}
