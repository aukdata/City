#pragma once
#include "../gen/TerrainType.hpp"

/// @brief シーン識別子
enum class SceneState { Title, Game };

/// @brief シーン間で共有するデータ
struct SceneData
{
	uint64      seed    = 20260316ULL;
	TerrainType terrain = TerrainType::Hills;
};

/// @brief SceneManager の型エイリアス
using App = SceneManager<SceneState, SceneData>;
