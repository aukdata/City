#include "CommandPalette.hpp"
#include "NavigationHeader.hpp"

void CommandPalette::open()
{
	GameInput::releaseTextFocus();visible=true;input=U"/";m_cursor=1;m_choice=0;
	m_historyIndex=m_history.size();m_textEdit=BufferedTextEdit{};m_justOpened=true;GameInput::textOwnedFrame=true;
}
void CommandPalette::close() { visible=false;GameInput::textOwnedFrame=true; }
namespace
{
	/// @brief パレットが所有するキーは、短い入力もフレームバッファから一度だけ受け取る。
	bool pressedOnce(const Input& key)
	{
		return key.down() || GameInput::buffer.down(key.code());
	}

	/// @brief Include complete short taps while keeping native key hold timing for fallback repeats.
	BufferedTextEditKey editKeyState(const Input& key)
	{
		return {Max(GameInput::buffer.editPressCount(key.code()), key.down() ? size_t{1} : size_t{0}),
			key.pressed(), key.pressedDuration().count()};
	}
}

Optional<String> CommandPalette::update()
{
	if (!visible) { return none; }
	GameInput::textOwnedFrame=true;
	if (m_justOpened) { m_justOpened=false;return none; }
	const bool composing=!TextInput::GetEditingText().isEmpty();
	if (pressedOnce(KeyEscape)) { close();return none; }
	if (!composing && pressedOnce(KeyTab))
	{
		const auto choices=GameCommands::suggest(input);
		if (!choices.isEmpty()) { input=choices[Min(m_choice,choices.size()-1)].completion;m_cursor=input.size(); }
		return none;
	}
	if (!composing && pressedOnce(KeyUp) && !m_history.isEmpty())
	{
		if (m_historyIndex>0) { --m_historyIndex; }input=m_history[m_historyIndex];m_cursor=input.size();return none;
	}
	if (!composing && pressedOnce(KeyDown) && !m_history.isEmpty())
	{
		m_historyIndex=Min(m_history.size(),m_historyIndex+1);input=m_historyIndex<m_history.size() ? m_history[m_historyIndex] : U"/";m_cursor=input.size();return none;
	}
	const String rawInput = TextInput::GetRawInput();
	m_cursor = TextInput::UpdateText(input, m_cursor);
	m_cursor = m_textEdit.update(input, m_cursor, rawInput, composing,
		editKeyState(KeyBackspace), editKeyState(KeyDelete));
	input.remove(U'\n').remove(U'\r').remove(U'\t');
	if (input.size()>256) { input.resize(256); }m_cursor=Min(m_cursor,input.size());
	if (!composing && pressedOnce(KeyEnter) && !input.trimmed().isEmpty())
	{
		const String command=input.trimmed();
		if (m_history.isEmpty() || m_history.back()!=command) { m_history << command; }
		if (m_history.size()>64) { m_history.erase(m_history.begin()); }m_historyIndex=m_history.size();return command;
	}
	return none;
}
RectF CommandPalette::bounds(Size size) const
{
	return {12,Max(12,size.y-272),Min(660,Max(280,size.x-24)),260};
}
void CommandPalette::draw(Size size,const Font& font) const
{
	if (!visible) { return; }
	const RectF area=bounds(size);const ScopedCustomShader2D standard{VertexShader{},PixelShader{}};
	area.rounded(6).draw(ColorF{.04,.08,.12,.97}).drawFrame(1,ColorF{.3,.5,.62});
	font(U"コマンド  ·  Tab:補完  ↑↓:履歴  Enter:実行  Esc:閉じる").draw(13,area.pos+Vec2{12,8},ColorF{.65,.8,.88});
	const RectF entry{area.pos+Vec2{10,34},area.w-20,32};entry.draw(ColorF{.12,.19,.25});
	const size_t cursor=Min(m_cursor,input.size());
	const String editing=TextInput::GetEditingText();
	String before=input.substr(0,cursor)+editing;
	while(before.size()>1 && font(before).region(16).w>entry.w-24) { before.erase(before.begin()); }
	String after=input.substr(cursor);
	while(!after.isEmpty() && font(before+after).region(16).w>entry.w-20) { after.pop_back(); }
	font(before+after).draw(16,entry.pos+Vec2{8,5},Palette::White);
	if (Fmod(Scene::Time(),1)<.6)
	{
		const double caret=entry.x+8+font(before).region(16).w;
		Line{caret,entry.y+5,caret,entry.y+24}.draw(1,Palette::White);
	}
	const auto choices=GameCommands::suggest(input);
	for (size_t i=0;i<Min<size_t>(5,choices.size());++i)
	{
		const Vec2 pos=area.pos+Vec2{12,74+i*29.0};
		NavigationHeader::fitted(font,choices[i].syntax,{pos,area.w-24,16},13,ColorF{.9});
		NavigationHeader::fitted(font,choices[i].description,{pos+Vec2{0,15},area.w-24,13},10,ColorF{.6,.75,.82});
	}
	NavigationHeader::fitted(font,message,{area.pos+Vec2{12,230},area.w-24,20},13,error ? ColorF{1,.55,.43} : ColorF{.62,.88,.65});
}
