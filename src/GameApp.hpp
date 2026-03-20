#pragma once
#include "scene/TitleScene.hpp"
#include "scene/GameScene.hpp"

/// @brief ゲームループ統括クラス
class GameApp
{
public:
	/// @brief ゲームを起動して実行する（メインループ）
	static void run();
};
