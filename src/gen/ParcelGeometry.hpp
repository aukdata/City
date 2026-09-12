#pragma once
#include <Siv3D.hpp>
#include <array>

/// @brief Shared XZ footprint geometry used by placement and its regression tests.
namespace ParcelGeometry
{
	using Quad = std::array<Vec2, 4>;
	inline Quad footprint(Vec2 center, double halfSize, double angle)
	{
		const Vec2 along{ Cos(angle) * halfSize, Sin(angle) * halfSize };
		const Vec2 inward{ -along.y, along.x };
		return { center - along - inward, center + along - inward,
			center + along + inward, center - along + inward };
	}
	/// @brief Separating-axis test; touching edges are permitted.
	inline bool overlaps(const Quad& a, const Quad& b)
	{
		for (const Quad* polygon : { &a, &b })
		{
			for (size_t i = 0; i < polygon->size(); ++i)
			{
				const Vec2 edge = (*polygon)[(i + 1) % polygon->size()] - (*polygon)[i];
				const Vec2 axis{ -edge.y, edge.x };
				if (axis.lengthSq() < 1e-10)
				{
					continue;
				}
				double minA = Math::Inf, maxA = -Math::Inf;
				double minB = Math::Inf, maxB = -Math::Inf;
				for (const Vec2& point : a)
				{
					minA = Min(minA, point.dot(axis));
					maxA = Max(maxA, point.dot(axis));
				}
				for (const Vec2& point : b)
				{
					minB = Min(minB, point.dot(axis));
					maxB = Max(maxB, point.dot(axis));
				}
				if (maxA <= minB || maxB <= minA)
				{
					return false;
				}
			}
		}
		return true;
	}
}
