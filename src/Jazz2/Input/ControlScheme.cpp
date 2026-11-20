#include "ControlScheme.h"
#include "../PreferencesCache.h"
#include "../../nCine/Input/IInputManager.h"

using namespace nCine;

namespace Jazz2::Input
{
	ControlSchemeMapping ControlScheme::_mappings[MaxSupportedPlayers * (std::int32_t)PlayerAction::Count] = {};

	void ControlScheme::Reset()
	{
		// Clear all mappings
		for (auto& mapping : _mappings) {
			mapping.Targets.clear();
		}

		constexpr PlayerAction actions[] = {
			PlayerAction::Left, PlayerAction::Right, PlayerAction::Down, PlayerAction::Up,
			PlayerAction::Fire, PlayerAction::Jump, PlayerAction::Run, PlayerAction::ChangeWeapon
		};
		constexpr Keys keyboardLayouts[][8] = {
			{ Keys::F, Keys::H, Keys::G, Keys::T, Keys::A, Keys::S, Keys::D, Keys::Q },
			{ Keys::Left, Keys::Right, Keys::Down, Keys::Up, Keys::Comma, Keys::Period, Keys::Slash, Keys::RShift },
			{ Keys::NumPad4, Keys::NumPad6, Keys::NumPad5, Keys::NumPad8, Keys::LeftBracket, Keys::RightBracket, Keys::Backslash, Keys::Minus }
		};
		for (std::int32_t player = 0; player < MaxSupportedPlayers && player < 3; player++) {
			auto mappings = GetMappings(player);
			for (std::size_t action = 0; action < 8; action++) {
				mappings[(std::int32_t)actions[action]].Targets.push_back(CreateTarget(keyboardLayouts[player][action]));
			}
			// Buttstomp always uses this player's Down key.
			mappings[(std::int32_t)PlayerAction::Buttstomp].Targets = mappings[(std::int32_t)PlayerAction::Down].Targets;
		}
		auto first = GetMappings(0);
		first[(std::int32_t)PlayerAction::Menu].Targets.push_back(CreateTarget(Keys::Escape));
		first[(std::int32_t)PlayerAction::Console].Targets.push_back(CreateTarget(Keys::Backquote));

		constexpr AxisName FireShoulder = AxisName::RightTrigger;
		constexpr AxisName RunShoulder = AxisName::LeftTrigger;
		constexpr ButtonName FireButton = ButtonName::X;
		constexpr ButtonName RunButton = ButtonName::B;
		constexpr ButtonName ChangeWeaponButton = ButtonName::Y;
		constexpr bool HasAnalogStick = true;
		for (std::int32_t i = 0; i < MaxSupportedPlayers; i++) {
			auto current = GetMappings(i);
			current[(std::int32_t)PlayerAction::Left].Targets.push_back(CreateTarget(i, ButtonName::Left));
			current[(std::int32_t)PlayerAction::Right].Targets.push_back(CreateTarget(i, ButtonName::Right));
			current[(std::int32_t)PlayerAction::Up].Targets.push_back(CreateTarget(i, ButtonName::Up));
			current[(std::int32_t)PlayerAction::Down].Targets.push_back(CreateTarget(i, ButtonName::Down));
			current[(std::int32_t)PlayerAction::Buttstomp].Targets.push_back(CreateTarget(i, ButtonName::Down));
			if constexpr (HasAnalogStick) {
				current[(std::int32_t)PlayerAction::Left].Targets.push_back(CreateTarget(i, AxisName::LeftX, true));
				current[(std::int32_t)PlayerAction::Right].Targets.push_back(CreateTarget(i, AxisName::LeftX));
				current[(std::int32_t)PlayerAction::Up].Targets.push_back(CreateTarget(i, AxisName::LeftY, true));
				current[(std::int32_t)PlayerAction::Down].Targets.push_back(CreateTarget(i, AxisName::LeftY));
				current[(std::int32_t)PlayerAction::Buttstomp].Targets.push_back(CreateTarget(i, AxisName::LeftY));
			}

			current[(std::int32_t)PlayerAction::Fire].Targets.push_back(CreateTarget(i, FireButton));
			current[(std::int32_t)PlayerAction::Fire].Targets.push_back(CreateTarget(i, FireShoulder));
			current[(std::int32_t)PlayerAction::Jump].Targets.push_back(CreateTarget(i, ButtonName::A));
			if constexpr (RunButton != ButtonName::Unknown) {
				current[(std::int32_t)PlayerAction::Run].Targets.push_back(CreateTarget(i, RunButton));
			}
			current[(std::int32_t)PlayerAction::Run].Targets.push_back(CreateTarget(i, RunShoulder));
			current[(std::int32_t)PlayerAction::ChangeWeapon].Targets.push_back(CreateTarget(i, ChangeWeaponButton));
			current[(std::int32_t)PlayerAction::Menu].Targets.push_back(CreateTarget(i, ButtonName::Start));
		}
	}

