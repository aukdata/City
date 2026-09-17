#pragma once
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../railway/TrainNetwork.hpp"

/// @brief 高さの近い歩行面を選択し、高架下やトンネルで別階層へ飛ばない。
class WalkSurface
{
public:
	double resolve(Vec3 point,const World& world,const RoadNetwork& roads,const TrainNetwork& railway)
	{
		const Point cell{static_cast<int>(Floor(point.x/64)),static_cast<int>(Floor(point.z/64))};
		const size_t frame=Scene::FrameCount()/60;
		if (cell!=m_cell || roads.edges().size()!=m_roadCount || railway.edges().size()!=m_railCount || frame!=m_frame)
		{
			m_cell=cell;m_roadCount=roads.edges().size();m_railCount=railway.edges().size();m_frame=frame;
			m_roads.clear();m_rails.clear();
			const RectF region{cell.x*64-40,cell.y*64-40,144,144};
			const auto overlaps=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d)
			{
				Vec2 low{a.x,a.z},high=low;
				for (const Vec3 p : {b,c,d}) { low.x=Min(low.x,p.x);low.y=Min(low.y,p.z);high.x=Max(high.x,p.x);high.y=Max(high.y,p.z); }
				return RectF{low,high-low}.stretched(1).intersects(region);
			};
			for (const auto& edge : roads.edges())
			{
				if (edge.id>=0 && overlaps(roads.getNode(edge.nodeA)->position,roads.getNode(edge.nodeB)->position,edge.ctrlA,edge.ctrlB)) { m_roads<<edge.id; }
			}
			for (const auto& edge : railway.edges())
			{
				if (edge.id>=0 && overlaps(railway.getNode(edge.nodeA)->position,railway.getNode(edge.nodeB)->position,edge.ctrlA,edge.ctrlB)) { m_rails<<edge.id; }
			}
		}
		const double terrain=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
		double result=terrain,best=2.5;
		const auto consider=[&](const CubicBezier& curve,double half,bool elevated,double lift)
		{
			const auto distance=[&](float t) { const Vec3 p=curve.evaluate(t); return Vec2{p.x-point.x,p.z-point.z}.lengthSq(); };
			int index=0;double nearest=distance(0);
			for (int i=1;i<=32;++i) { const double d=distance(i/32.0f);if (d<nearest) { nearest=d;index=i; } }
			float low=Max(0,index-1)/32.0f,high=Min(32,index+1)/32.0f;
			for (int i=0;i<20;++i) { const float a=low+(high-low)/3,b=high-(high-low)/3; if (distance(a)<distance(b)) { high=b; } else { low=a; } }
			const float fraction=(low+high)*.5f; if (distance(fraction)>half*half) { return; }
			const Vec3 p=curve.evaluate(fraction);
			const double surface=(elevated ? p.y : world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)))+lift;
			const double delta=Abs(surface-point.y);
			if (delta<best) { best=delta;result=surface; }
		};
		for (const int id : m_roads)
		{
			const auto* edge=roads.getEdge(id);
			if (!edge || !edge->isRoadbedBuilt()) { continue; }
			if (const auto curve=roads.getBezier(id)) { consider(*curve,edge->totalWidth()*.5,edge->usesDesignHeight(),kRoadSurfaceLift); }
		}
		for (const int id : m_rails) { if (const auto curve=railway.getBezier(id)) { consider(*curve,2.35,true,.15); } }
		return result;
	}
private:
	Point m_cell{-999,-999}; size_t m_roadCount=0,m_railCount=0,m_frame=0;
	Array<int> m_roads,m_rails;
};
