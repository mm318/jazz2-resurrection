#pragma once

/** @file
	@brief Class @ref Death::Threading::Spinlock
*/

#include "../Common.h"

#include <atomic>

#if defined(DEATH_TARGET_WINDOWS)
#include "../CommonWindows.h"
#define __DEATH_SPINLOCK_CAN_YIELD
#elif defined(DEATH_TARGET_APPLE) || defined(DEATH_TARGET_UNIX) && !(defined(DEATH_TARGET_CONSTRAINED_MEMORY))
#include <sched.h>
#define __DEATH_SPINLOCK_CAN_YIELD
#endif

namespace Death
{
	namespace Threading
	{
		//###==##====#=====--==~--~=~- --- -- -  -  -   -

		/**
		@brief Lightweight spinlock implementation
	*/
		class Spinlock
		{
		public:
			Spinlock() = default;

			Spinlock(Spinlock const&) = delete;
			Spinlock& operator=(Spinlock const&) = delete;

			/** @brief Acquires the lock, busy-waiting until it becomes available */
			void lock() noexcept
			{
				std::uint32_t spins = 0;
				do {
					while (_state.load(std::memory_order_relaxed) == State::Locked) {
						Backoff(spins);
					}
				} while (_state.exchange(State::Locked, std::memory_order_acquire) == State::Locked);
			}

			/** @brief Releases the lock */
			void unlock() noexcept
			{
				_state.store(State::Free, std::memory_order_release);
			}

		private:
			static void Backoff(std::uint32_t& spins) noexcept
			{
#if defined(__DEATH_SPINLOCK_CAN_YIELD)
				// The holder may have been preempted
				if (++spins >= 64) {
					spins = 0;
#if defined(DEATH_TARGET_WINDOWS)
					::SwitchToThread();
#else
					sched_yield();
#endif
					return;
				}
#else
				(void)spins;
#endif
				// Spin-wait hint
#if defined(DEATH_TARGET_WINDOWS)
				YieldProcessor();
#elif (defined(DEATH_TARGET_GCC) || defined(DEATH_TARGET_CLANG)) && defined(DEATH_TARGET_X86)
				__builtin_ia32_pause();
#elif (defined(DEATH_TARGET_GCC) || defined(DEATH_TARGET_CLANG)) && (defined(__aarch64__) || (defined(__ARM_ARCH) && __ARM_ARCH >= 7))
				__asm__ __volatile__("yield");
#endif
			}

			enum class State : std::uint8_t
			{
				Free = 0,
				Locked = 1
			};

			std::atomic<State> _state { State::Free };
		};

	}
}