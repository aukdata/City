#pragma once
#include "ModelBatch.hpp"
#include <array>

/// @brief Preserve buildings that still occupy a visible part of the screen without refining the whole world.
namespace BuildingLod
{
	inline constexpr double kPreloadDistance = 2200;
	/// @brief Full models remain through normal street-to-town views; larger buildings keep detail longer.
	inline double detailDistance(double dimension) { return Max(600.0,dimension * 18.0); }
	/// @brief Only small silhouettes use the coarsest baked tier.
	inline double farDistance(double dimension) { return Max(1800.0,dimension * 60.0); }

	/// @brief A closed low-cost body and a separate roof keep the original anchor without filling garden space.
	struct Proxy
	{
		MeshData wall,roof;
		ColorF wallColor{.60,.58,.53},roofColor{.18,.20,.22};
		bool pitched = false;
	};

	/// @brief Read the actual roof profile and texture colours once per asset, then reuse at every placement.
	inline Proxy proxy(const Box& bounds,const Array<ModelMeshSource>& parts,HashTable<String,Image>& textures)
	{
		Proxy result;
		const auto size = bounds.size;
		if (Min({size.x,size.y,size.z}) <= 0) { return result; }
		const double bottom = bounds.center.y - size.y * .5,top = bounds.center.y + size.y * .5;
		double eaves = top,slopedArea = 0,wallWeight = 0,roofWeight = 0;
		Vec3 wallColor{},roofColor{};
		Vec2 ridgeMin{Math::Inf,Math::Inf},ridgeMax{-Math::Inf,-Math::Inf};
		Vec2 roofMin{Math::Inf,Math::Inf},roofMax{-Math::Inf,-Math::Inf};
		for (const auto& part : parts)
		{
			const Image* texture = nullptr;
			if (!part.material.diffuseTextureName.isEmpty())
			{
				auto found = textures.find(part.material.diffuseTextureName);
				if (found == textures.end()) { found = textures.emplace(part.material.diffuseTextureName,Image{part.material.diffuseTextureName}).first; }
				if (found->second) { texture = &found->second; }
			}
			for (const auto& triangle : part.geometry.indices)
			{
				const auto& a = part.geometry.vertices[triangle.i0];
				const auto& b = part.geometry.vertices[triangle.i1];
				const auto& c = part.geometry.vertices[triangle.i2];
				const Vec3 normal = Vec3{b.pos-a.pos}.cross(Vec3{c.pos-a.pos});
				const double area = normal.length() * .5;
				if (area < 1e-8) { continue; }
				const double up = Abs(normal.y) / (area * 2),height = (a.pos.y + b.pos.y + c.pos.y) / 3.0;
				const bool roof = up > .25 && height > bottom + size.y * .6;
				if (roof)
				{
					for (const auto* vertex : {&a,&b,&c})
					{
						roofMin.x = Min(roofMin.x,static_cast<double>(vertex->pos.x)); roofMin.y = Min(roofMin.y,static_cast<double>(vertex->pos.z));
						roofMax.x = Max(roofMax.x,static_cast<double>(vertex->pos.x)); roofMax.y = Max(roofMax.y,static_cast<double>(vertex->pos.z));
						if (up < .96 && vertex->pos.y > bottom + size.y * .55) { eaves = Min(eaves,static_cast<double>(vertex->pos.y)); }
					}
					if (up < .96) { slopedArea += area; }
				}
				for (const auto* vertex : {&a,&b,&c})
				{
					if (vertex->pos.y < top - size.y * .025) { continue; }
					ridgeMin.x = Min(ridgeMin.x,static_cast<double>(vertex->pos.x)); ridgeMin.y = Min(ridgeMin.y,static_cast<double>(vertex->pos.z));
					ridgeMax.x = Max(ridgeMax.x,static_cast<double>(vertex->pos.x)); ridgeMax.y = Max(ridgeMax.y,static_cast<double>(vertex->pos.z));
				}
				if (!roof && up > .25) { continue; }
				ColorF color = part.material.diffuse;
				if (texture)
				{
					const Float2 uv = (a.tex + b.tex + c.tex) / 3;
					const int x = Clamp(static_cast<int>((uv.x - Floor(uv.x)) * texture->width()),0,texture->width()-1);
					const int y = Clamp(static_cast<int>((uv.y - Floor(uv.y)) * texture->height()),0,texture->height()-1);
					color *= ColorF{(*texture)[y][x]}.removeSRGBCurve();
				}
				if (roof) { roofColor += Vec3{color.r,color.g,color.b} * area; roofWeight += area; }
				else { wallColor += Vec3{color.r,color.g,color.b} * area; wallWeight += area; }
			}
		}
		if (wallWeight > 0) { const Vec3 c = wallColor / wallWeight; result.wallColor = ColorF{c.x,c.y,c.z}; }
		if (roofWeight > 0) { const Vec3 c = roofColor / roofWeight; result.roofColor = ColorF{c.x,c.y,c.z}; }
		// Yard slabs and fence extents in an OBJ must not become a solid building footprint.
		const Vec2 footprint = roofWeight > 0 ? roofMax-roofMin : Vec2{size.x,size.z};
		const Vec2 center = roofWeight > 0 ? (roofMin+roofMax)*.5 : Vec2{bounds.center.x,bounds.center.z};
		result.pitched = slopedArea > footprint.x * footprint.y * .10 && top - eaves > Max(.25,size.y * .035);
		eaves = result.pitched ? Clamp(eaves,bottom+size.y*.6,top-size.y*.04) : top-Max(.15,size.y*.025);
		const float wallScale = result.pitched ? .94f : 1.0f;
		result.wall = MeshData::Box(Float3{static_cast<float>(center.x),static_cast<float>((bottom+eaves)*.5),static_cast<float>(center.y)},
			Float3{static_cast<float>(footprint.x)*wallScale,static_cast<float>(eaves-bottom),static_cast<float>(footprint.y)*wallScale});
		if (!result.pitched)
		{
			result.roof = MeshData::Box(Float3{static_cast<float>(center.x),static_cast<float>((eaves+top)*.5),static_cast<float>(center.y)},
				Float3{static_cast<float>(footprint.x),static_cast<float>(top-eaves),static_cast<float>(footprint.y)});
			return result;
		}
		const float x0 = static_cast<float>(center.x-footprint.x*.5),x1 = static_cast<float>(center.x+footprint.x*.5);
		const float z0 = static_cast<float>(center.y-footprint.y*.5),z1 = static_cast<float>(center.y+footprint.y*.5);
		const float low = static_cast<float>(eaves),high = static_cast<float>(top);
		const bool alongX = ridgeMax.x-ridgeMin.x > ridgeMax.y-ridgeMin.y;
		const std::array<Float3,6> vertices{{{x0,low,z0},{x1,low,z0},{x1,low,z1},{x0,low,z1},
			alongX ? Float3{x0,high,static_cast<float>(center.y)} : Float3{static_cast<float>(center.x),high,z0},
			alongX ? Float3{x1,high,static_cast<float>(center.y)} : Float3{static_cast<float>(center.x),high,z1}}};
		const std::array<TriangleIndex32,6> triangles{{{0,4,1},{1,4,5},{1,5,2},{2,5,3},{3,5,4},{3,4,0}}};
		for (const auto& face : triangles)
		{
			const uint32 offset = static_cast<uint32>(result.roof.vertices.size());
			const auto normal = (vertices[face.i1]-vertices[face.i0]).cross(vertices[face.i2]-vertices[face.i0]).normalized();
			for (const auto index : {face.i0,face.i1,face.i2}) { result.roof.vertices << Vertex3D{vertices[index],normal,{0,0}}; }
			result.roof.indices << TriangleIndex32{offset,offset+1,offset+2};
		}
		return result;
	}
}
