#include "RailwaySite.hpp"
#include "../world/World.hpp"
#include "../road/RoadEnvironment.hpp"
#include "../gen/ParcelRoadIndex.hpp"
#include "../debug/DebugLog.hpp"

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
		for (const int first : node->edgeIds)
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
	ParcelGeometry::Quad forecourtFootprint(const Frame& station,const StationForecourt& forecourt)
	{
		constexpr double kEntranceInset = 4;
		return rectangle({forecourt.origin,station.along,station.right},kEntranceInset - forecourt.size.x,kEntranceInset,0,forecourt.size.y);
	}
	void prepareForecourts(TrainNetwork& network,const World& world,const RoadNetwork& roads)
	{
		constexpr double kMaximumRelief = 1.5;
		constexpr double kMaximumStreetDistance = 100;
		constexpr double kSurveyStep = 4;
		constexpr double kStepRise = .18;
		constexpr double kStepRun = .28;
		constexpr double kStairStart = 22.3;
		constexpr double kStationFloor = 1.05;
		const ParcelRoadIndex occupied{roads,false,false};
		Array<int> stations;
		for (const auto& node : network.nodes()) { if (node.type == TrackNodeType::Station) { stations << node.id; } }
		int created = 0;
		for (const int id : stations)
		{
			auto* node = network.getNode(id);
			if (node->stationKind == StationKind::Underground || node->forecourt) { continue; }
			const auto frame = stationFrame(network,id); if (!frame) { continue; }
			Vec3 entrance = frame->point(-12,0,40);
			constexpr int kLandingIterations = 4;
			for (int pass = 0; pass < kLandingIterations; ++pass)
			{
				const double ground = world.sampleHeight(static_cast<float>(entrance.x),static_cast<float>(entrance.z));
				const int steps = Max(1,static_cast<int>(Ceil(Max(kStepRise,frame->origin.y + kStationFloor - ground) / kStepRise)));
				entrance = frame->point(-12,0,kStairStart + steps * kStepRun);
			}
			entrance.y = world.sampleHeight(static_cast<float>(entrance.x),static_cast<float>(entrance.z));
			const Array<Vec2> sizes = node->stationKind == StationKind::Terminal
				? Array<Vec2>{{38,42},{28,32},{20,24},{14,18},{12,14},{10,16}} : Array<Vec2>{{24,28},{20,24},{14,18},{12,14},{10,16}};
			int roadOverlap = 0, uneven = 0, noStreet = 0, blockedPath = 0;
			// 駅舎の出口に横断道路が接している駅では、道を渡った先の街区に広場を設ける。
			constexpr std::array<double,3> kPlazaOffsets{0.0,16.0,32.0};
			for (size_t attempt = 0; attempt < kPlazaOffsets.size() * sizes.size() && !node->forecourt; ++attempt)
			{
				const Vec2 size = sizes[attempt % sizes.size()];
				Vec3 origin = entrance + frame->along * kPlazaOffsets[attempt / sizes.size()];
				origin.y = world.sampleHeight(static_cast<float>(origin.x),static_cast<float>(origin.z));
				const Frame surface{origin,frame->along,frame->right};
				StationForecourt proposed{origin,size,{}};
				const auto footprint = forecourtFootprint(*frame,proposed);
				if (occupied.overlaps(footprint)) { ++roadOverlap; continue; }
				bool valid = true; double low = Math::Inf, high = -Math::Inf;
				const int rows = static_cast<int>(Ceil(size.y / kSurveyStep)), columns = static_cast<int>(Ceil(size.x / kSurveyStep));
				for (int row = 0; row <= rows; ++row) { for (int column = 0; column <= columns; ++column)
				{
					const Vec3 p = surface.point(4 - size.x + size.x * column / columns,0,size.y * row / rows);
					const double height = world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
					valid &= p.x >= 0 && p.z >= 0 && p.x < WORLD_SIZE && p.z < WORLD_SIZE
						&& height > world.waterPlanningHeight(p.x,p.z) + 1;
					low = Min(low,height); high = Max(high,height);
				} }
				if (!valid || high - low > kMaximumRelief) { ++uneven; continue; }
				// 道路中心ではなく広場側の歩道端へ接続する。線路を横断する接続は除外。
				// 最寄りの道へ別の道路を横切らずに歩けない場合は、次に近い道を試す。
				const Vec3 center = surface.point(4 - size.x * .5,0,size.y * .5);
				Array<std::pair<double,Vec3>> candidates;
				for (const auto& edge : roads.edges())
				{
					if (edge.id < 0 || !edge.hasRoadLanes() || !edge.isRoadbedBuilt() || edge.useElevation || edge.tunnel || edge.roadType == RoadType::Expressway) { continue; }
					const auto curve = roads.getBezier(edge.id); if (!curve) { continue; }
					const int samples = Max(1,static_cast<int>(Ceil(curve->totalLength / kSurveyStep)));
					Optional<std::pair<double,Vec3>> best;
					for (int sample = 0; sample <= samples; ++sample)
					{
						const float arc = curve->totalLength * sample / samples;
						Vec3 p = curve->positionAt(arc); const Vec3 right = tangentToRight(curve->tangentAt(arc));
						p += right * ((center - p).dot(right) > 0 ? 1 : -1) * (edge.totalWidth() * .5 - .5);
						if ((p - frame->origin).dot(frame->right) > -8) { continue; }
						const double distance = Vec2{p.x - center.x,p.z - center.z}.lengthSq();
						if (distance < Square(kMaximumStreetDistance) && (!best || distance < best->first))
						{
							p.y = world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)); best = std::pair{distance,p};
						}
					}
					if (best) { candidates << *best; }
				}
				if (candidates.isEmpty()) { ++noStreet; continue; }
				candidates.sort_by([](const auto& a,const auto& b) { return a.first < b.first; });
				Optional<Vec3> access;
				constexpr size_t kMaximumAccessCandidates = 6;
				for (size_t index = 0; index < Min(candidates.size(),kMaximumAccessCandidates) && !access; ++index)
				{
					const Vec3 target = candidates[index].second;
					Vec3 previous = center; previous.y = world.sampleHeight(static_cast<float>(center.x),static_cast<float>(center.z));
					const int samples = Max(1,static_cast<int>(Ceil(Sqrt(candidates[index].first) / kSurveyStep)));
					constexpr double kMaximumWalkGrade = .12;
					constexpr double kConnectionHalfWidth = 1.5;
					bool walkable = true;
					for (int sample = 1; sample <= samples && walkable; ++sample)
					{
						Vec3 point = center.lerp(target,static_cast<double>(sample) / samples);
						point.y = world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
						walkable = point.y > world.waterPlanningHeight(point.x,point.z) + 1
							&& Abs(point.y - previous.y) <= Vec2{point.x - previous.x,point.z - previous.z}.length() * kMaximumWalkGrade
							&& (sample + 1 >= samples || !occupied.overlaps(ParcelGeometry::footprint({point.x,point.z},kConnectionHalfWidth,0)));
						previous = point;
					}
					if (walkable) { access = target; }
				}
				if (!access) { ++blockedPath; continue; }
				proposed.streetAccess = *access; node->forecourt = proposed; node->entrance = entrance; ++created;
				DBG_LOG(U"[StationForecourt] id={} name={} position={} offset={} size={} street={} relief={}"_fmt(id,node->name,entrance,kPlazaOffsets[attempt / sizes.size()],size,*access,high - low));
				break;
			}
			if (!node->forecourt)
			{
				DBG_LOG(U"[StationForecourt] id={} position={} reason=no-clear-ground-with-street-access roadOverlap={} uneven={} noStreet={} blockedPath={}"_fmt(
					id,node->position,roadOverlap,uneven,noStreet,blockedPath));
			}
		}
		DBG_LOG(U"[StationForecourt] created={} stations={}"_fmt(created,stations.size()));
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
				if (node.forecourt)
				{
					const auto& site = *node.forecourt; result << forecourtFootprint(*frame,site);
					const Frame surface{site.origin,frame->along,frame->right};
					const Vec3 center = surface.point(4 - site.size.x * .5,0,site.size.y * .5);
					Vec3 along = site.streetAccess - center; along.y = 0; const double length = along.length();
					if (length > .1)
					{
						along /= length; result << rectangle({center,along,{along.z,0,-along.x}},-1.5,1.5,0,length);
					}
				}
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
