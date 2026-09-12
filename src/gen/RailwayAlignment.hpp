#pragma once
#include "MapGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "TransportClearance.hpp"
#include "RailCostProfile.hpp"
#include "../debug/DebugLog.hpp"

/// @brief 近接駅の接続ごとに複数の地形回廊と縦断施工費を比較して線形を選ぶ。
namespace RailwayAlignment
{
	inline void generate(TrainNetwork& network,World& world,const Array<MapGenerator::Settlement>& towns,const RoadNetwork* roads=nullptr)
	{
		const TransportClearance crossings{roads,world};
		network=TrainNetwork{}; Array<int> stations; HashTable<int,Vec3> axes;
		for (const auto& town : towns)
		{
			if (!town.plan.station) { continue; }
			const Vec2 point=town.center+town.gridAxisX*town.plan.station->x+town.gridAxisZ*town.plan.station->y;
			double required=Max(world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+9.0,crossings.minimumRailHeight(point,80)+2.0);
			for (int i=0;i<16;++i) for (const double distance : {160.0,320.0,640.0})
			{
				const Vec2 around=point+Vec2{Cos(i*Math::TwoPi/16),Sin(i*Math::TwoPi/16)}*distance;
				required=Max(required,crossings.minimumRailHeight(around,64)+2-distance*.012);
			}
			required=Min(required,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+22.0);
			const double elevation=std::ceil(required/.5)*.5;
			const int id=network.addStation({point.x,elevation,point.y},town.name); stations << id; axes[id]={town.gridAxisX.x,0,town.gridAxisX.y};
		}
		if (stations.size()<2) { return; }
		HashSet<int> connected{stations.front()}; HashSet<uint64> rejected; int lines=0,failures=0;
		const auto pairKey=[](int a,int b) { return (static_cast<uint64>(Min(a,b))<<32)|static_cast<uint32>(Max(a,b)); };
		while (connected.size()<stations.size())
		{
			int from=-1,to=-1; double nearest=1e30;
			for (const int a : stations) if (connected.contains(a)) for (const int b : stations) if (!connected.contains(b))
			{
				if (rejected.contains(pairKey(a,b))) { continue; }
				const double distance=network.getNode(a)->position.distanceFromSq(network.getNode(b)->position);
				if (distance<nearest) { nearest=distance; from=a; to=b; }
			}
			if (from<0) { throw Error{U"鉄道の接続可能な経路が見つかりません"}; }
			const Vec3 start=network.getNode(from)->position,end=network.getNode(to)->position;
			const Vec2 delta{end.x-start.x,end.z-start.z};
			Array<Vec3> bestPoints; Array<double> bestHeights; double bestCost=Math::Inf; int chosen=-1;
			for (int alternative=0;alternative<4;++alternative)
			{
				const double padding=alternative<3 ? 3000 : 6500;
				const Vec2 offset{Max(0.0,Min(start.x,end.x)-padding),Max(0.0,Min(start.z,end.z)-padding)};
				const Vec2 upper{Min(static_cast<double>(WORLD_SIZE),Max(start.x,end.x)+padding),Min(static_cast<double>(WORLD_SIZE),Max(start.z,end.z)+padding)};
				RoadPathfinder finder; finder.setRailwayRouting(true);
				finder.setConstructionCost([&](Vec2 point,double ground)
				{
					const double t=Clamp((point-Vec2{start.x,start.z}).dot(delta)/Max(1.0,delta.lengthSq()),0.0,1.0);
					const double reference=Math::Lerp(start.y,end.y,t);
					const double coefficient=alternative==0 ? .35 : alternative==1 ? .7 : alternative==2 ? .12 : 1.4;
					const double water=world.waterSurfaceHeight(point.x,point.y);
					return 1+RailCostProfile::unitCost(reference-ground)*coefficient+(ground<water+1 ? 3.0 : 0.0);
				});
				finder.setup(world,offset,Max(2,static_cast<int>((upper.x-offset.x)/100)),Max(2,static_cast<int>((upper.y-offset.y)/100)),100);
				const auto path=finder.findPath(finder.worldToGrid(static_cast<float>(start.x),static_cast<float>(start.z)),finder.worldToGrid(static_cast<float>(end.x),static_cast<float>(end.z)));
				auto coarse=finder.samplePath(path,2); if (coarse.size()<2) { continue; }
				coarse.front()=start; coarse.back()=end;
				for (int pass=0;pass<5;++pass) { auto next=coarse; for (size_t i=1;i+1<coarse.size();++i) { next[i]=coarse[i]*.5+(coarse[i-1]+coarse[i+1])*.25; } coarse=std::move(next); }
				Array<Vec3> points;
				for (size_t i=0;i+1<coarse.size();++i)
				{
					const Vec3 direction=coarse[i+1]-coarse[i]; const double handle=Vec2{direction.x,direction.z}.length()/3;
					Vec3 a=i>0 ? coarse[i+1]-coarse[i-1] : axes[from]*(axes[from].dot(direction)>=0 ? 1.0 : -1.0);
					Vec3 b=i+2<coarse.size() ? coarse[i+2]-coarse[i] : axes[to]*(axes[to].dot(direction)>=0 ? 1.0 : -1.0); a.y=b.y=0;
					const CubicBezier curve{coarse[i],coarse[i]+a.normalized()*handle,coarse[i+1]-b.normalized()*handle,coarse[i+1]};
					const int count=Max(1,static_cast<int>(std::ceil(curve.totalLength/70)));
					for (int sample=0;sample<count;++sample) { points << curve.positionAt(curve.totalLength*sample/count); }
				}
				points << end;
				Array<RailCostProfile::Sample> samples;
				for (size_t i=0;i<points.size();++i)
				{
					const auto& point=points[i]; const Vec2 position{point.x,point.z};
					const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
					double minimum=Max(-50.0,ground-300),maximum=ground+30,roadClearance=-1e9,underpass=1e9;
					for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx)
					{
						const Vec2 around=position+Vec2{dx*8.0,dz*8.0}; const double terrain=world.sampleHeight(static_cast<float>(around.x),static_cast<float>(around.y)),water=world.waterSurfaceHeight(around.x,around.y);
						const auto corridor=crossings.interval(around,40);
						roadClearance=Max(roadClearance,corridor.second+.6); underpass=Min(underpass,corridor.first-.6);
						if (terrain<water+1) { roadClearance=Max(roadClearance,water+6); underpass=Min(underpass,terrain-10); }
					}
					if (i==0) { minimum=maximum=start.y; } if (i+1==points.size()) { minimum=maximum=end.y; }
					samples << RailCostProfile::Sample{position,ground,minimum,maximum,roadClearance,underpass};
				}
				const auto profile=RailCostProfile::solve(samples,start.y,end.y);
				DBG_LOG(U"[RailCostCandidate] line={} variant={} feasible={} cost={:.0f} points={}"_fmt(lines,alternative,profile.feasible,profile.cost,points.size()));
				if (profile.feasible && profile.cost<bestCost) { bestCost=profile.cost; bestPoints=std::move(points); bestHeights=profile.heights; chosen=alternative; }
			}
			if (bestPoints.isEmpty()) { ++failures; rejected.insert(pairKey(from,to)); DBG_LOG(U"[RailCostFailure] from={} to={}"_fmt(from,to)); continue; }
			connected.insert(to);
			Array<int> ids{from};
			for (size_t i=0;i<bestPoints.size();++i) { bestPoints[i].y=bestHeights[i]; if (i>0 && i+1<bestPoints.size()) { ids << network.addNode(bestPoints[i]); } } ids << to;
			for (size_t i=0;i+1<bestPoints.size();++i)
			{
				const Vec3 a=bestPoints[i],b=bestPoints[i+1],span=b-a;
				Vec3 ta=i>0 ? b-bestPoints[i-1] : axes[from]*(axes[from].dot(span)>=0 ? 1.0 : -1.0);
				Vec3 tb=i+2<bestPoints.size() ? bestPoints[i+2]-a : axes[to]*(axes[to].dot(span)>=0 ? 1.0 : -1.0);
				ta.y=tb.y=0; const double handle=Vec2{span.x,span.z}.length()/3;
				Vec3 controlA=a+ta.normalized()*handle,controlB=b-tb.normalized()*handle;
				controlA.y=Math::Lerp(a.y,b.y,1.0/3); controlB.y=Math::Lerp(a.y,b.y,2.0/3);
				network.addEdge(ids[i],ids[i+1],controlA,controlB,80);
			}
			DBG_LOG(U"[RailCostChosen] line={} variant={} cost={:.0f} sections={}"_fmt(lines,chosen,bestCost,bestPoints.size()-1));
			TrainSchedule schedule; schedule.id=lines++; schedule.headwaySec=1800; schedule.stops={StopEntry{from,30,0},StopEntry{to,30,0}}; schedule.loop=false; network.addSchedule(schedule);
		}
		// Shallow cuts are physical earthworks, not track meshes hidden inside the original terrain.
		HashTable<int64,float> cutting;
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id);
			for (float arc=0;arc<=curve->totalLength;arc+=8)
			{
				const Vec3 point=curve->positionAt(arc); const int gx=static_cast<int>(point.x/16),gz=static_cast<int>(point.z/16);
				for (int z=Max(0,gz-1);z<=Min(WORLD_CHUNKS*HEIGHT_CELLS,gz+2);++z) for (int x=Max(0,gx-1);x<=Min(WORLD_CHUNKS*HEIGHT_CELLS,gx+2);++x)
				{
					if (Vec2{x*16.0,z*16.0}.distanceFrom({point.x,point.z})>22) { continue; }
					const float ground=world.sampleHeight(static_cast<float>(x*16),static_cast<float>(z*16));
					const float target=static_cast<float>(point.y-.35); if (ground<=target || ground-target>5) { continue; }
					const int64 key=static_cast<int64>(z)*65536+x; if (!cutting.contains(key) || cutting[key]>target) { cutting[key]=target; }
				}
			}
		}
		for (const auto& [key,height] : cutting) { world.setGridHeight(static_cast<int>(key%65536),static_cast<int>(key/65536),height); }
		for (int z=0;z<WORLD_CHUNKS;++z) for (int x=0;x<WORLD_CHUNKS;++x) { if (auto* chunk=world.getChunk({x,z});chunk && chunk->meshDirty) { chunk->updateHeightBounds(); } }
		Array<double> clearances; double maximumGrade=0; int conflicts=0,underWater=0;
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id); const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/6)));
			for (int i=0;i<=count;++i)
			{
				const Vec3 point=curve->evaluate(static_cast<float>(i)/count),tangent=curve->tangent(static_cast<float>(i)/count);
				clearances << point.y-world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
				maximumGrade=Max(maximumGrade,Abs(tangent.y)/Max(.001,Vec2{tangent.x,tangent.z}.length()));
				const auto crossing=crossings.interval({point.x,point.z}); conflicts+=point.y<crossing.second-.1 && point.y>crossing.first+.1; underWater+=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))<world.waterSurfaceHeight(point.x,point.z)+1 && point.y<world.waterSurfaceHeight(point.x,point.z) && point.y>world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))-8;
			}
		}
		clearances.sort();
		if (!clearances.isEmpty()) { DBG_LOG(U"[RailHeight] samples={} median={} p95={} maximum={}"_fmt(clearances.size(),clearances[clearances.size()/2],clearances[clearances.size()*95/100],clearances.back())); }
		DBG_LOG(U"[RailwayAudit] maxGrade={} roadConflicts={} underWater={} failedConnections={}"_fmt(maximumGrade,conflicts,underWater,stations.size()-connected.size()));
		DBG_LOG(U"[RailwayAlignment] stations={} lines={} sections={} rejectedCorridors={}"_fmt(stations.size(),lines,network.edges().size(),failures));
	}
}