	ProcessedInput ControlScheme::FetchProcessedInput(std::int32_t playerIndex, const BitArray& pressedKeys, const ArrayView<const JoyMappedState*> joyStates, std::uint64_t prevPressedActions, bool analogAsButtons)
	{
		ProcessedInput result = {};
		std::size_t joyStateCount = joyStates.size();

		auto mappings = GetMappings(playerIndex);
		for (std::size_t i = 0; i < (std::size_t)PlayerAction::Count; i++) {
			for (const auto& target : mappings[i].Targets) {
				if (target.Data & GamepadMask) {
					// Gamepad
					std::uint32_t joyIdx = (target.Data & GamepadIndexMask) >> 16;
					if (joyIdx < joyStateCount) {
						if (target.Data & GamepadAnalogMask) {
							// Analog axis
							AxisName axisName = (AxisName)(target.Data & ButtonMask);
							float axisValue = joyStates[joyIdx]->GetAxisValue(axisName);
							if (target.Data & GamepadNegativeMask) {
								axisValue = -axisValue;
							}

							std::uint64_t maskedBits = (1ull << (std::uint32_t)i) | (1ull << (32 + (std::uint32_t)i));
							if (analogAsButtons && axisValue >= (axisName >= AxisName::LeftTrigger ? IInputManager::TriggerButtonDeadZone : ((prevPressedActions & maskedBits) != maskedBits ? IInputManager::AnalogInButtonDeadZone : IInputManager::AnalogOutButtonDeadZone))) {
								result.PressedActions |= maskedBits;
							}

							if (i < 4 && axisValue > GamepadDeadZone) {
								switch ((PlayerAction)i) {
									case PlayerAction::Left: {
										float axisValueAdjusted = -axisValue;
										if (result.Movement.X < GamepadDeadZone && axisValueAdjusted < result.Movement.X) {
											result.Movement.X = axisValueAdjusted;
										}
										break;
									}
									case PlayerAction::Right: {
										if (result.Movement.X > -GamepadDeadZone && axisValue > result.Movement.X) {
											result.Movement.X = axisValue;
										}
										break;
									}
									case PlayerAction::Up: {
										float axisValueAdjusted = -axisValue;
										if (result.Movement.Y < GamepadDeadZone && axisValueAdjusted < result.Movement.Y) {
											result.Movement.Y = axisValueAdjusted;
										}
										break;
									}
									case PlayerAction::Down: {
										if (result.Movement.Y > -GamepadDeadZone && axisValue > result.Movement.Y) {
											result.Movement.Y = axisValue;
										}
										break;
									}
								}
							}
						} else {
							// Button
							if (joyStates[joyIdx]->IsButtonPressed((ButtonName)(target.Data & ButtonMask))) {
								result.PressedActions |= (1ull << (std::uint32_t)i) | (1ull << (32 + (std::uint32_t)i));
								if (analogAsButtons) {
									switch ((PlayerAction)i) {
										case PlayerAction::Left:
											result.Movement.X = -1.0f;
											break;
										case PlayerAction::Right:
											result.Movement.X = 1.0f;
											break;
										case PlayerAction::Up:
											result.Movement.Y = -1.0f;
											break;
										case PlayerAction::Down:
											result.Movement.Y = 1.0f;
											break;
									}
								}
								break;
							}
						}
					}
				} else {
					// Keyboard
					if (pressedKeys[target.Data]) {
						result.PressedActions |= (1ull << (std::uint32_t)i);
						if (analogAsButtons) {
							switch ((PlayerAction)i) {
								case PlayerAction::Left:
									result.Movement.X = -1.0f;
									break;
								case PlayerAction::Right:
									result.Movement.X = 1.0f;
									break;
								case PlayerAction::Up:
									result.Movement.Y = -1.0f;
									break;
								case PlayerAction::Down:
									result.Movement.Y = 1.0f;
									break;
							}
						}
						break;
					}
				}
			}
		}

		// Normalize both axes
		float movementLengthX = std::abs(result.Movement.X);
		float normalizedLength = (movementLengthX - GamepadDeadZone) / (1.0f - GamepadDeadZone * 2.0f);
		normalizedLength = std::clamp(normalizedLength, 0.0f, 1.0f);
		result.Movement.X = std::copysign(normalizedLength, result.Movement.X);

		float movementLengthY = std::abs(result.Movement.Y);
		normalizedLength = (movementLengthY - GamepadDeadZone) / (1.0f - GamepadDeadZone * 2.0f);
		normalizedLength = std::clamp(normalizedLength, 0.0f, 1.0f);
		result.Movement.Y = std::copysign(normalizedLength, result.Movement.Y);

		return result;
	}

