#pragma once
#include <Siv3D.hpp>
#include "../world/World.hpp"
#include "TerrainSurfaceGeometry.hpp"
#include "TerrainMaterials.hpp"

/// @brief 地域全体を見渡す高度で使う地形。近傍チャンクの外側も海岸線・山脈を保つ。
namespace RegionalTerrain
{
	constexpr int kCells=512;
	constexpr double kMinimumClearance=4500;
	inline MeshData build(const World& world)
	{
		const float extent=static_cast<float>(WORLD_SIZE);
		auto data=MeshData::Grid(Float3{extent*.5f,0,extent*.5f},Float2{extent,extent},kCells,kCells,Float2{extent/32,extent/32});
		const auto forest=VegetationProfile::ForestCover::current();
		for(auto& vertex:data.vertices)
		{
			vertex.pos.y=world.sampleHeight(Clamp(vertex.pos.x,0.0f,extent-.01f),Clamp(vertex.pos.z,0.0f,extent-.01f));
			vertex.tex=Float2{TerrainMaterials::forestCover(forest.get(),Vec3{vertex.pos}),0};
		}
		data.computeNormals();return data;
	}
	struct Batch {int materialKey=0;MeshData data;};
	/// @brief 詳細地形と同じ標高分類で三角形をまとめる。頂点は各材質内で再利用する。
	inline Array<Batch> batches(const MeshData& source)
	{
		Array<Batch> result{{0,{}},{1,{}}};
		std::array<HashTable<uint32,uint32>,2> remap;
		for(const auto& triangle:source.indices)
		{
			const float height=(source.vertices[triangle.i0].pos.y+source.vertices[triangle.i1].pos.y+source.vertices[triangle.i2].pos.y)/3;
			const int key=TerrainMaterials::keyForHeight(height);auto& target=result[key].data;
			const auto index=[&](uint32 old)
			{
				if(const auto found=remap[key].find(old);found!=remap[key].end()) { return found->second; }
				const auto next=static_cast<uint32>(target.vertices.size());target.vertices<<source.vertices[old];remap[key][old]=next;return next;
			};
			target.indices << TriangleIndex32{index(triangle.i0),index(triangle.i1),index(triangle.i2)};
		}
		return result;
	}
	/// @brief 地上視点の遠景。詳細チャンクが描く範囲の三角形を除き、周囲の山並みだけを残す。
	/// @details 格子 128m はチャンク 1024m の約数なので、除外境界はチャンク境界と一致する。海面下だけの三角形は省く。
	inline Array<Batch> backdrop(const MeshData& source, const HashSet<Point>& excluded)
	{
		MeshData kept;
		kept.vertices = source.vertices;
		for (const auto& triangle : source.indices)
		{
			const auto& a = source.vertices[triangle.i0].pos; const auto& b = source.vertices[triangle.i1].pos; const auto& c = source.vertices[triangle.i2].pos;
			if (Max({ a.y, b.y, c.y }) <= 0) { continue; }
			const Point chunk{ static_cast<int>(Math::Floor((a.x + b.x + c.x) / (3 * CHUNK_SIZE))), static_cast<int>(Math::Floor((a.z + b.z + c.z) / (3 * CHUNK_SIZE))) };
			if (!excluded.contains(chunk)) { kept.indices << triangle; }
		}
		return batches(kept);
	}
}
