#pragma once

#include "IAudioDevice.h"
#include "IAudioReader.h"
#include "../Base/TimeStamp.h"

#include <memory>

#include <Containers/SmallVector.h>
#include <Containers/StringView.h>

using namespace Death::Containers;

namespace nCine
{
	class IAudioReader;
	class IAudioLoader;

	/** @brief Decoded audio loaded from runtime assets */
	class AudioStream
	{
		friend class AudioStreamPlayer;

	public:
		~AudioStream();

		/** @brief Returns the backend id of the currently playing buffer, or `0` if none */
		inline std::uint32_t GetBufferId() const
		{
			return _currentBufferId;
		}

		/** @brief Returns the number of bytes per sample */
		inline std::int32_t GetBytesPerSample() const
		{
			return _bytesPerSample;
		}
		/** @brief Returns the number of audio channels */
		inline std::int32_t GetChannelCount() const
		{
			return _numChannels;
		}
		/** @brief Returns the sample frequency */
		inline std::int32_t GetFrequency() const
		{
			return _frequency;
		}

		/** @brief Returns the total number of samples, or `-1` if unknown */
		inline std::int32_t GetSampleCount() const
		{
			return _numSamples;
		}
		/** @brief Returns the duration in seconds */
		inline float GetDuration() const
		{
			return _duration;
		}

		/** @brief Returns the total decoded size in bytes, or `-1` if unknown */
		inline std::int32_t GetBufferSize() const
		{
			return (_numSamples == -1 ? -1 : (_numSamples * _numChannels * _bytesPerSample));
		}

		/** @brief Returns the number of samples held by a single streaming buffer */
		std::int32_t GetStreamSampleCount() const;
		/** @brief Returns the size of a single streaming buffer in bytes */
		inline std::int32_t GetStreamBufferSize() const
		{
			return BufferSize;
		}

		/**
		 * @brief Decodes and enqueues new buffers and unqueues processed ones
		 *
		 * @return `false` once the stream has been entirely decoded and played
		 */
		bool Enqueue(std::uint32_t source, bool looping);
		/** @brief Unqueues any remaining buffers and rewinds the reader */
		void Stop(std::uint32_t source);

		/** @brief Returns `true` if the stream is looping */
		inline bool IsLooping() const
		{
			return _isLooping;
		}
		/** @brief Sets whether the stream should loop */
		void SetLooping(bool value);

		/** @brief Decoded audio loaded from runtime assets */
		bool GetModuleState(AudioModuleState& state) const;
		/** @brief Decoded audio loaded from runtime assets */
		bool SetModulePosition(std::uint32_t source, std::int32_t order, std::int32_t row, bool restart, bool looping);
		/** @brief Decoded audio loaded from runtime assets */
		bool SetModuleSpeed(std::int32_t speed);
		/** @brief Decoded audio loaded from runtime assets */
		bool SetModuleTempo(std::int32_t tempo);
		/** @brief Decoded audio loaded from runtime assets */
		bool SlideModuleChannelVolume(std::int32_t channel, float volume, std::int32_t milliseconds);

	private:
		/** @brief Number of buffers used for streaming */
		static const std::int32_t NumBuffers = 3;
		/** @brief Backend buffer queue used for streaming */
		SmallVector<std::uint32_t, NumBuffers> _buffersIds;
		/** @brief Index of the next available buffer, which is also the number of buffers currently queued */
		std::int32_t _nextAvailableBufferIndex;

		/**
		 * @brief Returns the buffers the backend has finished playing to the free list
		 *
		 * @return Number of buffers returned, never more than this object queued
		 */
		std::int32_t UnqueueProcessedBuffers(IAudioDevice& device, std::uint32_t source);

		/** @brief Size in bytes of each streaming buffer */
		// Same on all platforms: only one chunk is decoded ahead, so it must hold more than a frame plays,
		// otherwise the main thread waits on the decoder for every buffer (4 KB on PSP was measurably slower)
		static const std::int32_t BufferSize = 16 * 1024;
		/** @brief Reusable decode request holding the intermediate buffer, executed ahead of time on the decoding thread when available */
		std::shared_ptr<StreamDecodeRequest> _decodeRequest;

		/** @brief Backend id of the currently playing buffer, or 0 if none */
		std::uint32_t _currentBufferId;

		/** @brief Whether the last chunk could be handed to a decoding thread, so this one need not decode it */
		bool _asyncDecodeAvailable;

		/** @brief Sample format of the decoded data */
		IAudioDevice::BufferFormat _format;
		/** @brief Reader that continuously streams decoded data, shared with @ref _decodeRequest */
		std::shared_ptr<IAudioReader> _audioReader;

		/** @brief Frames of the reader held by a queued buffer, see @ref IAudioReader::GetDecodedFrames() */
		struct QueuedChunk
		{
			std::int64_t StartFrame;
			std::int32_t FrameCount;
		};
		/** @brief Queued buffers in the order they play, a ring starting at @ref _firstQueuedChunk */
		QueuedChunk _queuedChunks[NumBuffers] = {};
		std::int32_t _firstQueuedChunk = 0;
		std::int32_t _queuedChunkCount = 0;
		/** @brief When the first queued buffer started to play, as far as can be told */
		TimeStamp _firstChunkStartTime;

		void PushQueuedChunk(std::int32_t bytes);
		void PopQueuedChunks(std::int32_t count);

		/** @brief Number of bytes per sample */
		std::int32_t _bytesPerSample;
		/** @brief Number of channels */
		std::int32_t _numChannels;
		/** @brief Sample frequency */
		std::int32_t _frequency;

		/** @brief Number of samples */
		std::int32_t _numSamples;
		/** @brief Duration in seconds */
		float _duration;

		/** @brief Whether the stream loops */
		bool _isLooping;

		// Private constructors called only by AudioStreamPlayer
		AudioStream();
		explicit AudioStream(StringView filename);

		AudioStream(AudioStream&&);
		AudioStream& operator=(AudioStream&&);
		AudioStream(const AudioStream&) = delete;
		AudioStream& operator=(const AudioStream&) = delete;

		bool LoadFromFile(StringView filename);

		void CreateReader(IAudioLoader& audioLoader);
	};
}