	std::uint32_t ControlScheme::FetchNavigation(const BitArray& pressedKeys, const ArrayView<const JoyMappedState*> joyStates, NavigationFlags flags)
	{
		std::uint32_t pressedActions = 0;
		std::size_t joyStateCount = joyStates.size();
		bool allowGamepads = (flags & NavigationFlags::AllowGamepads) == NavigationFlags::AllowGamepads;
		bool allowKeyboard = (flags & NavigationFlags::AllowKeyboard) == NavigationFlags::AllowKeyboard;
		std::uint32_t assignedGamepads = 0;

		for (std::int32_t j = 0; j < MaxSupportedPlayers; j++) {
			const auto* mappings = &_mappings[j * (std::int32_t)PlayerAction::Count];
			for (std::size_t i = 0; i < (std::size_t)PlayerAction::CountInMenu; i++) {
				for (const auto& target : mappings[i].Targets) {
					if (target.Data & GamepadMask) {
						// Gamepad
						std::uint32_t joyIdx = (target.Data & GamepadIndexMask) >> 16;
						if (allowGamepads && joyIdx < joyStateCount) {
							if (joyIdx < 32) {
								assignedGamepads |= (1u << joyIdx);
							}
							if (target.Data & GamepadAnalogMask) {
								// Analog axis
								AxisName axisName = (AxisName)(target.Data & ButtonMask);
								float axisValue = joyStates[joyIdx]->GetAxisValue(axisName);
								if (target.Data & GamepadNegativeMask) {
									axisValue = -axisValue;
								}
								if (axisValue >= (axisName >= AxisName::LeftTrigger
														 ? IInputManager::TriggerButtonDeadZone
														 : IInputManager::AnalogOutButtonDeadZone)) {
									pressedActions |= (1 << (std::uint32_t)i);
								}
							} else if (i == (std::size_t)PlayerAction::Up || i == (std::size_t)PlayerAction::Down ||
								i == (std::size_t)PlayerAction::Left || i == (std::size_t)PlayerAction::Right) {
								// Button, only directions follow bindings, the rest is fixed below
								if (joyStates[joyIdx]->IsButtonPressed((ButtonName)(target.Data & ButtonMask))) {
									pressedActions |= (1 << (std::uint32_t)i);
									break;
								}
							}
						}
					} else if (allowKeyboard) {
						// Keyboard
						if (pressedKeys[target.Data]) {
							pressedActions |= (1 << (std::uint32_t)i);
							break;
						}
					}
				}
			}
		}

		if (PreferencesCache::SwapJumpAndChangeWeaponInMenu) {
			// Flips the two bits only if exactly one of them is set
			constexpr std::uint32_t SwapMask = (1u << (std::uint32_t)PlayerAction::Jump) | (1u << (std::uint32_t)PlayerAction::ChangeWeapon);
			std::uint32_t swapBits = (pressedActions & SwapMask);
			if (swapBits != 0 && swapBits != SwapMask) {
				pressedActions ^= SwapMask;
			}
		}

		// Menu buttons are fixed on every gamepad assigned to a player, regardless of action bindings
		for (std::uint32_t joyIdx = 0; joyIdx < joyStateCount && joyIdx < 32; joyIdx++) {
			if ((assignedGamepads & (1u << joyIdx)) == 0) {
				continue;
			}
			const auto* joyState = joyStates[joyIdx];
			if (joyState->IsButtonPressed(ButtonName::A) || joyState->IsButtonPressed(ButtonName::X)) {
				pressedActions |= (1 << (std::uint32_t)PlayerAction::Fire);
			}
			if (joyState->IsButtonPressed(ButtonName::B) || joyState->IsButtonPressed(ButtonName::Start)) {
				pressedActions |= (1 << (std::uint32_t)PlayerAction::Menu);
			}
			if (joyState->IsButtonPressed(ChangeWeaponMenuButton)) {
				pressedActions |= (1 << (std::uint32_t)PlayerAction::ChangeWeapon);
			}
		}

		// Allow Jump action as confirm key
		if (pressedActions & (1 << (std::uint32_t)PlayerAction::Jump)) {
			pressedActions |= (1 << (std::uint32_t)PlayerAction::Fire);
		}

		if (allowKeyboard) {
			// Also allow Return (Enter) as confirm key
			if (pressedKeys[(std::uint32_t)Keys::Return] || pressedKeys[(std::uint32_t)Keys::NumPadEnter]) {
				pressedActions |= (1 << (std::int32_t)PlayerAction::Fire);
			}
			// Use ChangeWeapon action as delete key
			if (pressedKeys[(std::uint32_t)Keys::Delete]) {
				pressedActions |= (1 << (std::int32_t)PlayerAction::ChangeWeapon);
			}
		}

		return pressedActions;
	}

