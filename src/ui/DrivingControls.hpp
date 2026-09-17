#pragma once
#include "KeyboardActions.hpp"
#include "../traffic/DrivingController.hpp"

/// @brief 入力欄・地図・メニューが占有する間は運転操作を返さない。
namespace DrivingControls
{
	inline DrivingInput read(bool available)
	{
		if (!available || GameInput::keyboardBlocked()) { return {}; }
		return {static_cast<double>(GameInput::pressed(KeyW)),static_cast<double>(GameInput::pressed(KeyS)),
			static_cast<double>(GameInput::pressed(KeyD))-GameInput::pressed(KeyA),GameInput::pressed(KeySpace)};
	}
}
