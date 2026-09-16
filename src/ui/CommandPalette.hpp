#pragma once
#include "KeyboardActions.hpp"
#include "../debug/GameCommands.hpp"

/// @brief 左下のコマンド入力。開いている間はゲームへのキー入力を遮断する。
class CommandPalette
{
public:
	bool visible=false;
	String input, message;
	bool error=false;
	void open();
	void close();
	[[nodiscard]] Optional<String> update();
	void draw(Size size,const Font& font) const;
	[[nodiscard]] RectF bounds(Size size) const;
	void report(String text,bool failed) { message=std::move(text);error=failed; }
private:
	Array<String> m_history;
	size_t m_cursor=0,m_historyIndex=0,m_choice=0;
	bool m_justOpened=false;
};
