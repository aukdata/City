#pragma once
#include "TunnelGeometry.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../railway/TrainNetwork.hpp"
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
		const auto append=[&](const CubicBezier& curve,double width,bool railwayTrack)
		{
			auto geometry=TunnelGeometry::build(curve,world,width,railwayTrack);
			if (geometry.openings.isEmpty()) { return false; }
			openings.append(geometry.openings);
			Batch batch;batch.center=curve.positionAt(curve.totalLength*.5f);batch.radius=curve.totalLength*.5;
			if (!geometry.lining.indices.isEmpty()) { batch.lining=Mesh{geometry.lining}; }
			if (!geometry.lights.indices.isEmpty()) { batch.lights=Mesh{geometry.lights}; }
			m_batches<<std::move(batch);return true;
		};
		for (auto& section : TunnelGeometry::buildRoadNetwork(world,roads))
		{
			openings.append(section.geometry.openings);Batch batch;batch.center=section.center;batch.radius=section.radius;
			if (!section.geometry.lining.indices.isEmpty()) { batch.lining=Mesh{section.geometry.lining}; }
			if (!section.geometry.lights.indices.isEmpty()) { batch.lights=Mesh{section.geometry.lights}; }
			m_batches<<std::move(batch);++roadCount;
		}
		for (const auto& edge : railway.edges()) { if (edge.id>=0) { if (const auto curve=railway.getBezier(edge.id)) { railCount+=append(*curve,4.8,true); } } }
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
