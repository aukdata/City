#pragma once
#include <Siv3D.hpp>
#include <queue>
#include "../debug/DebugLog.hpp"

/// @brief 流域の集水・合流・下流への河床低下を共有する河川網。単位はm。
class RiverNetwork
{
public:
	struct Reach { Vec3 start,end; double halfWidth=20; double barSide=0; double catchment=0; RectF bounds; };
	struct Sample { Vec2 center; double distance=1e30; double surface=0; double halfWidth=0; int reach=-1; };
	Array<Reach> reaches;
	template<class Height>
	void generate(double width,double depth,const Height& height)
	{
		reaches.clear(); m_index.clear();
		constexpr double spacing=256;
		const int columns=static_cast<int>(width/spacing)+1,rows=static_cast<int>(depth/spacing)+1,count=columns*rows;
		Array<double> ground(count),filled(count),water(count); Array<int> parent(count,-1),order,flow(count,1),mainChild(count,-1);
		Array<bool> visited(count,false);
		using Entry=std::pair<double,int>;
		std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
		const auto position=[&](int id) { return Vec2{(id%columns)*spacing,(id/columns)*spacing}; };
		for (int id=0;id<count;++id)
		{
			const Vec2 point=position(id); ground[id]=height(point.x,point.y); water[id]=Max(0.0,ground[id]-.8);
			const bool border=id%columns==0 || id%columns==columns-1 || id/columns==0 || id/columns==rows-1;
			if (border || ground[id]<=0) { visited[id]=true; filled[id]=Max(0.0,ground[id]); pending.emplace(filled[id],id); }
		}
		// Priority flood creates a drainage tree through depressions; it does not let streams terminate uphill.
		while (!pending.empty())
		{
			const auto [level,id]=pending.top(); pending.pop(); order << id;
			for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
			{
				if (dx==0 && dz==0) { continue; }
				const int x=id%columns+dx,z=id/columns+dz; if (x<0 || x>=columns || z<0 || z>=rows) { continue; }
				const int next=z*columns+x; if (visited[next]) { continue; }
				visited[next]=true; parent[next]=id; filled[next]=Max(ground[next],level+.0001*spacing); pending.emplace(filled[next],next);
			}
		}
		for (auto it=order.rbegin();it!=order.rend();++it)
		{
			const int id=*it,next=parent[id]; if (next<0) { continue; }
			flow[next]+=flow[id]; if (mainChild[next]<0 || flow[id]>flow[mainChild[next]]) { mainChild[next]=id; }
		}
		constexpr int threshold=100;
		Array<Vec2> centers(count);
		for (int id=0;id<count;++id)
		{
			centers[id]=position(id);
			if (flow[id]>=threshold && parent[id]>=0 && mainChild[id]>=0 && ground[id]>2)
			{
				centers[id]=position(id)*.5+(position(parent[id])+position(mainChild[id]))*.25;
			}
		}
		// Limit water to the real valley bottom, then breach downstream sills instead of raising whole valleys.
		for (int id=0;id<count;++id)
		{
			const int next=parent[id]; if (next<0 || flow[id]<threshold) { continue; }
			double low=1e9;
			for (int sample=0;sample<=16;++sample) { const Vec2 point=centers[id].lerp(centers[next],sample/16.0); low=Min(low,static_cast<double>(height(point.x,point.y))); }
			water[id]=Min(water[id],Max(0.0,low-.7)); water[next]=Min(water[next],Max(0.0,low-.7));
		}
		for (auto it=order.rbegin();it!=order.rend();++it)
		{
			const int id=*it,next=parent[id]; if (next<0 || flow[id]<threshold) { continue; }
			water[next]=Min(water[next],Max(0.0,water[id]-.0003*centers[id].distanceFrom(centers[next])));
		}
		double length=0,maxCut=0; int confluences=0;
		for (int id=0;id<count;++id)
		{
			const int next=parent[id]; if (next<0 || flow[id]<threshold || ground[id]<.1) { continue; }
			Reach reach; reach.start={centers[id].x,water[id],centers[id].y}; reach.end={centers[next].x,water[next],centers[next].y};
			reach.catchment=flow[id]*spacing*spacing; reach.halfWidth=Clamp(8+std::sqrt(static_cast<double>(flow[id]))*1.1,18.0,72.0);
			if (mainChild[id]>=0)
			{
				const Vec2 incoming=(centers[id]-centers[mainChild[id]]).normalized(),outgoing=(centers[next]-centers[id]).normalized();
				const double bend=incoming.x*outgoing.y-incoming.y*outgoing.x;
				if (Abs(bend)>.08) { reach.barSide=bend>0 ? -1 : 1; }
			}
			const Vec2 lower{Min(centers[id].x,centers[next].x),Min(centers[id].y,centers[next].y)};
			const Vec2 upper{Max(centers[id].x,centers[next].x),Max(centers[id].y,centers[next].y)};
			reach.bounds=RectF{lower,upper-lower}.stretched(reach.halfWidth+80);
			const int index=static_cast<int>(reaches.size()); reaches << reach;
			for (int z=static_cast<int>(reach.bounds.y/512);z<=static_cast<int>(reach.bounds.br().y/512);++z)
				for (int x=static_cast<int>(reach.bounds.x/512);x<=static_cast<int>(reach.bounds.br().x/512);++x) { m_index[key(x,z)] << index; }
			length+=centers[id].distanceFrom(centers[next]); maxCut=Max(maxCut,ground[id]-water[id]);
			confluences+=mainChild[next]>=0 && mainChild[next]!=id && flow[mainChild[next]]>=threshold;
		}
		DBG_LOG(U"[RiverNetwork] reaches={} lengthKm={:.1f} confluences={} maximumIncision={:.1f}"_fmt(reaches.size(),length/1000,confluences,maxCut));
	}
	Sample nearest(Vec2 point) const
	{
		Sample result;
		const auto found=m_index.find(key(static_cast<int>(point.x/512),static_cast<int>(point.y/512))); if (found==m_index.end()) { return result; }
		for (const int id : found->second)
		{
			const auto& reach=reaches[id]; const Vec2 a{reach.start.x,reach.start.z},b{reach.end.x,reach.end.z},delta=b-a;
			const double t=Clamp((point-a).dot(delta)/Max(.01,delta.lengthSq()),0.0,1.0); const Vec2 center=a+delta*t;
			const double distance=point.distanceFrom(center);
			if (distance<result.distance) { result={center,distance,Math::Lerp(reach.start.y,reach.end.y,t),reach.halfWidth,id}; }
		}
		return result;
	}
	double waterLevel(Vec2 point) const { const auto sample=nearest(point); return sample.distance<=sample.halfWidth+2 ? sample.surface : 0; }
	double carveHeight(Vec2 point,double original) const
	{
		const auto sample=nearest(point); if (sample.reach<0 || sample.distance>sample.halfWidth+65) { return original; }
		const double bank=Max(0.0,sample.distance-sample.halfWidth);
		const double target=sample.surface-1.4+bank*.18;
		const double blend=Clamp((sample.distance-sample.halfWidth-24)/41,0.0,1.0);
		return Math::Lerp(Min(original,target),original,blend*blend*(3-2*blend));
	}
private:
	static int64 key(int x,int z) { return static_cast<int64>(x)*0x100000000LL+static_cast<uint32>(z); }
	HashTable<int64,Array<int>> m_index;
};
