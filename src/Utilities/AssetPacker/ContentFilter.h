#pragma once

#include "../../Main.h"

#include <Containers/StringView.h>

using namespace Death::Containers;

namespace Jazz2::AssetPacker
{
	class ContentFilter
	{
	public:
		/**
			@brief Returns `true` if an entry of a content tree is left out

			@param name			Name of the file or the directory
			@param isDirectory	Whether it's a directory
			@param isTopLevel	Whether it's right in the root of the tree
			@param hasEditor	Whether the build the tree is for has the level editor
		*/
		static bool IsLeftOut(StringView name, bool isDirectory, bool isTopLevel, bool hasEditor);
	};
}
