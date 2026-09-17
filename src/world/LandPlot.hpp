#pragma once
#include "Chunk.hpp"

/// @brief Queries use the actual plot polygon, including concave boundaries.
namespace LandPlot
{
	inline bool selectable(const LandPatch& patch)
	{
		return patch.type != LandPatchType::Beach && patch.type != LandPatchType::Seawall && patch.polygon.size() >= 3;
	}
	inline s3d::Polygon shape(const LandPatch& patch)
	{
		Array<Vec2> outline=patch.polygon;
		double area=0;
		for (size_t i=0;i<outline.size();++i) { area+=outline[i].cross(outline[(i+1)%outline.size()]); }
		if (area<0) { outline.reverse(); }
		return s3d::Polygon{outline};
	}
	inline const LandPatch* find(const Chunk& chunk,Vec2 position)
	{
		for (const auto& patch : chunk.landPatches)
		{
			if (selectable(patch) && shape(patch).contains(position)) { return &patch; }
		}
		return nullptr;
	}
	inline bool containsSurface(const MeshData& surface,Vec2 position)
	{
		for (const auto& triangle : surface.indices)
		{
			const auto a=surface.vertices[triangle.i0].pos,b=surface.vertices[triangle.i1].pos,c=surface.vertices[triangle.i2].pos;
			if (Triangle{Vec2{a.x,a.z},Vec2{b.x,b.z},Vec2{c.x,c.z}}.contains(position)) { return true; }
		}
		return false;
	}
	inline StringView name(const LandPatch& patch)
	{
		if (patch.type==LandPatchType::FarmTrack) { return U"耕作道"; }
		if (patch.type==LandPatchType::IrrigationDitch) { return U"用排水路"; }
		if (patch.type==LandPatchType::PaddyField) { return U"水田"; }
		if (patch.type==LandPatchType::FarmField) { return U"畑"; }
		if (patch.type==LandPatchType::GardenSoil) { return U"庭・住宅敷地"; }
		if (patch.type==LandPatchType::ParcelGravel) { return U"砂利の敷地"; }
		return U"舗装された敷地";
	}
}
