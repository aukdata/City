#pragma once
#include "MapGenerator.hpp"
#include "RoadPathfinder.hpp"
#include "TransportClearance.hpp"
#include "../debug/DebugLog.hpp"
#include <queue>

/// @brief Nearby stations form a minimum spanning network, fitted to terrain before plots are built.
namespace RailwayAlignment
{
	inline void generate(TrainNetwork& network,const World& world,const Array<MapGenerator::Settlement>& towns,const RoadNetwork* roads=nullptr)
	{
		network=TrainNetwork{};
		Array<int> stations;
		HashTable<int,Vec3> stationAxes;
		for (const auto& town : towns)
		{
			if (!town.plan.station) { continue; }
			const Vec2 point=town.center+town.gridAxisX*town.plan.station->x+town.gridAxisZ*town.plan.station->y;
			const int id=network.addStation({point.x,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y))+8,point.y},town.name);
			stations << id; stationAxes[id]={town.gridAxisX.x,0,town.gridAxisX.y};
		}
		if (stations.size()<2) { return; }
		HashSet<int> connected{stations.front()};
		int lineCount=0;
		while (connected.size()<stations.size())
		{
			int from=-1,to=-1; double nearest=1e30;
			for (const int a : stations)
			{
				if (!connected.contains(a)) { continue; }
				for (const int b : stations)
				{
					if (connected.contains(b)) { continue; }
					const double distance=network.getNode(a)->position.distanceFromSq(network.getNode(b)->position);
					if (distance<nearest) { nearest=distance; from=a; to=b; }
				}
			}
			connected.insert(to);
			const Vec3 a=network.getNode(from)->position,b=network.getNode(to)->position;
			const Vec2 offset{Max(0.0,Min(a.x,b.x)-1800),Max(0.0,Min(a.z,b.z)-1800)};
			const Vec2 upper{Min(static_cast<double>(WORLD_SIZE),Max(a.x,b.x)+1800),Min(static_cast<double>(WORLD_SIZE),Max(a.z,b.z)+1800)};
			RoadPathfinder finder; finder.setRailwayRouting(true);
			finder.setup(world,offset,Max(2,static_cast<int>((upper.x-offset.x)/120)),Max(2,static_cast<int>((upper.y-offset.y)/120)),120);
			const auto path=finder.findPath(finder.worldToGrid(static_cast<float>(a.x),static_cast<float>(a.z)),finder.worldToGrid(static_cast<float>(b.x),static_cast<float>(b.z)));
			Array<Vec3> points=finder.samplePath(path,2);
			if (points.size()<2) { points={a,b}; }
			points.front()=a; points.back()=b;
			// Smooth grid headings over several cells while retaining station locations.
			for (int pass=0;pass<4;++pass)
			{
				auto smoothed=points;
				for (size_t i=1;i+1<points.size();++i) { smoothed[i]=points[i]*.5+(points[i-1]+points[i+1])*.25; }
				points=std::move(smoothed);
			}
			Array<int> ids{from};
			for (size_t i=1;i+1<points.size();++i) { ids << network.addNode(points[i]); }
			ids << to;
			for (size_t i=0;i+1<points.size();++i)
			{
				const Vec3 delta=points[i+1]-points[i];
				Vec3 tangentA=i>0 ? points[i+1]-points[i-1] : stationAxes[from]*(stationAxes[from].dot(delta)>=0 ? 1.0 : -1.0);
				Vec3 tangentB=i+2<points.size() ? points[i+2]-points[i] : stationAxes[to]*(stationAxes[to].dot(delta)>=0 ? 1.0 : -1.0);
				tangentA.y=tangentB.y=0;
				if (tangentA.lengthSq()<.01) { tangentA=delta; }
				if (tangentB.lengthSq()<.01) { tangentB=delta; }
				const double handle=Vec2{delta.x,delta.z}.length()/3;
				network.addEdge(ids[i],ids[i+1],points[i]+tangentA.normalized()*handle,points[i+1]-tangentB.normalized()*handle,80);
			}
			TrainSchedule schedule; schedule.id=lineCount++; schedule.headwaySec=1800; schedule.loop=false;
			schedule.stops << StopEntry{from,30,0} << StopEntry{to,30,0}; network.addSchedule(schedule);
		}
		// Every control hull clears the sampled ground / urban viaduct / water envelope.
		// Grade propagation happens across shared station nodes, so no line ends at a different height.
		const TransportClearance crossings{roads,world};
		HashTable<int,double> levels;
		std::priority_queue<std::pair<double,int>> pending;
		for (const auto& node : network.nodes()) { levels[node.id]=Max(6.0,node.position.y); }
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id);
			double minimum=0;
			const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/8)));
			for (int i=0;i<=count;++i)
			{
				const Vec3 point=curve->evaluate(static_cast<float>(i)/count),right=tangentToRight(curve->tangent(static_cast<float>(i)/count));
				double ground=-1e9;
				for (const int side : {-1,0,1})
				{
					const Vec3 sample=point+right*(4.0*side);
					ground=Max(ground,static_cast<double>(world.sampleHeight(static_cast<float>(sample.x),static_cast<float>(sample.z))));
				}
				bool urban=false;
				for (const auto& town : towns)
				{
					const Vec2 local{point.x-town.center.x,point.z-town.center.y};
					if (!town.plan.frontageRoads && Abs(local.dot(town.gridAxisX))<town.plan.halfExtent.x+80 && Abs(local.dot(town.gridAxisZ))<town.plan.halfExtent.y+80) { urban=true; break; }
				}
				minimum=Max(minimum,Max(Max(6.0,ground+(urban ? 8.0 : .45)),crossings.minimumRailHeight({point.x,point.z},8)));
			}
			levels[edge.nodeA]=Max(levels[edge.nodeA],minimum); levels[edge.nodeB]=Max(levels[edge.nodeB],minimum);
		}
		for (const auto& [id,level] : levels) { pending.emplace(level,id); }
		while (!pending.empty())
		{
			const auto [level,id]=pending.top(); pending.pop();
			if (level<levels[id]-.001) { continue; }
			for (const int edgeId : network.getNode(id)->edgeIds)
			{
				const auto* edge=network.getEdge(edgeId); const int other=edge->nodeA==id ? edge->nodeB : edge->nodeA;
				const Vec3 delta=network.getNode(id)->position-network.getNode(other)->position;
				const double minimum=level-Vec2{delta.x,delta.z}.length()*.017;
				if (levels[other]+.001<minimum) { levels[other]=minimum; pending.emplace(minimum,other); }
			}
		}
		for (const auto& [id,level] : levels) { network.getNode(id)->position.y=level; }
		for (const auto& source : network.edges())
		{
			auto* edge=network.getEdge(source.id);
			edge->ctrlA.y=Math::Lerp(levels[edge->nodeA],levels[edge->nodeB],1.0/3);
			edge->ctrlB.y=Math::Lerp(levels[edge->nodeA],levels[edge->nodeB],2.0/3);
			edge->length=network.getBezier(edge->id)->totalLength;
		}
		double maxGrade=0,minGround=1e9; int roadConflicts=0,underWater=0;
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id);
			const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/6)));
			for (int i=0;i<=count;++i)
			{
				const Vec3 point=curve->evaluate(static_cast<float>(i)/count),tangent=curve->tangent(static_cast<float>(i)/count);
				maxGrade=Max(maxGrade,Abs(tangent.y)/Max(.0001,Vec2{tangent.x,tangent.z}.length()));
				minGround=Min(minGround,point.y-world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)));
				roadConflicts+=point.y<crossings.minimumRailHeight({point.x,point.z})-.05; underWater+=point.y<5.9;
			}
		}
		DBG_LOG(U"[RailwayAudit] maxGrade={} minimumGroundClearance={} roadConflicts={} underWater={}"_fmt(maxGrade,minGround,roadConflicts,underWater));
		DBG_LOG(U"[RailwayAlignment] stations={} lines={} sections={}"_fmt(stations.size(),lineCount,network.edges().size()));
	}
}
