#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/BezierUtil.hpp"

namespace
{
	struct EdgeProjection
	{
		float arc = 0.0f;
		Vec3 position{ 0.0, 0.0, 0.0 };
		float distance = 0.0f;
	};

	EdgeProjection projectPointToBezierXZ(const CubicBezier& bezier, const Vec2& point)
	{
		EdgeProjection result;
		double bestDistanceSquared = Math::Inf;
		int bestIndex = 0;
		constexpr int kSamples = 96;
		for (int index = 0; index <= kSamples; ++index)
		{
			const float arc = bezier.totalLength * (static_cast<float>(index) / kSamples);
			const Vec3 position = bezier.positionAt(arc);
			const double dx = position.x - point.x;
			const double dz = position.z - point.y;
			const double distanceSquared = dx * dx + dz * dz;
			if (distanceSquared < bestDistanceSquared)
			{
				bestDistanceSquared = distanceSquared;
				bestIndex = index;
			}
		}

		float lower = bezier.totalLength * (Max(bestIndex - 1, 0) / static_cast<float>(kSamples));
		float upper = bezier.totalLength * (Min(bestIndex + 1, kSamples) / static_cast<float>(kSamples));
		constexpr int kRefinementIterations = 24;
		for (int iteration = 0; iteration < kRefinementIterations; ++iteration)
		{
			const float firstArc = lower + (upper - lower) / 3.0f;
			const float secondArc = upper - (upper - lower) / 3.0f;
			const Vec3 firstPosition = bezier.positionAt(firstArc);
			const Vec3 secondPosition = bezier.positionAt(secondArc);
			const double firstDx = firstPosition.x - point.x;
			const double firstDz = firstPosition.z - point.y;
			const double secondDx = secondPosition.x - point.x;
			const double secondDz = secondPosition.z - point.y;
			const double firstDistance = firstDx * firstDx + firstDz * firstDz;
			const double secondDistance = secondDx * secondDx + secondDz * secondDz;
			if (firstDistance < secondDistance)
			{
				upper = secondArc;
			}
			else
			{
				lower = firstArc;
			}
		}

		result.arc = (lower + upper) * 0.5f;
		result.position = bezier.positionAt(result.arc);
		result.distance = static_cast<float>(
			Vec2{ result.position.x, result.position.z }.distanceFrom(point));
		return result;
	}
}

void registerBezierBuildingFrontageTests(TestRunner& runner)
{
	runner.add(U"Bezier.BuildingFrontageUsesClosestPoint", [](TestContext& context)
	{
		const CubicBezier bezier{
			Vec3{ 100, 0, 120 },
			Vec3{ 230, 0, 330 },
			Vec3{ 430, 0, 10 },
			Vec3{ 560, 0, 220 }
		};
		constexpr float kLegacySampleParameter = 0.25f;
		const Vec2 buildingCenter{ 210, 150 };
		const Vec3 legacyPosition = bezier.positionAt(bezier.totalLength * kLegacySampleParameter);
		const double legacyDistance =
			Vec2{ legacyPosition.x, legacyPosition.z }.distanceFrom(buildingCenter);
		const EdgeProjection projected = projectPointToBezierXZ(bezier, buildingCenter);

		context.expect(projected.distance < legacyDistance,
			U"最近傍点は固定t=0.25の点より建物中心に近い必要があります");
		context.expect(projected.arc > 0.0f && projected.arc < bezier.totalLength,
			U"最近傍点はこの再現ケースではベジェ曲線内部にあります");
	});
}
