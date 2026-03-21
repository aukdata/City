#pragma once

/// @brief マップ生成時の地形タイプ（03_procedural_generation_spec.md §2）
enum class TerrainType : uint8
{
	Basin    = 0,  ///< 山間盆地：四方を山に囲まれた盆地
	Coastal  = 1,  ///< 沿岸平野：片側が海、反対側が山地
	RiverFan = 2,  ///< 河川扇状地：山から流れ出る扇状地
	Hills    = 3,  ///< 丘陵台地：緩やかな丘が連続する地形
};
