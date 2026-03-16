#pragma once
#include "../time/GameClock.hpp"
#include "../economy/Economy.hpp"

/// @brief 2D HUD の描画クラス
class UIRenderer
{
public:
	UIRenderer();

	/// @brief HUD を描画する（毎フレーム呼ぶ）
	/// @param clock        ゲーム時計
	/// @param vehicleCount 現在の車両数
	/// @param modeText     現在の編集モード文字列
	/// @param economy      経済状態
	void render(const GameClock& clock, int vehicleCount, StringView modeText,
	            const Economy& economy);

private:
	Font m_font;
	Font m_smallFont;
};
