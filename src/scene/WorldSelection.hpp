#pragma once
#include <Siv3D.hpp>

/// @brief Depth arbitration for the existing ground-proximity infrastructure candidates.
namespace WorldSelection
{
	enum class Surface { None, GuideSign, Signal, Building };

	/// @brief Keep forgiving ground picks, but a roof blocks infrastructure unless its visible mesh is nearer.
	[[nodiscard]] inline Surface choose(Optional<double> buildingDistance, bool guideCandidate,
		Optional<double> guideDistance, bool signalCandidate, Optional<double> signalDistance)
	{
		if (!buildingDistance)
		{
			if (guideCandidate) { return Surface::GuideSign; }
			if (signalCandidate) { return Surface::Signal; }
			return Surface::None;
		}
		Surface result = Surface::Building;
		double nearest = *buildingDistance;
		if (guideCandidate && guideDistance && *guideDistance <= nearest)
		{
			nearest = *guideDistance;
			result = Surface::GuideSign;
		}
		if (signalCandidate && signalDistance && *signalDistance < nearest)
		{
			result = Surface::Signal;
		}
		return result;
	}

	/// @brief Nearest hit on the same transformed CPU triangles used to create the rendered mesh.
	[[nodiscard]] inline Optional<double> meshDistance(const Ray& ray, const Array<Vertex3D>& vertices,
		const Array<TriangleIndex32>& indices, const Mat4x4& transform)
	{
		Optional<double> nearest;
		for (const auto& triangle : indices)
		{
			const Triangle3D transformed{
				transform.transformPoint(vertices[triangle.i0].pos),
				transform.transformPoint(vertices[triangle.i1].pos),
				transform.transformPoint(vertices[triangle.i2].pos)};
			if (const auto distance = ray.intersects(transformed); distance && (!nearest || *distance < *nearest))
			{
				nearest = *distance;
			}
		}
		return nearest;
	}
}
