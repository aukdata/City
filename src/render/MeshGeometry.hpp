#pragma once
#include <Siv3D.hpp>

/// @brief 道路と建物で共有する CPU メッシュの結合・回転。
namespace MeshGeometry
{
	/// @brief 頂点を追加し、追加面のインデックスを既存頂点数だけずらす。
	inline void appendMeshData(MeshData& dst, const MeshData& src)
	{
		const uint32 offset = static_cast<uint32>(dst.vertices.size());
		dst.vertices.append(src.vertices);
		for (const auto& tri : src.indices)
		{
			dst.indices << TriangleIndex32{ tri.i0 + offset, tri.i1 + offset, tri.i2 + offset };
		}
	}

	/// @brief 指定した XZ 中心の周りで位置と法線を同じ角度だけ回転する。
	inline void rotateMeshY(MeshData& mesh, float cx, float cz, float angle)
	{
		if (angle == 0.0f) { return; }
		const float cosA = Math::Cos(angle);
		const float sinA = Math::Sin(angle);
		for (auto& v : mesh.vertices)
		{
			const float dx = v.pos.x - cx;
			const float dz = v.pos.z - cz;
			v.pos.x = cx + dx * cosA - dz * sinA;
			v.pos.z = cz + dx * sinA + dz * cosA;
			const float nx = v.normal.x;
			const float nz = v.normal.z;
			v.normal.x = nx * cosA - nz * sinA;
			v.normal.z = nx * sinA + nz * cosA;
		}
	}

	/// @brief 中心・寸法・向きから箱を生成し、既存メッシュへ追加する。
	inline void appendOrientedBox(MeshData& dst, const Vec3& center, const Float3& size, float angle)
	{
		MeshData box = MeshData::Box(Float3{ static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z) }, size);
		rotateMeshY(box, static_cast<float>(center.x), static_cast<float>(center.z), angle);
		appendMeshData(dst, box);
	}

}
