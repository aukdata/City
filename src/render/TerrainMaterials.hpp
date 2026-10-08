#pragma once
#include <Siv3D.hpp>
#include "../world/Chunk.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @brief 詳細地形と遠景地形で共用する材質。LODによって色やテクスチャを変えない。
namespace TerrainMaterials
{
	/// @brief 全LODで同じ標高を同じ草地材質へ分類する。
	inline int keyForHeight(float height) { return height>28.0f ? 1 : 0; }
	/// @brief 市街地の建物間の土間・砂利。芝生にしない。
	constexpr int kUrbanGround = 6;
	/// @brief Shared paved yards between urban buildings, separate from earth and planted vacant plots.
	constexpr int kUrbanPaving = 7;
	/// @brief Land use and actual occupation determine the ground; zoning alone does not clear vacant gardens.
	inline int keyForCell(const Chunk& chunk, int col, int row)
	{
		const int x = Clamp(col,0,ZONE_CELLS-1),z = Clamp(row,0,ZONE_CELLS-1);
		const float h = (chunk.heightMap[{x,z}]+chunk.heightMap[{x+1,z}]+chunk.heightMap[{x,z+1}]+chunk.heightMap[{x+1,z+1}])*.25f;
		const int natural = keyForHeight(h);
		const auto zone = chunk.zoneMap[{x,z}];
		if (zone!=ZoneType::Residential && zone!=ZoneType::LowResidential && zone!=ZoneType::Commercial && zone!=ZoneType::Industrial) { return natural; }
		int built = 0;
		for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
		{
			const int nx=x+dx,nz=z+dz;
			if (nx<0 || nz<0 || nx>=ZONE_CELLS || nz>=ZONE_CELLS) { continue; }
			const auto type = chunk.buildingGrid[{nx,nz}].type;
			built += type!=BuildingType::None && type!=BuildingType::Farmland && type!=BuildingType::ParkBuilding;
		}
		if (built<2) { return natural; }
		const auto type = chunk.buildingGrid[{x,z}].type;
		if (zone==ZoneType::Industrial) { return kUrbanGround; }
		if (type==BuildingType::None || type==BuildingType::Farmland || type==BuildingType::ParkBuilding || zone==ZoneType::LowResidential) { return natural; }
		return kUrbanPaving;
	}
	inline ColorF color(int materialKey)
	{
		switch (materialKey)
		{
		case 1: return ColorF{ 0.61, 0.66, 0.72 };
		case 2: return ColorF{ 0.38, 0.54, 0.30 };
		case 3: return ColorF{ 0.50, 0.45, 0.30 };
		case 4: return ColorF{ 0.76, 0.70, 0.52 };
		case 5: return ColorF{ 0.48, 0.58, 0.56 };
		case kUrbanGround: return ColorF{ 0.74, 0.72, 0.66 };
		case kUrbanPaving: return ColorF{ .68,.68,.66 };
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
		case kUrbanPaving:
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
		case kUrbanGround:
			return Asset::Gravel;
		case 101:
		case 113:
			return Asset::SparseGrass;
		default:
			return Asset::Grass;
		}
	}
}
