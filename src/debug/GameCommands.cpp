#include "GameCommands.hpp"

namespace
{
	struct Definition { GameCommands::Kind kind; String syntax, description; };
	const Array<Definition>& definitions()
	{
		using Kind=GameCommands::Kind;
		static const Array<Definition> values{
			{Kind::Help,U"/help",U"コマンド一覧を表示"},
			{Kind::Time,U"/time set <hour> <minute>",U"時刻を変更（0～23時、0～59分）"},
			{Kind::Day,U"/day set <year> <month> <day>",U"日付を変更（各月30日）"},
			{Kind::RoadStatus,U"/road status set <id> planned",U"指定道路を計画中にする"},
			{Kind::RoadStatus,U"/road status set <id> open",U"指定道路を建設済み・供用中にする"},
			{Kind::RoadStatus,U"/road status set <id> closed",U"指定道路を通行止めにする"},
			{Kind::RoadStatus,U"/road status set <id> construction",U"指定道路を工事中にする"},
			{Kind::RoadInspect,U"/road inspect <id>",U"道路の状態・延長・構造を調べる"},
			{Kind::CameraGoto,U"/camera goto <x> <z>",U"指定座標へ俯瞰視点で移動"},
			{Kind::CameraZoom,U"/camera zoom <meters>",U"俯瞰距離を指定（5～60000 m）"},
			{Kind::Money,U"/money set <amount>",U"資金を設定（億円）"},
			{Kind::Speed,U"/speed set <speed>",U"時間速度を設定（0・1・2・4）"},
			{Kind::Fps,U"/fps on",U"FPSグラフを表示"},
			{Kind::Fps,U"/fps off",U"FPSグラフを非表示"}
		};
		return values;
	}
	Array<String> words(StringView value)
	{
		String text{value};text.replace(U'\t',U' ');auto result=text.split(U' ');
		result.remove_if([](const String& word){return word.isEmpty();});return result;
	}
	bool placeholder(const String& token) { return token.starts_with(U"<"); }
	bool integer(double value) { return Floor(value)==value; }
}

Array<GameCommands::Suggestion> GameCommands::suggest(StringView input)
{
	const String raw{input};const auto typed=words(raw);const bool trailing=!raw.isEmpty() && raw.back()==U' ';
	Array<Suggestion> result;
	for (const auto& definition:definitions())
	{
		const auto pattern=words(definition.syntax);if (typed.size()>pattern.size()) { continue; }
		bool matches=true;
		for(size_t i=0;i<typed.size();++i)
		{
			if (placeholder(pattern[i])) { continue; }
			const bool partial=i+1==typed.size() && !trailing;
			matches &= partial ? pattern[i].starts_with(typed[i]) : pattern[i]==typed[i];
		}
		if (!matches) { continue; }
		const size_t token=trailing ? typed.size() : (typed.isEmpty() ? 0 : typed.size()-1);
		Array<String> completed=typed;
		if (token<pattern.size() && !placeholder(pattern[token]))
		{
			if (token==completed.size()) { completed << pattern[token]; } else { completed[token]=pattern[token]; }
		}
		String completion;
		for(const auto& part:completed) { if (!completion.isEmpty()) { completion+=U" "; }completion+=part; }
		if (token+1<pattern.size()) { completion+=U" "; }
		result << Suggestion{definition.syntax,definition.description,std::move(completion)};
	}
	return result;
}

GameCommands::ParseResult GameCommands::parse(StringView input)
{
	const auto typed=words(input);
	for (const auto& definition:definitions())
	{
		const auto pattern=words(definition.syntax);if (typed.size()!=pattern.size()) { continue; }
		Command command{definition.kind,{},U""};bool matches=true;
		for(size_t i=0;i<typed.size();++i)
		{
			if (!placeholder(pattern[i])) { matches &= typed[i]==pattern[i];continue; }
			const auto number=ParseOpt<double>(typed[i]);
			if (!number || !IsFinite(*number)) { matches=false;break; }command.numbers << *number;
		}
		if (!matches) { continue; }
		const auto& n=command.numbers;bool valid=true;
		switch(command.kind)
		{
		case Kind::Time: valid=integer(n[0]) && integer(n[1]) && InRange(n[0],0.0,23.0) && InRange(n[1],0.0,59.0);break;
		case Kind::Day: valid=integer(n[0]) && integer(n[1]) && integer(n[2]) && InRange(n[0],1.0,9999.0) && InRange(n[1],1.0,12.0) && InRange(n[2],1.0,30.0);break;
		case Kind::RoadStatus:
		case Kind::RoadInspect: valid=integer(n[0]) && InRange(n[0],0.0,2147483646.0);break;
		case Kind::CameraGoto: valid=InRange(n[0],0.0,65535.0) && InRange(n[1],0.0,65535.0);break;
		case Kind::CameraZoom: valid=InRange(n[0],5.0,60000.0);break;
		case Kind::Money: valid=InRange(n[0],0.0,1e9);break;
		case Kind::Speed: valid=n[0]==0 || n[0]==1 || n[0]==2 || n[0]==4;break;
		default: break;
		}
		if (!valid) { return {none,U"値が範囲外です: "+definition.description}; }
		command.argument=typed.back();return {std::move(command),U""};
	}
	return {none,U"書式を確認してください。/help または Tab で候補を表示できます"};
}
