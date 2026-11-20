#pragma once

#include <IO/FileSystem.h>

namespace Jazz2::Compatibility
{
	/** @brief Writes converted assets to individual runtime files */
	class AssetWriter
	{
	public:
		explicit AssetWriter(Death::Containers::StringView directory)
			: _directory(directory), _valid(Death::IO::FileSystem::CreateDirectories(directory))
		{
		}

		bool IsValid() const
		{
			return _valid;
		}
		bool AddFile(Death::IO::Stream& source, Death::Containers::StringView path)
		{
			if (!_valid) {
				return false;
			}

			auto targetPath = Death::IO::FileSystem::CombinePath(_directory, path);
			if (!Death::IO::FileSystem::CreateDirectories(Death::IO::FileSystem::GetDirectoryName(targetPath))) {
				_valid = false;
				return false;
			}
			auto target = Death::IO::FileSystem::Open(targetPath, Death::IO::FileAccess::Write);
			const auto remaining = source.GetSize() - source.GetPosition();
			_valid = target->IsValid() && remaining >= 0 && source.CopyTo(*target) == remaining &&
				target->GetSize() == remaining && target->Flush() && target->IsValid();
			return _valid;
		}

	private:
		Death::Containers::String _directory;
		bool _valid;
	};
}
