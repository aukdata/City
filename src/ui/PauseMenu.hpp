#pragma once
#include <Siv3D.hpp>
#include "KeyboardActions.hpp"

/// @brief A pause menu containing only implemented actions, with keyboard navigation.
class PauseMenu
{
public:
	enum class Action { Resume, Save, Title, Quit, Settings };
	static constexpr int kActionCount = 5;
	int selected=0;
	static RectF panel(Size size) { return {(size.x-340)*.5,(size.y-410)*.5,340,410}; }
	static RectF button(Size size,int index) { const auto p=panel(size);return {p.x+40,p.y+92+index*54,260,42}; }
	Optional<Action> draw(const Font& font,Size size)
	{
		if (GameInput::down(KeyUp)) { selected=(selected+kActionCount-1)%kActionCount; }
		if (GameInput::down(KeyDown)) { selected=(selected+1)%kActionCount; }
		RectF{0,0,size.x,size.y}.draw(ColorF{0,.6});
		const auto bounds=panel(size);bounds.rounded(10).draw(ColorF{.10,.13,.17,.98}).drawFrame(1,ColorF{.4,.48,.56});
		font(U"一時停止").drawAt(26,Vec2{bounds.center().x,bounds.y+36},ColorF{.95});
		font(U"↑ ↓ で選択  /  Enter で決定  /  Esc で戻る").drawAt(12,Vec2{bounds.center().x,bounds.y+67},ColorF{.68,.76,.82});
		const std::array<String,kActionCount> labels{U"ゲームに戻る",U"セーブ",U"タイトルに戻る",U"ゲーム終了",
			U"設定"};
		for (int index=0;index<kActionCount;++index)
		{
			const auto area=button(size,index);
			const bool hover=area.mouseOver();
			area.rounded(5).draw(hover || index==selected ? ColorF{.23,.37,.48} : ColorF{.16,.21,.26});
			font(labels[index]).drawAt(18,area.center(),ColorF{.94});
			if (hover && MouseL.down()) { selected=index;return static_cast<Action>(index); }
		}
		if (GameInput::down(KeyEnter)) { return static_cast<Action>(selected); }
		return none;
	}
};
