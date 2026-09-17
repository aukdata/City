#pragma once
#include "../gen/GenerationOptions.hpp"

/// @brief 新規生成と保存一覧の共通レイアウト。本体へ接続する前にTestで描画する。
namespace StartScreenControls
{
	RectF optionBounds(Vec2 origin, size_t index);
	void drawOptions(Vec2 origin, const GenerationOptions& options, const Font& font);
	bool selectOption(Vec2 origin, GenerationOptions& options, Vec2 cursor, bool clicked);
	struct SaveList
	{
		Array<String> names;
		int selected = -1, first = 0;
		String confirming;
		String error;
	};
	enum class Action
	{
		None,
		Load,
		Delete
	};
	constexpr int kVisibleSaves = 7;
	RectF saveRow(Vec2 origin, int row);
	RectF loadButton(Vec2 origin);
	RectF deleteButton(Vec2 origin);
	RectF confirmationButton(Vec2 origin, bool confirm);
	void drawSaves(Vec2 origin, const SaveList& state, const Font& font);
	Action interactSaves(Vec2 origin, SaveList& state, Vec2 cursor, bool clicked, double wheel = 0);
	/// @brief 地上・地下の切替。入力と描画で同じ領域を使う。
	RectF layerButton(Size size, bool fpsVisible = false);
	void drawLayerButton(Size size, bool underground, const Font& font, bool fpsVisible = false);
} // namespace StartScreenControls
