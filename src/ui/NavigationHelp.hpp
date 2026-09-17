#pragma once
#include <Siv3D.hpp>

/// @brief 必要時に開くカメラ・編集操作の一覧。HUDと描画テストで共有する。
namespace NavigationHelp
{
	inline const String kOverviewText = U"WASD:移動  Ctrl:高速  F:徒歩  C:運転  M:地図\n"
		U"中ドラッグ:回転  Shift＋中ドラッグ:平行移動\n"
		U"ホイール:ズーム  R:道路  Z:用途  X:線路  H:ダイヤ\n"
		U"Space:停止/再開  1-3:速度  Esc:閉じる\n"
		U"G:地形  B:バス  N:地名  Tab:用途表示\n"
		U"地形:Ctrl＋ホイールで範囲  バス:Bで確定";
	inline const String kWalkingText = U"WASD:歩く  Shift:走る  Ctrl:高速  F:俯瞰\n"
		U"右/中ドラッグ:見回す  C:運転  M:地図\n"
		U"R:道路  Z:用途  G:地形  X:線路  H:ダイヤ  B:バス\n"
		U"Space:停止/再開  1-3:速度  Esc:閉じる\n"
		U"G:地形  B:バス  N:地名  Tab:用途表示\n"
		U"地形:Ctrl＋ホイールで範囲  バス:Bで確定";
	inline RectF bounds(Size size) { return {size.x-418,size.y-124,410,116}; }
	inline void draw(const Font& font, Size size, bool walking = false)
	{
		const auto area = bounds(size);
		area.rounded(4).draw(ColorF{.04,.06,.08,.70});
		font(walking ? kWalkingText : kOverviewText).draw(12, area.pos+Vec2{10,5}, ColorF{.94});
	}
}
