#pragma once
#include <Siv3D.hpp>
#include "../asset/AssetRegistrar.hpp"

/// @brief 詳細地形と遠景地形で共用する材質。LODによって色やテクスチャを変えない。
namespace TerrainMaterials
{
	/// @brief 全LODで同じ標高を同じ草地材質へ分類する。
	inline int keyForHeight(float height) { return height>28.0f ? 1 : 0; }
	inline ColorF color(int materialKey)
	{
		switch (materialKey)
		{
		case 1: return ColorF{ 0.61, 0.66, 0.72 };
		case 2: return ColorF{ 0.38, 0.54, 0.30 };
		case 3: return ColorF{ 0.50, 0.45, 0.30 };
		case 4: return ColorF{ 0.76, 0.70, 0.52 };
		case 5: return ColorF{ 0.48, 0.58, 0.56 };
		default: return ColorF{ 0.65, 0.69, 0.76 };
		}
	}

	inline StringView texture(int materialKey)
	{
		switch (materialKey)
		{
		case 1:
		case 2:
			return Asset::Grass;
		case 4:
			return Asset::Sand;
		case 5:
			return Asset::CoastSand;
		case 100:
		case 117:
		case 119:
			return Asset::Concrete;
		case 120:
			return Asset::Gravel;
		case 110:
		case 111:
		case 114:
			return Asset::Sand;
		case 101:
		case 113:
			return Asset::SparseGrass;
		default:
			return Asset::Grass;
		}
	}
}
