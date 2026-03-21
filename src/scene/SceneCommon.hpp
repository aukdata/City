#pragma once
#include "../gen/TerrainType.hpp"

/// @brief シーン識別子
enum class SceneState { Title, Game };

/// @brief シーン間で共有するデータ
struct SceneData
{
	uint64      seed        = 20260316ULL;
	TerrainType terrain     = TerrainType::Hills;
	bool        sandboxMode = true;   ///< サンドボックスモード（道路形状を自由に編集）
	String      saveName;             ///< セーブ名（空ならセーブなし）
};

/// @brief SceneManager の型エイリアス
using App = SceneManager<SceneState, SceneData>;
