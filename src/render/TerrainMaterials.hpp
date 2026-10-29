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
	/// @brief 市街地の未利用地。雑草と裸地がまだらに混じる。
	constexpr int kVacantLot = 8;
	/// @brief 公園・庭として手入れされた芝地。
	constexpr int kTendedGreen = 9;
	/// @brief 舗装・空き地・手入れされた緑地を頂点ごとの割合で混ぜる町なかの地表。セル境界に段差を出さない。
	constexpr int kUrbanBlend = 11;
	/// @brief kUrbanBlend に混ぜるセル。それ以外の材質は従来どおりセル単位で描く。
	inline bool blendsInTown(int materialKey) { return materialKey==kUrbanPaving || materialKey==kVacantLot || materialKey==kTendedGreen; }
	/// @brief 地形シェーダーが選ぶ地表の植生。拡散色のアルファで渡し、描画単位を増やさない。
	enum class Cover { Plain, Meadow, Vacant, Tended, Blend };
	inline Cover cover(int materialKey)
	{
		switch (materialKey)
		{
		case 0: case 1: return Cover::Meadow;
		case kVacantLot: return Cover::Vacant;
		case kTendedGreen: return Cover::Tended;
		case kUrbanBlend: return Cover::Blend;
		default: return Cover::Plain;
		}
	}
	/// @brief Land use and actual occupation determine the ground; zoning alone does not clear vacant gardens.
	inline int keyForCell(const Chunk& chunk, int col, int row)
	{
		const int x = Clamp(col,0,ZONE_CELLS-1),z = Clamp(row,0,ZONE_CELLS-1);
		const float h = (chunk.heightMap[{x,z}]+chunk.heightMap[{x+1,z}]+chunk.heightMap[{x,z+1}]+chunk.heightMap[{x+1,z+1}])*.25f;
		const int natural = keyForHeight(h);
		const auto zone = chunk.zoneMap[{x,z}];
		if (zone!=ZoneType::Residential && zone!=ZoneType::LowResidential && zone!=ZoneType::Commercial && zone!=ZoneType::Industrial) { return natural; }
		const auto own = chunk.buildingGrid[{x,z}].type;
		if (own==BuildingType::ParkBuilding) { return kTendedGreen; }
		int built = 0;
		for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
		{
			const int nx=x+dx,nz=z+dz;
			if (nx<0 || nz<0 || nx>=ZONE_CELLS || nz>=ZONE_CELLS) { continue; }
			const auto type = chunk.buildingGrid[{nx,nz}].type;
			built += type!=BuildingType::None && type!=BuildingType::Farmland && type!=BuildingType::ParkBuilding;
		}
		// 町の中で建物のない土地は芝生ではなく、雑草地・空き地になる。
		if (built<2) { return kVacantLot; }
		if (zone==ZoneType::Industrial) { return kUrbanGround; }
		if (own==BuildingType::None || own==BuildingType::Farmland) { return kVacantLot; }
		if (zone==ZoneType::LowResidential) { return kTendedGreen; }
		return kUrbanPaving;
	}
	inline ColorF baseColor(int materialKey)
	{
		switch (materialKey)
		{
		case 1: return ColorF{ 0.61, 0.66, 0.72 };
		case 2: return ColorF{ 0.38, 0.54, 0.30 };
		case 3: return ColorF{ 0.50, 0.45, 0.30 };
		case 4: return ColorF{ 0.76, 0.70, 0.52 };
		case 5: return ColorF{ 0.48, 0.58, 0.56 };
		case kUrbanGround: return ColorF{ 0.62, 0.74, 1.05 };
		case kUrbanPaving: return ColorF{ .66,.76,1.04 };
		case kVacantLot: return ColorF{ 0.66, 0.66, 0.70 };
		case kTendedGreen: return ColorF{ 0.62, 0.68, 0.74 };
		default: return ColorF{ 0.65, 0.69, 0.76 };
		}
	}
	inline ColorF color(int materialKey)
	{
		// アルファは不透明度ではなく、Terrain_PS が植生を選ぶための区分。
		constexpr double kCoverAlpha[] = { 1.0, 0.75, 0.5, 0.25, 0.0 };
		ColorF result = baseColor(materialKey);
		result.a = kCoverAlpha[static_cast<int>(cover(materialKey))];
		return result;
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
