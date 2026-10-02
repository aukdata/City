#include "RailwaySite.hpp"
#include "../world/World.hpp"
#include "../road/RoadEnvironment.hpp"

namespace RailwaySite
{
	double stationMinimumRadius(const TrainNetwork& network,int station)
	{
		const auto* node=network.getNode(station); if (!node) { return 0; }
		double minimum=Math::Inf; bool any=false;
		for (const int first : node->edgeIds)
		{
			const auto* edge=network.getEdge(first); if (!edge || edge->depotTrack) { continue; }
			any=true; int current=station; double remaining=kPlatformLength; Optional<Vec3> previousTangent; HashSet<int> visited;
			while (edge && remaining>.01 && !visited.contains(edge->id))
			{
				visited.insert(edge->id); const auto curve=network.getBezier(edge->id); if (!curve) { return 0; }
				const bool forward=edge->nodeA==current; const double length=Min(remaining,static_cast<double>(edge->length));
				const double from=forward ? 0 : curve->tFromArcLength(static_cast<float>(edge->length-length));
				const double to=forward ? curve->tFromArcLength(static_cast<float>(length)) : 1;
				minimum=Min(minimum,curve->minimumHorizontalRadius(from,to));
				Vec3 start=curve->tangent(forward ? 0.0f : 1.0f)*(forward ? 1 : -1);start.y=0;start.normalize();
				if (previousTangent && previousTangent->dot(start)<Cos(.5_deg)) { return 0; }
				Vec3 end=curve->tangent(forward ? 1.0f : 0.0f)*(forward ? 1 : -1);end.y=0;previousTangent=end.normalized();
				remaining-=length;current=forward ? edge->nodeB : edge->nodeA;
				const auto* next=network.getNode(current);const int previous=edge->id;edge=nullptr;
				if (remaining<=.01) { break; }
				if (!next || next->type==TrackNodeType::Station || next->edgeIds.size()!=2) { return 0; }
				for (const int id : next->edgeIds) { if (id!=previous && !network.getEdge(id)->depotTrack) { edge=network.getEdge(id);break; } }
			}
			if (remaining>.01) { return 0; }
		}
		return any ? minimum : 0;
	}
	Array<Array<Vec3>> stationPaths(const TrainNetwork& network, int station)
	{
		Array<Array<Vec3>> paths;
		const auto* node = network.getNode(station); if (!node) { return paths; }
		// Storage slots can reorder incident edges during synchronize. Keep station-facing geometry stable.
		Array<int> incidentEdges = node->edgeIds;
		incidentEdges.sort();
		for (const int first : incidentEdges)
		{
			const auto* edge = network.getEdge(first); if (!edge || edge->depotTrack) { continue; }
			const int firstLane = TransportCrossSection::railLane(*edge,edge->nodeA == station);
			const float lateral = firstLane >= 0 ? Abs(edge->lanes[firstLane].centerAt(0)) : 0;
			Array<Vec3> path{node->position}; int current = station; double left = kPlatformLength;
			HashSet<int> visited;
			while (edge && left > .1 && !visited.contains(edge->id))
			{
				visited.insert(edge->id); const auto curve = network.getBezier(edge->id); if (!curve) { break; }
				const double length = Min(left, static_cast<double>(edge->length)); const bool forward = edge->nodeA == current;
				const int steps = Max(1, static_cast<int>(std::ceil(length / 4)));
				for (int i = 1; i <= steps; ++i)
				{
					const float arc = static_cast<float>(length * i / steps);
					path << curve->positionAt(forward ? arc : edge->length-arc);
				}
				left -= length; current = forward ? edge->nodeB : edge->nodeA;
				const auto* next = network.getNode(current); const int previous = edge->id; edge = nullptr;
				if (!next || next->type == TrackNodeType::Station || next->edgeIds.size() != 2) { break; }
				for (const int id : next->edgeIds)
				{
					if (id != previous && !network.getEdge(id)->depotTrack) { edge = network.getEdge(id); break; }
				}
			}
			if (path.size() >= 2)
			{
				const auto original = path;
				for (size_t i=0;i<path.size();++i)
				{
					const Vec3 along = original[Min(i+1,original.size()-1)]-original[i>0 ? i-1 : 0];
					path[i] -= tangentToRight(along)*lateral;
				}
				paths << std::move(path);
				// 終端駅でも上下線の外側にホームを配置する。
				if (lateral > .1)
				{
					Array<Vec3> opposite = original;
					for (size_t i=0;i<opposite.size();++i)
					{
						const Vec3 along = original[Min(i+1,original.size()-1)]-original[i>0 ? i-1 : 0];
						opposite[i] += tangentToRight(along)*lateral;
					}
					opposite.reverse(); paths << std::move(opposite);
				}
			}
		}
		return paths;
	}
	Optional<Frame> stationFrame(const TrainNetwork& network, int station)
	{
		const auto paths = stationPaths(network, station); if (paths.isEmpty()) { return none; }
		Vec3 along = paths.front()[1]-paths.front()[0]; along.y = 0;
		if (along.lengthSq() < .001) { return none; }
		along.normalize(); return Frame{network.getNode(station)->position, along, {along.z,0,-along.x}};
	}
	Optional<Frame> depotFrame(const TrainNetwork& network, const RailDepot& depot)
	{
		const auto* throat = network.getNode(depot.throatNodeId);
		const auto* end = depot.sidingNodes.isEmpty() ? nullptr : network.getNode(depot.sidingNodes.front());
		if (!throat || !end) { return none; }
		Vec3 along = end->position-throat->position; along.y = 0;
		if (along.lengthSq() < .001) { return none; }
		along.normalize(); return Frame{throat->position, along, {along.z,0,-along.x}};
	}
	ParcelGeometry::Quad rectangle(const Frame& frame, double left, double right, double start, double end)
	{
		const auto flat = [](Vec3 point) { return Vec2{point.x,point.z}; };
		return {flat(frame.point(left,0,start)),flat(frame.point(right,0,start)),flat(frame.point(right,0,end)),flat(frame.point(left,0,end))};
	}
	Array<ParcelGeometry::Quad> footprints(const TrainNetwork& network)
	{
		Array<ParcelGeometry::Quad> result;
		for (const auto& node : network.nodes())
		{
			if (node.type != TrackNodeType::Station) { continue; }
			if (node.stationKind==StationKind::Underground && node.entrance)
			{
				if (const auto frame=stationFrame(network,node.id))
				{
					result << rectangle({*node.entrance,frame->along,frame->right},-5,5,-5,5);
				}
				continue;
			}
			for (const auto& path : stationPaths(network, node.id))
			{
				for (size_t i = 1; i < path.size(); ++i)
				{
					Vec3 along = path[i]-path[i-1]; along.y = 0; const double length = along.length();
					if (length < .001) { continue; } along /= length;
					result << rectangle({path[i-1],along,{along.z,0,-along.x}}, -7, -1.7, 0, length);
				}
			}
			if (const auto frame = stationFrame(network, node.id))
			{
				result << rectangle(*frame,-17,-5,2,64);
				if (node.stationKind==StationKind::Terminal)
				{
					const auto& settings=GenerationSettings::get();
					result << rectangle(*frame,-settings.landmarks_terminalWidth*.5-1,settings.landmarks_terminalWidth*.5+1,2,settings.landmarks_terminalLength+4);
				}
			}
		}
		for (const auto& depot : network.depots())
		{
			if (const auto frame = depotFrame(network, depot)) { result << rectangle(*frame, -5, 13, -4, 140); }
		}
		return result;
	}
	Array<ParcelGeometry::Quad> landscapeFootprints(const TrainNetwork& network,const World& world)
	{
		auto result=footprints(network);
		for (const auto& edge : network.edges())
		{
			const auto curve=network.getBezier(edge.id); if (!curve) { continue; }
			const int count=Max(1,static_cast<int>(Ceil(curve->totalLength/12)));
			for (int i=0;i<count;++i)
			{
				const Vec3 a=curve->positionAt(curve->totalLength*i/count),b=curve->positionAt(curve->totalLength*(i+1)/count),middle=(a+b)*.5;
				if (RoadEnvironment::coveredAt(world,middle,true,RoadEnvironment::kRailTunnelCrown)) { continue; }
				Vec3 along=b-a;along.y=0;const double length=along.length();if (length<.01) { continue; } along/=length;
				result<<rectangle({a,along,{along.z,0,-along.x}},-edge.totalWidth()*.5-.5,edge.totalWidth()*.5+.5,-.3,length+.3);
			}
		}
		return result;
	}

}