	ArrayView<ControlSchemeMapping> ControlScheme::GetAllMappings()
	{
		return _mappings;
	}

	ArrayView<ControlSchemeMapping> ControlScheme::GetMappings(std::int32_t playerIdx)
	{
		return ArrayView(&_mappings[playerIdx * (std::int32_t)PlayerAction::Count], (std::int32_t)PlayerAction::Count);
	}

	std::int32_t ControlScheme::GetGamepadForPlayer(std::int32_t playerIdx)
	{
		const auto* mappings = &_mappings[playerIdx * (std::int32_t)PlayerAction::Count];
		for (std::size_t i = 0; i < (std::size_t)PlayerAction::CountInMenu; i++) {
			for (const auto& target : mappings[i].Targets) {
				if (target.Data & GamepadMask) {
					std::uint32_t joyIdx = (target.Data & GamepadIndexMask) >> 16;
					return joyIdx;
				}
			}
		}
		return -1;
	}

	bool ControlScheme::ContainsTarget(PlayerAction action, MappingTarget target)
	{
		std::int32_t index = (std::int32_t)action;
		if (index < (std::int32_t)PlayerAction::Count) {
			for (std::int32_t j = 0; j < MaxSupportedPlayers; j++) {
				const auto& mappings = _mappings[j * (std::int32_t)PlayerAction::Count + index];
				for (const auto& t : mappings.Targets) {
					if (t.Data == target.Data) {
						return true;
					}
				}
			}
		}

		return false;
	}

	MappingTarget ControlScheme::CreateTarget(Keys key)
	{
		return { (std::uint32_t)key };
	}

	MappingTarget ControlScheme::CreateTarget(std::uint32_t gamepadIndex, ButtonName button)
	{
		return { GamepadMask | ((gamepadIndex << 16) & GamepadIndexMask) | (std::uint32_t)button };
	}

	MappingTarget ControlScheme::CreateTarget(std::uint32_t gamepadIndex, AxisName axis, bool negative)
	{
		std::uint32_t result = GamepadMask | GamepadAnalogMask | ((gamepadIndex << 16) & GamepadIndexMask) | (std::uint32_t)axis;
		if (negative) {
			result |= GamepadNegativeMask;
		}
		return { result };
	}
}