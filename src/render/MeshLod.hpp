#pragma once
#include <Siv3D.hpp>

/// @brief 手続き生成モデルの遠景用頂点集約。面の向きと材質単位を維持する。
inline MeshData simplifyMesh(const MeshData& source, float cellSize)
{
	MeshData result;
	HashTable<uint64, uint32> clusters;
	Array<uint32> remap; remap.reserve(source.vertices.size());
	constexpr uint64 kMask = (1ULL << 20) - 1;
	for (const auto& vertex : source.vertices)
	{
		const auto cell = [&](float coordinate) { return static_cast<uint64>(static_cast<int64>(Floor(coordinate / cellSize))) & kMask; };
		const auto& normal = vertex.normal;
		const int axis = Abs(normal.x) > Abs(normal.y) && Abs(normal.x) > Abs(normal.z) ? 0 : (Abs(normal.y) > Abs(normal.z) ? 1 : 2);
		const float component = axis == 0 ? normal.x : (axis == 1 ? normal.y : normal.z);
		const uint64 face = static_cast<uint64>(axis * 2 + (component < 0));
		const uint64 key = cell(vertex.pos.x) | (cell(vertex.pos.y) << 20) | (cell(vertex.pos.z) << 40) | (face << 60);
		const auto [it, inserted] = clusters.try_emplace(key, static_cast<uint32>(result.vertices.size()));
		if (inserted) { result.vertices << vertex; }
		remap << it->second;
	}
	for (const auto& triangle : source.indices)
	{
		const uint32 a = remap[triangle.i0], b = remap[triangle.i1], c = remap[triangle.i2];
		if (a != b && b != c && c != a) { result.indices << TriangleIndex32{a, b, c}; }
	}
	return result;
}
