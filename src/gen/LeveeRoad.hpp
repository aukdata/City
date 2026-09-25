#pragma once
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "RoadNodeIndex.hpp"
#include "../debug/DebugLog.hpp"

/// @brief Generate occasional connected maintenance roads on broad lowland river levees.
namespace LeveeRoad
{
	/// @brief Quantized water node key for following reaches through tributary joins.
	inline uint64 nodeKey(Vec3 point)
	{
		const int x=static_cast<int>(Round(point.x*10)),z=static_cast<int>(Round(point.z*10));
		return (static_cast<uint64>(static_cast<uint32>(x))<<32)|static_cast<uint32>(z);
	}

	/// @brief Dry, reasonably graded link from a riverbank road to an existing junction.
	inline bool dryConnection(const World& world,Vec3 a,Vec3 b)
	{
		const double distance=Vec2{a.x-b.x,a.z-b.z}.length();
		if (distance<12 || distance>210 || Abs(a.y-b.y)/distance>.08) { return false; }
		for (double along=0;along<=distance;along+=20)
		{
			const Vec3 point=a.lerp(b,along/distance);
			const double ground=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
			if (ground<world.waterSurfaceHeight(point.x,point.z)+.2) { return false; }
		}
		return true;
	}

	/// @brief Add curves along the river rather than extending a short straight road.
	inline int generate(uint64 seed,const World& world,RoadNetwork& network)
	{
		const auto& reaches=world.rivers().reaches;
		HashTable<uint64,int> downstream;
		downstream.reserve(reaches.size());
		for (size_t index=0;index<reaches.size();++index)
		{
			downstream[nodeKey(reaches[index].start)]=static_cast<int>(index);
		}
		RoadNodeIndex access;
		for (const RoadNode& node:network.nodes())
		{
			if (node.id>=0 && !node.attachments.isEmpty()) { access.insert(node.position,node.id); }
		}
		Array<Vec2> used;
		int generated=0,connected=0,candidates=0,validPaths=0,startAccesses=0,bothAccesses=0;
		for (size_t candidate=0;candidate<reaches.size() && generated<4;candidate+=7)
		{
			const auto& first=reaches[candidate];
			const Vec2 center{first.start.x,first.start.z};
			if (first.halfWidth<46 || first.start.y>145 || first.start.y<1.5
				|| center.x<1400 || center.y<1400 || center.x>WORLD_SIZE-1400 || center.y>WORLD_SIZE-1400
				|| used.any([&](Vec2 site) { return site.distanceFrom(center)<2800; })) { continue; }
			const uint32 hash=static_cast<uint32>(candidate)*747796405u^static_cast<uint32>(seed);
			if (hash%4!=0) { continue; }
			++candidates;
			for (const int side:{-1,1})
			{
				Array<Vec3> points;
				int reachId=static_cast<int>(candidate);
				double length=0,lastPoint=0;
				for (int steps=0;steps<180 && length<620;++steps)
				{
					if (reachId<0 || reachId>=static_cast<int>(reaches.size())) { break; }
					const auto& reach=reaches[reachId];
					const Vec3 delta=reach.end-reach.start;
					const Vec2 flow{delta.x,delta.z};
					const double run=flow.length();
					if (run<.5 || Max(reach.start.y,reach.end.y)>185) { break; }
					const Vec2 normal{flow.y/run,-flow.x/run};
					const Vec3 end=reach.end+Vec3{normal.x*side*(reach.endHalfWidth+43),0,
						normal.y*side*(reach.endHalfWidth+43)};
					if (points.isEmpty())
					{
						const Vec3 begin=reach.start+Vec3{normal.x*side*(reach.halfWidth+43),0,
							normal.y*side*(reach.halfWidth+43)};
						const double ground=world.sampleHeight(static_cast<float>(begin.x),static_cast<float>(begin.z));
						points << Vec3{begin.x,ground+1.45,begin.z};
					}
					length+=run;
					if (length-lastPoint>=88 || length>=560)
					{
						const double ground=world.sampleHeight(static_cast<float>(end.x),static_cast<float>(end.z));
						const double water=world.waterSurfaceHeight(end.x,end.z);
						if (end.x<300 || end.z<300 || end.x>WORLD_SIZE-300 || end.z>WORLD_SIZE-300
							|| ground<water-.2 || ground>reach.end.y+20) { break; }
						const Vec3 roadPoint{end.x,ground+1.45,end.z};
						if (roadPoint.xz().distanceFrom(points.back().xz())<35
							|| roadPoint.xz().distanceFrom(points.back().xz())>175) { break; }
						points << roadPoint;
						lastPoint=length;
					}
					const auto next=downstream.find(nodeKey(reach.end));
					if (next==downstream.end() || next->second==reachId) { break; }
					reachId=next->second;
				}
				if (length<400 || points.size()<4) { continue; }
				++validPaths;
				const auto findAccess=[&](Vec3 roadPoint,int exclude)
				{
					return access.findBest(roadPoint,network,210,[&](const RoadNode& node,float distanceSq)->Optional<double>
					{
						if (node.id==exclude || node.attachments.isEmpty()
							|| !dryConnection(world,node.position,roadPoint)) { return none; }
						return distanceSq;
					});
				};
				const auto start=findAccess(points.front(),-1);
				if (!start) { continue; }
				++startAccesses;
				const auto finish=findAccess(points.back(),*start);
				if (!finish) { continue; }
				++bothAccesses;
				bool gentle=true;
				for (size_t index=1;index<points.size();++index)
				{
					const double run=points[index].xz().distanceFrom(points[index-1].xz());
					if (run<30 || Abs(points[index].y-points[index-1].y)/run>.075) { gentle=false;break; }
				}
				if (!gentle) { continue; }
				Array<int> nodes;
				nodes << *start;
				for (const Vec3 point:points) { nodes << network.addNode(point); }
				nodes << *finish;
				bool success=true;
				Array<int> addedEdges;
				for (size_t index=1;index<nodes.size();++index)
				{
					const Vec3 a=network.getNode(nodes[index-1])->position;
					const Vec3 b=network.getNode(nodes[index])->position;
					const auto id=network.addEdge(nodes[index-1],nodes[index],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
					if (!id) { success=false;break; }
					addedEdges << *id;
					RoadEdge* edge=network.getEdge(*id);
					edge->designGrade=true;
					edge->leveeRoad=true;
					edge->edgeState=EdgeState::Open;
				}
				if (!success)
				{
					for (const int id:addedEdges) { network.removeEdge(id); }
					for (size_t index=1;index+1<nodes.size();++index) { network.removeNode(nodes[index]); }
					continue;
				}
				DBG_LOG(U"[LeveeRoadRoute] start=({}, {}) end=({}, {}) river=({}, {}) length={:.0f}"_fmt(
					points.front().x,points.front().z,points.back().x,points.back().z,center.x,center.y,length));
				used << center;
				++generated;
				connected+=static_cast<int>(nodes.size())-1;
				break;
			}
		}
		DBG_LOG(U"[LeveeRoad] routes={} connectedSegments={} candidates={} paths={} starts={} both={}"_fmt(
			generated,connected,candidates,validPaths,startAccesses,bothAccesses));
		return generated;
	}
}
