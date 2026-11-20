#include "nCine/Audio/IAudioLoader.h"
#include "nCine/Audio/IAudioReader.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
	if (argc != 2) {
		return 1;
	}
	auto loader = nCine::IAudioLoader::CreateFromFile(argv[1]);
	if (!loader->HasLoaded() || loader->GetFrequency() != 22050 || loader->GetOriginalFrequency() != 44100 ||
		loader->GetChannelCount() != 1 || loader->GetBytesPerSample() != 2 || loader->GetSampleCount() != 4410) {
		std::fputs("Resampled Vorbis format or original sample rate was lost\n", stderr);
		return 1;
	}
	std::vector<unsigned char> samples(loader->GetBufferSize());
	auto reader = loader->CreateReader();
	if (reader->Read(samples.data(), samples.size()) != samples.size()) {
		std::fputs("Vorbis decoding did not produce the complete effect\n", stderr);
		return 1;
	}
	for (unsigned char sample : samples) {
		if (sample != 0) {
			std::puts("Vorbis sound decoded; original sample rate retained for scripted pitch");
			return 0;
		}
	}
	std::fputs("Decoded sound was silent\n", stderr);
	return 1;
}
