#pragma once
#include "ParcelGeometry.hpp"
#include "../road/RoadNetwork.hpp"
#include "../road/RoadGeometry.hpp"

/// @brief Spatial index of ground-level road ribbons used to protect every side of a lot.
class ParcelRoadIndex
{
public:
	explicit ParcelRoadIndex(const RoadNetwork& network)
	{
		constexpr float kSampleLength = 4.0f;
		for (const RoadEdge& edge : network.edges())
		{
			if (edge.id < 0 || !edge.isRoadbedBuilt() || edge.useElevation)
			{
				continue;
			}
			const auto bezier = network.getBezier(edge.id);
			if (!bezier || bezier->totalLength < 0.1f)
			{
				continue;
			}
			const int count = Max(1, static_cast<int>(Ceil(bezier->totalLength / kSampleLength)));
			Vec2 previousLeft{ 0, 0 }, previousRight{ 0, 0 };
			bool hasPrevious = false;
			for (int sample = 0; sample <= count; ++sample)
			{
				const float fraction = static_cast<float>(sample) / count;
				const float arc = bezier->totalLength * fraction;
				const Vec3 position = bezier->positionAt(arc);
				const Vec3 right3 = tangentToRight(bezier->tangentAt(arc));
				const Vec2 right{ right3.x, right3.z };
				const auto range = RoadGeometry::structuralRangeAt(edge, fraction);
				if (!range.valid)
				{
					hasPrevious = false;
					continue;
				}
				constexpr float kRoadMargin = 0.35f;
				const Vec2 center{ position.x, position.z };
				const Vec2 leftPoint = center + right * (range.left - kRoadMargin);
				const Vec2 rightPoint = center + right * (range.right + kRoadMargin);
				if (hasPrevious)
				{
					const ParcelGeometry::Quad quad{ previousLeft, leftPoint, rightPoint, previousRight };
					visitBuckets(quad, [&](int64 key) { m_buckets[key] << quad; });
				}
				previousLeft = leftPoint;
				previousRight = rightPoint;
				hasPrevious = true;
			}
		}
	}

	[[nodiscard]] bool overlaps(const ParcelGeometry::Quad& footprint) const
	{
		bool collision = false;
		visitBuckets(footprint, [&](int64 key)
		{
			const auto bucket = m_buckets.find(key);
			if (collision || bucket == m_buckets.end())
			{
				return;
			}
			for (const auto& road : bucket->second)
			{
				if (ParcelGeometry::overlaps(footprint, road))
				{
					collision = true;
					return;
				}
			}
		});
		return collision;
	}

private:
	template <class Visitor>
	static void visitBuckets(const ParcelGeometry::Quad& quad, Visitor visitor)
	{
		constexpr double kBucketSize = 64.0;
		double minX = quad[0].x, maxX = minX, minZ = quad[0].y, maxZ = minZ;
		for (const Vec2& point : quad)
		{
			minX = Min(minX, point.x); maxX = Max(maxX, point.x);
			minZ = Min(minZ, point.y); maxZ = Max(maxZ, point.y);
		}
		for (int z = static_cast<int>(Floor(minZ / kBucketSize)); z <= static_cast<int>(Floor(maxZ / kBucketSize)); ++z)
		{
			for (int x = static_cast<int>(Floor(minX / kBucketSize)); x <= static_cast<int>(Floor(maxX / kBucketSize)); ++x)
			{
				const int64 key = static_cast<int64>((static_cast<uint64>(static_cast<uint32>(x)) << 32) | static_cast<uint32>(z));
				visitor(key);
			}
		}
	}
	HashTable<int64, Array<ParcelGeometry::Quad>> m_buckets;
};
