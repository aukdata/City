#pragma once
#include "GenerationSettings.hpp"
#include "ParcelGeometry.hpp"
#include "../road/RoadNetwork.hpp"
#include "../road/RoadGeometry.hpp"
#include "../railway/TrainNetwork.hpp"
#include "../railway/RailwaySite.hpp"

/// @brief Canonical road-clearance quads shared by placement and developability diagnostics.
namespace ParcelRoadGeometry
{
	template <class Visitor>
	inline void forEachRibbon(const RoadEdge& edge,const CubicBezier& bezier,Visitor&& visit)
	{
		constexpr float kSampleLength=4.0f;
		const int count=Max(1,static_cast<int>(Ceil(bezier.totalLength/kSampleLength)));
		Vec2 previousLeft{0,0},previousRight{0,0}; bool hasPrevious=false;
		for (int sample=0;sample<=count;++sample)
		{
			const float fraction=static_cast<float>(sample)/count;
			const float arc=bezier.totalLength*fraction;
			const Vec3 position=bezier.positionAt(arc),right3=tangentToRight(bezier.tangentAt(arc));
			const Vec2 right{right3.x,right3.z};
			const auto range=RoadGeometry::structuralRangeAt(edge,fraction);
			if (!range.valid) { hasPrevious=false; continue; }
			const float margin=GenerationSettings::get().parcels_roadMargin;
			const Vec2 center{position.x,position.z};
			const Vec2 leftPoint=center+right*(range.left-margin),rightPoint=center+right*(range.right+margin);
			if (hasPrevious) { visit(ParcelGeometry::Quad{previousLeft,leftPoint,rightPoint,previousRight}); }
			previousLeft=leftPoint; previousRight=rightPoint; hasPrevious=true;
		}
	}
}

/// @brief Spatial index of ground-level road ribbons used to protect every side of a lot.
class ParcelRoadIndex
{
public:
	explicit ParcelRoadIndex(const RoadNetwork& network,bool includeElevated=false,bool includeRailway=true)
	{
		for (const RoadEdge& edge : network.edges())
		{
			if ((!includeRailway && !edge.hasRoadLanes()) || edge.id < 0 || !edge.isRoadbedBuilt() || (edge.useElevation && !includeElevated))
			{
				continue;
			}
			const auto bezier = network.getBezier(edge.id);
			if (!bezier || bezier->totalLength < 0.1f)
			{
				continue;
			}
			ParcelRoadGeometry::forEachRibbon(edge,*bezier,[&](const ParcelGeometry::Quad& quad)
			{
				visitBuckets(quad,[&](int64 key) { m_buckets[key] << quad; });
			});
		}
	}

	void addRailway(const TrainNetwork& network)
	{
		for (const auto& site : RailwaySite::footprints(network))
		{
			visitBuckets(site,[&](int64 key) { m_buckets[key] << site; });
		}
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			Vec2 previousLeft{0, 0}, previousRight{0, 0};
			const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/8)));
			for (int i=0;i<=count;++i)
			{
				const float arc=curve->totalLength*i/count;
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				const Vec2 left{point.x-right.x*5,point.z-right.z*5},other{point.x+right.x*5,point.z+right.z*5};
				if (i>0)
				{
					const ParcelGeometry::Quad quad{previousLeft,left,other,previousRight};
					visitBuckets(quad,[&](int64 key) { m_buckets[key] << quad; });
				}
				previousLeft=left; previousRight=other;
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

	/// @brief Visit the exact registered validator obstacles near a query, without bucket duplicates.
	template <class Visitor>
	void forEachNearby(const RectF& bounds,Visitor&& visitor) const
	{
		Array<ParcelGeometry::Quad> visited;
		const ParcelGeometry::Quad query{bounds.tl(),bounds.tr(),bounds.br(),bounds.bl()};
		visitBuckets(query,[&](int64 key)
		{
			const auto bucket=m_buckets.find(key); if (bucket==m_buckets.end()) { return; }
			for (const auto& quad : bucket->second)
			{
				if (visited.contains(quad)) { continue; }
				visited << quad; visitor(quad);
			}
		});
	}
	/// @brief 生成中に追加した接道面・敷地を同じ空間索引へ登録する。
	void add(const ParcelGeometry::Quad& quad) { visitBuckets(quad,[&](int64 key){m_buckets[key] << quad;}); }

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
