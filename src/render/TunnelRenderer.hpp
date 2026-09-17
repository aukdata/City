#pragma once
#include "TunnelGeometry.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../railway/TrainNetwork.hpp"
#include "../railway/RailwaySite.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @brief 地中区間の覆工・坑門・照明と、坑口だけの地形開口を同じ断面から構成する。
class TunnelRenderer
{
public:
	Array<TunnelGeometry::Opening> openings;
	bool dirty=true;
	void build(const World& world,const RoadNetwork& roads,const TrainNetwork& railway)
	{
		m_batches.clear(); openings.clear(); int roadCount=0,railCount=0;
		const auto append =
			[&](const CubicBezier& curve, double width, bool railwayTrack, bool stationA = false, bool stationB = false)
		{
			auto geometry = TunnelGeometry::build(curve, world, width, railwayTrack, stationA, stationB);
			if (geometry.openings.isEmpty())
			{
				return false;
			}
			openings.append(geometry.openings);
			Batch batch;
			batch.center = curve.positionAt(curve.totalLength * .5f);
			batch.radius = curve.totalLength * .5;
			if (!geometry.lining.indices.isEmpty())
			{
				batch.lining = Mesh{geometry.lining};
			}
			if (!geometry.lights.indices.isEmpty())
			{
				batch.lights = Mesh{geometry.lights};
			}
			m_batches<<std::move(batch);
			return true;
		};
		for (auto& section : TunnelGeometry::buildRoadNetwork(world,roads))
		{
			openings.append(section.geometry.openings);Batch batch;batch.center=section.center;batch.radius=section.radius;
			if (!section.geometry.lining.indices.isEmpty()) { batch.lining=Mesh{section.geometry.lining}; }
			if (!section.geometry.lights.indices.isEmpty()) { batch.lights=Mesh{section.geometry.lights}; }
			m_batches<<std::move(batch);++roadCount;
		}
		for (const auto& edge : railway.edges())
		{
			if (edge.id >= 0 && !edge.hasRoadLanes() && edge.isRoadbedBuilt() &&
				(edge.edgeState == EdgeState::Open || edge.edgeState == EdgeState::Existing))
			{
				if (const auto curve = railway.getBezier(edge.id))
				{
					railCount += append(*curve, edge.totalWidth(), true,
						railway.getNode(edge.nodeA)->stationKind == StationKind::Underground,
						railway.getNode(edge.nodeB)->stationKind == StationKind::Underground);
				}
			}
		}
		// 地下駅の階段口だけを地表に開ける。地下ホーム全体を地表の穴にはしない。
		for (const auto& node : railway.nodes())
		{
			if (node.stationKind != StationKind::Underground || !node.entrance)
			{
				continue;
			}
			const auto frame = RailwaySite::stationFrame(railway, node.id);
			if (!frame)
			{
				continue;
			}
			const RailwaySite::Frame surface{*node.entrance, frame->along, frame->right};
			TunnelGeometry::Opening opening;
			const auto footprint = RailwaySite::rectangle(surface, -2.4, 1.2, -3.8, 3.8);
			Vec2 low{Math::Inf, Math::Inf}, high{-Math::Inf, -Math::Inf};
			for (const auto corner : footprint)
			{
				opening.footprint << corner;
				low.x = Min(low.x, corner.x);
				low.y = Min(low.y, corner.y);
				high.x = Max(high.x, corner.x);
				high.y = Max(high.y, corner.y);
			}
			opening.bounds = {low, high - low};
			opening.floor = static_cast<float>(surface.origin.y - 3.2);
			const Vec3 center = surface.point(-.6, -1.5, 0), up{0, 1, 0};
			for (const auto normal : {surface.right, -surface.right, surface.along, -surface.along, up, -up})
			{
				const double reach = Abs(normal.dot(up)) > .5 ? 1.7 : (Abs(normal.dot(surface.right)) > .5 ? 1.8 : 3.8);
				opening.planes << MeshBoolean::HalfSpace{normal, normal.dot(center) + reach};
			}
			openings << std::move(opening);
		}
		dirty=false; DBG_LOG(U"[TunnelRenderer] roadSections={} railSections={} mouthOpenings={}"_fmt(roadCount,railCount,openings.size()));
	}
	void draw(Vec3 eye) const
	{
		for (const auto& batch : m_batches)
		{
			if (batch.center.distanceFrom(eye)>4000+batch.radius) { continue; }
			batch.lining.draw(TextureAsset(Asset::Concrete),ColorF{.54,.53,.50}.removeSRGBCurve());
			if (!batch.lights.isEmpty()) { batch.lights.draw(ColorF{1,.92,.68}); }
		}
	}
private:
	struct Batch { Vec3 center; double radius=0; Mesh lining,lights; };
	Array<Batch> m_batches;
};
