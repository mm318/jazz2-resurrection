#pragma once

#include <IO/FileSystem.h>
#include <IO/PakFile.h>

namespace Jazz2::Compatibility
{
	/** @brief Writes converted assets to individual files or a platform-specific package */
	class AssetWriter
	{
	public:
		explicit AssetWriter(Death::Containers::StringView directory)
			: _directory(directory), _package(nullptr), _valid(Death::IO::FileSystem::CreateDirectories(directory)) {}

		explicit AssetWriter(Death::IO::PakWriter& package)
			: _package(&package), _valid(package.IsValid()) {}

		bool IsValid() const { return _valid; }
		bool IsDirectory() const { return _package == nullptr; }

		bool AddFile(Death::IO::Stream& source, Death::Containers::StringView path,
			Death::IO::PakPreferredCompression compression = Death::IO::PakPreferredCompression::None)
		{
			if (!_valid) {
				return false;
			}
			if (_package != nullptr) {
				_valid = _package->AddFile(source, path, compression);
				return _valid;
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
		Death::IO::PakWriter* _package;
		bool _valid;
	};
}
