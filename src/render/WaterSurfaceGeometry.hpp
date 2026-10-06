#pragma once
#include <Siv3D.hpp>
#include "BridgeStructure.hpp"
#include "../gen/RiverNetwork.hpp"

/// @brief Render the existing water topology at its physical source datum.
/// @details Endpoint discs remain the legacy topology; this helper does not certify their overlap or ownership.
namespace WaterSurfaceGeometry
{
	/// @brief Append the existing reach quad and endpoint discs without a geometric water-height bias.
	inline void appendRiverReach(MeshData& water, const RiverNetwork::Reach& reach)
	{
		const Vec3 delta = reach.end - reach.start;
		if (Vec2{delta.x, delta.z}.lengthSq() < 1) { return; }
		const Vec3 right = Vec3{delta.z, 0, -delta.x}.normalized();
		const Vec3 a = reach.start, b = reach.end;
		BridgeStructure::quad(water, a - right * reach.halfWidth, b - right * reach.endHalfWidth,
			b + right * reach.endHalfWidth, a + right * reach.halfWidth);
		for (const Vec3 center : {a, b}) for (int i = 0; i < 16; ++i)
		{
			const double t = i * Math::TwoPi / 16, u = (i + 1) * Math::TwoPi / 16;
			const double width = center == a ? reach.halfWidth : reach.endHalfWidth;
			const uint32 n = static_cast<uint32>(water.vertices.size());
			for (const Vec3 p : {center, center + Vec3{Cos(u), 0, Sin(u)} * width, center + Vec3{Cos(t), 0, Sin(t)} * width})
			{
				water.vertices << Vertex3D{Float3{p}, Float3{0, 1, 0}, Float2{static_cast<float>(p.x * .1), static_cast<float>(p.z * .1)}};
			}
			water.indices << TriangleIndex32{n, n + 1, n + 2};
		}
	}

	/// @brief Keep the existing sea box thickness below nominal sea level; both render paths have top Y = 0.
	inline Box seaBox(Vec2 centerXZ, Vec2 extentXZ)
	{
		constexpr double thickness = .04;
		return Box{Vec3{centerXZ.x, -thickness * .5, centerXZ.y}, Vec3{extentXZ.x, thickness, extentXZ.y}};
	}
}
