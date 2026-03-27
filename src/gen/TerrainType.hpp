#pragma once

/// @brief バイオーム種別（表示・分類用）
enum class BiomeType : uint8
{
	Ocean,         ///< 海
	Lake,          ///< 湖
	CoastalPlain,  ///< 海岸平野
	CoastalHill,   ///< 海岸丘陵
	Plain,         ///< 平野
	Basin,         ///< 窪地
	Hill,          ///< 丘陵
	Foothill,      ///< 山麓
	Plateau,       ///< 高原
	Mountain,      ///< 山地
	MountainRange, ///< 山脈
};

/// @brief 旧 TerrainType の互換エイリアス（段階的移行用）
using TerrainType = BiomeType;
