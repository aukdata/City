#pragma once
#include "Chunk.hpp"

/// @brief Queries use the actual plot polygon, including concave boundaries.
namespace LandPlot
{
	/// @brief Reject collapsed or self-intersecting outlines before applying an edit.
	inline bool validEditablePolygon(const Array<Vec2>& vertices)
	{
		if (vertices.size()<3 || vertices.size()>128) { return false; }
		double area=0;
		for (size_t i=0;i<vertices.size();++i)
		{
			const Vec2 a=vertices[i],b=vertices[(i+1)%vertices.size()];
			if (!IsFinite(a.x) || !IsFinite(a.y) || a.x<0 || a.y<0 || a.x>=WORLD_SIZE || a.y>=WORLD_SIZE
				|| a.distanceFromSq(b)<1.0) { return false; }
			area+=a.cross(b);
		}
		if (Abs(area)<8.0) { return false; }
		const auto cross=[](Vec2 a,Vec2 b,Vec2 c) { return (b-a).cross(c-a); };
		for (size_t i=0;i<vertices.size();++i)
		{
			const Vec2 a=vertices[i],b=vertices[(i+1)%vertices.size()];
			for (size_t j=i+2;j<vertices.size();++j)
			{
				if (i==0 && j+1==vertices.size()) { continue; }
				const Vec2 c=vertices[j],d=vertices[(j+1)%vertices.size()];
				if (cross(a,b,c)*cross(a,b,d)<=0 && cross(c,d,a)*cross(c,d,b)<=0) { return false; }
			}
		}
		return true;
	}
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
		if (patch.type==LandPatchType::GardenSoil) { return patch.sourceParcelKey<0 ? U"公園・緑地" : U"庭・住宅敷地"; }
		if (patch.type==LandPatchType::ParcelGravel) { return U"砂利の敷地"; }
		return U"舗装された敷地";
	}
}
