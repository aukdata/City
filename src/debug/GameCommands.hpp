#pragma once
#include <Siv3D.hpp>

/// @brief デバッグコマンドの構文と補完。解析中はゲーム状態を変更しない。
namespace GameCommands
{
	enum class Kind : uint8 { Help, Time, Day, RoadStatus, RoadInspect, CameraGoto, CameraZoom, Money, Speed, Fps, RenderDistance };
	struct Command { Kind kind; Array<double> numbers; String argument; };
	struct ParseResult { Optional<Command> command; String message; };
	struct Suggestion { String syntax, description, completion; };
	[[nodiscard]] ParseResult parse(StringView input);
	[[nodiscard]] Array<Suggestion> suggest(StringView input);
}
