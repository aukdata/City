#pragma once
#include "GameCommands.hpp"
#include "../road/RoadNetwork.hpp"
#include "../ui/Camera.hpp"

/// @brief コマンドの確定操作。描画とUI更新は結果の差分から呼び出し側が行う。
namespace CommandExecution
{
	struct Context { GameClock& clock; RoadNetwork& roads; const World& world; GameCamera& camera; double& funds; bool& fps; };
	struct Result { bool success=false; String message; Array<int> changedEdges; bool changedTime=false; };
	[[nodiscard]] Result execute(const GameCommands::Command& command,Context context);
}
