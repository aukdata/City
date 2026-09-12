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
			const double crown=railwayTrack ? 6.5 : 5.8,half=width*.5+.7;
			MeshData lining,lights; bool found=false;
			const int count=Max(2,static_cast<int>(std::ceil(curve.totalLength/8)));
			for (int i=0;i<count;++i)
			{
				const float start=curve.totalLength*i/count,end=curve.totalLength*(i+1)/count;
				const Vec3 a=curve.positionAt(start),b=curve.positionAt(end),middle=(a+b)*.5;
				const double depth=world.sampleHeight(static_cast<float>(middle.x),static_cast<float>(middle.z))-middle.y;
				if (depth<1) { continue; }
				const Vec3 rightA=tangentToRight(curve.tangentAt(start)),rightB=tangentToRight(curve.tangentAt(end));
				found=true; BridgeStructure::append(lining,TunnelGeometry::lining(a,b,rightA,rightB,half,crown,depth<crown+1));
				if (depth<crown+1)
				{
					TunnelGeometry::Opening opening; const Vec3 along=(b-a).normalized();
					for (const Vec3 p : {a-along*1.0-rightA*half,b+along*1.0-rightB*half,b+along*1.0+rightB*half,a-along*1.0+rightA*half}) { opening.footprint << Vec2{p.x,p.z}; }
					double signedArea=0;for (size_t j=0;j<opening.footprint.size();++j) { const Vec2 p=opening.footprint[j],q=opening.footprint[(j+1)%opening.footprint.size()];signedArea+=p.x*q.y-q.x*p.y; }
					if (signedArea<0) { opening.footprint.reverse(); }
					Vec2 low{1e9,1e9},high{-1e9,-1e9}; for (const auto p : opening.footprint) { low.x=Min(low.x,p.x);low.y=Min(low.y,p.y);high.x=Max(high.x,p.x);high.y=Max(high.y,p.y); }
					opening.bounds=RectF{low,high-low}; opening.floor=static_cast<float>(Min(a.y,b.y)-.3); openings << std::move(opening);
				}
				if (i%3==0)
				{
					for (const int side : {-1,1})
					{
						const Vec3 p=a+rightA*(side*(half-.22))+Vec3{0,3.0,0};
						BridgeStructure::append(lights,MeshData::Box(Float3{p},Float3{.15,.22,1.5}));
					}
				}
			}
			if (!found) { return false; }
			Batch batch; batch.center=curve.positionAt(curve.totalLength*.5f); batch.radius=curve.totalLength*.5; batch.lining=Mesh{lining};
			if (!lights.indices.isEmpty()) { batch.lights=Mesh{lights}; } m_batches << std::move(batch); return true;
		};
		for (const auto& edge : roads.edges()) { if (edge.id>=0 && edge.useElevation && edge.isRoadbedBuilt()) { if (const auto curve=roads.getBezier(edge.id)) { roadCount+=append(*curve,edge.totalWidth(),false); } } }
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
