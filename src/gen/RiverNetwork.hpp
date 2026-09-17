#pragma once
#include "GenerationSettings.hpp"
#include <Siv3D.hpp>
#include <queue>
#include "../debug/DebugLog.hpp"

/// @brief 流域の集水・合流・下流への河床低下を共有する河川網。単位はm。
class RiverNetwork
{
public:
	struct Reach { Vec3 start,end; double halfWidth=20; double endHalfWidth=20; double barSide=0; double catchment=0; RectF bounds; };
	struct Sample { Vec2 center; double distance=1e30; double surface=0; double halfWidth=0; int reach=-1; };
	Array<Reach> reaches;
	template<class Height>
	void generate(double width,double depth,const Height& height)
	{
		reaches.clear(); m_index.clear();
		const double spacing=GenerationSettings::get().rivers_catchmentGrid;
		const int columns=static_cast<int>(width/spacing)+1,rows=static_cast<int>(depth/spacing)+1,count=columns*rows;
		Array<double> ground(count),filled(count),water(count); Array<int> parent(count,-1),order,flow(count,1),mainChild(count,-1);
		Array<bool> visited(count,false);
		using Entry=std::pair<double,int>;
		std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
		const auto position=[&](int id) { return Vec2{(id%columns)*spacing,(id/columns)*spacing}; };
		for (int id=0;id<count;++id)
		{
			const Vec2 point=position(id); ground[id]=height(point.x,point.y); water[id]=Max(0.0,ground[id]-GenerationSettings::get().rivers_initialWaterDepth);
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
		const int threshold=GenerationSettings::get().rivers_minimumCatchmentCells;
		Array<Vec2> centers(count);
		for (int id=0;id<count;++id)
		{
			centers[id]=position(id);
			if (flow[id]>=threshold && parent[id]>=0 && mainChild[id]>=0 && ground[id]>GenerationSettings::get().rivers_smoothingMinimumAltitude)
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
			water[id]=Min(water[id],Max(0.0,low-GenerationSettings::get().rivers_valleyWaterDepth)); water[next]=Min(water[next],Max(0.0,low-GenerationSettings::get().rivers_valleyWaterDepth));
		}
		for (auto it=order.rbegin();it!=order.rend();++it)
		{
			const int id=*it,next=parent[id]; if (next<0 || flow[id]<threshold) { continue; }
			water[next]=Min(water[next],Max(0.0,water[id]-GenerationSettings::get().rivers_minimumWaterGrade*centers[id].distanceFrom(centers[next])));
		}
		double length=0,maxCut=0; int confluences=0;
		for (int id=0;id<count;++id)
		{
			const int next=parent[id]; if (next<0 || flow[id]<threshold || ground[id]<GenerationSettings::get().rivers_mouthAltitude) { continue; }
			const Vec2 first=centers[id],last=centers[next];
			const double run=first.distanceFrom(last);
			if (run<1) { continue; }
			const Vec2 incoming=mainChild[id]>=0 ? (last-centers[mainChild[id]])*.5 : last-first;
			const Vec2 outgoing=parent[next]>=0 ? (centers[parent[next]]-first)*.5 : last-first;
			const auto tangent=[&](Vec2 direction) { return direction.lengthSq()>.01 ? direction.normalized()*Min(run,direction.length()) : last-first; };
			const Vec2 controlA=first+tangent(incoming)/3,controlB=last-tangent(outgoing)/3;
			const auto widthAtFlow=[](int amount) { return Clamp(GenerationSettings::get().rivers_widthBase+std::sqrt(static_cast<double>(amount))*GenerationSettings::get().rivers_widthFlowScale,GenerationSettings::get().rivers_minimumHalfWidth,GenerationSettings::get().rivers_maximumHalfWidth); };
			const double widthA=widthAtFlow(flow[id]),widthB=widthAtFlow(flow[next]);
			const int sections=Max(2,static_cast<int>(std::ceil(run/GenerationSettings::get().rivers_segmentLength)));
			const auto pointAt=[&](double fraction)
			{
				const double reverse=1-fraction;
				const Vec2 p=first*(reverse*reverse*reverse)+controlA*(3*reverse*reverse*fraction)+controlB*(3*reverse*fraction*fraction)+last*(fraction*fraction*fraction);
				return Vec3{p.x,Math::Lerp(water[id],water[next],fraction),p.y};
			};
			for (int section=0;section<sections;++section)
			{
				Reach reach; reach.start=pointAt(static_cast<double>(section)/sections); reach.end=pointAt(static_cast<double>(section+1)/sections);
				reach.halfWidth=Math::Lerp(widthA,widthB,static_cast<double>(section)/sections);
				reach.endHalfWidth=Math::Lerp(widthA,widthB,static_cast<double>(section+1)/sections);
				reach.catchment=flow[id]*spacing*spacing;
				const Vec2 lower{Min(reach.start.x,reach.end.x),Min(reach.start.z,reach.end.z)},upper{Max(reach.start.x,reach.end.x),Max(reach.start.z,reach.end.z)};
				reach.bounds=RectF{lower,upper-lower}.stretched(Max(reach.halfWidth,reach.endHalfWidth)+80);
				const int index=static_cast<int>(reaches.size()); reaches<<reach;
				for (int z=static_cast<int>(Floor(reach.bounds.y/512));z<=static_cast<int>(Floor(reach.bounds.br().y/512));++z)
					for (int x=static_cast<int>(Floor(reach.bounds.x/512));x<=static_cast<int>(Floor(reach.bounds.br().x/512));++x) { m_index[key(x,z)]<<index; }
			}
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
			const double width=Math::Lerp(reach.halfWidth,reach.endHalfWidth,t);
			if (distance-width<result.distance-result.halfWidth) { result={center,distance,Math::Lerp(reach.start.y,reach.end.y,t),width,id}; }
		}
		return result;
	}
	/// @brief 樹冠・根元とも水面と河岸の余裕幅に侵入させない。
	bool vegetationAllowed(Vec2 point,double ground,double radius=10) const
	{
		const auto sample=nearest(point);
		return ground>GenerationSettings::get().rivers_vegetationMinimumAltitude && (sample.reach<0 || sample.distance>sample.halfWidth+radius+GenerationSettings::get().rivers_vegetationBankMargin);
	}
	/// @brief 非同期景観生成へ渡す読み取り専用の局所水系。
	RiverNetwork subset(const RectF& bounds) const
	{
		RiverNetwork result;
		for (const auto& reach : reaches)
		{
			if (!reach.bounds.intersects(bounds)) { continue; }
			const int index=static_cast<int>(result.reaches.size()); result.reaches<<reach;
			for (int z=static_cast<int>(Floor(reach.bounds.y/512));z<=static_cast<int>(Floor(reach.bounds.br().y/512));++z)
				for (int x=static_cast<int>(Floor(reach.bounds.x/512));x<=static_cast<int>(Floor(reach.bounds.br().x/512));++x) { result.m_index[key(x,z)]<<index; }
		}
		return result;
	}
	double waterLevel(Vec2 point) const { const auto sample=nearest(point); return sample.distance<=sample.halfWidth+GenerationSettings::get().rivers_waterEdgeMargin ? sample.surface : 0; }
	double carveHeight(Vec2 point,double original) const
	{
		const auto sample=nearest(point); if (sample.reach<0 || sample.distance>sample.halfWidth+GenerationSettings::get().rivers_carveExtent) { return original; }
		const double bank=Max(0.0,sample.distance-sample.halfWidth);
		const double target=sample.surface-GenerationSettings::get().rivers_bedDepth+bank*GenerationSettings::get().rivers_bankSlope;
		const double blend=Clamp((sample.distance-sample.halfWidth-GenerationSettings::get().rivers_bankBlendStart)/GenerationSettings::get().rivers_bankBlendWidth,0.0,1.0);
		return Math::Lerp(Min(original,target),original,blend*blend*(3-2*blend));
	}
private:
	static int64 key(int x,int z) { return static_cast<int64>(x)*0x100000000LL+static_cast<uint32>(z); }
	HashTable<int64,Array<int>> m_index;
};
