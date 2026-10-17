#include "AgriculturalLayout.hpp"
#include "ParcelRoadIndex.hpp"
#include "SettlementPlan.hpp"
#include "StreetProfile.hpp"
#include "UrbanParcel.hpp"
#include "RoadDesignLimits.hpp"
#include "../road/RoadPlanDraft.hpp"
#include "../debug/DebugLog.hpp"

namespace AgriculturalLayout
{
	namespace
	{
		constexpr double kCellSize=static_cast<double>(CHUNK_SIZE)/ZONE_CELLS;
		Vec2 horizontal(Vec3 point) { return {point.x,point.z}; }
		Vec3 groundPoint(const World& world,Vec2 point) { return {point.x,world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)),point.y}; }
		bool agricultural(const World& world,Vec2 point)
		{
			if (point.x<0 || point.y<0 || point.x>=WORLD_SIZE || point.y>=WORLD_SIZE) { return false; }
			const auto river=world.rivers().nearest(point);
			if (river.reach>=0 && river.distance<=river.halfWidth+(river.halfWidth>=42 ? 45 : 18)) { return false; }
			const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});
			if (!chunk) { return false; }
			const Point local{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS};
			const ZoneType zone=chunk->zoneMap[local];
			// 日本の平野では田畑が住宅のすぐ裏まで続く。未開発の住宅用地と白地も、宅地化されるまでは耕作地にする。
			// 公共施設用地（市街化調整区域）だけは耕作地にしない。開発時に農地と決まったセルも耕作できる。
			return zone==ZoneType::Agriculture
				|| (zone!=ZoneType::UrbanControl && (chunk->buildingGrid[local].type==BuildingType::None || chunk->buildingGrid[local].type==BuildingType::Farmland));
		}
		/// @brief 都市計画上の農業地域。一般道沿いの農家を市街地の宅地予定地へ置かないために使う。
		bool farmingZone(const World& world,Vec2 point)
		{
			if (point.x<0 || point.y<0 || point.x>=WORLD_SIZE || point.y>=WORLD_SIZE) { return false; }
			const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});
			return chunk && chunk->zoneMap[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}]==ZoneType::Agriculture;
		}
		/// @brief 造成済みのニュータウンの内側か。
		bool insidePlannedTown(Vec2 point,const Array<Frame>& frames)
		{
			for (const auto& frame:frames)
			{
				if (frame.plannedHalfExtent.x<=0) { continue; }
				const Vec2 delta=point-frame.center;
				if (Abs(delta.dot(frame.axisX))<=frame.plannedHalfExtent.x && Abs(delta.dot(frame.axisZ))<=frame.plannedHalfExtent.y) { return true; }
			}
			return false;
		}
		/// @brief 町割の内側か。市街地の空き地には道路沿いの小さな田だけを残す。
		bool insideTown(Vec2 point,const Array<Frame>& frames)
		{
			for (const auto& frame:frames)
			{
				if (frame.townHalfExtent.x<=0) { continue; }
				const Vec2 delta=point-frame.center;
				if (Abs(delta.dot(frame.axisX))<=frame.townHalfExtent.x && Abs(delta.dot(frame.axisZ))<=frame.townHalfExtent.y) { return true; }
			}
			return false;
		}
		/// @brief 建物が建て込んだ市街地か。駅前や中心街の空き地には田畑を作らない。
		bool builtUp(const World& world,Vec2 point)
		{
			constexpr int kRadius=4;
			constexpr int kBuiltLimit=30;
			const Point center{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			int built=0;
			for (int dz=-kRadius;dz<=kRadius;++dz) { for (int dx=-kRadius;dx<=kRadius;++dx)
			{
				const Point cell=center+Point{dx,dz};
				if (cell.x<0 || cell.y<0) { continue; }
				const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});if (!chunk) { continue; }
				const auto type=chunk->buildingGrid[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}].type;
				built+=type!=BuildingType::None && type!=BuildingType::Farmland && type!=BuildingType::RuralHouse && type!=BuildingType::VillageHouse;
			} }
			return built>kBuiltLimit;
		}
		uint32 hash(uint64 seed,Vec2 point)
		{
			return static_cast<uint32>(UrbanMorphology::mix(seed^(static_cast<uint64>(static_cast<uint32>(Floor(point.x)))<<32)^static_cast<uint32>(Floor(point.y))));
		}
		Array<Frame> localFrames(const RoadNetwork& roads,const Array<Frame>& frames)
		{
			if (!frames.isEmpty()) { return frames; }
			Vec2 center{};int count=0;
			for (const auto& node:roads.nodes()) { if (node.id>=0) { center+=horizontal(node.position);++count; } }
			Frame frame;frame.center=count ? center/count : Vec2{};frame.origin=frame.center;
			return {frame};
		}
		double settlementDistance(Vec2 point,const Array<Frame>& frames)
		{
			double distance=Math::Inf;
			for (const auto& frame:frames) { distance=Min(distance,point.distanceFrom(frame.center)); }
			return distance;
		}
		double density(Vec2 point,const Array<Frame>& frames)
		{
			const auto& config=GenerationSettings::get();const double distance=settlementDistance(point,frames);
			return distance>config.agriculture_maximumRadius ? 0 : Exp(-Square(distance/config.agriculture_densityRadius)*config.agriculture_densityFalloff);
		}
		ParcelGeometry::Quad ribbon(Vec2 a,Vec2 b,double halfWidth)
		{
			const Vec2 delta=b-a;const Vec2 normal=Vec2{-delta.y,delta.x}.normalized()*halfWidth;
			return {a-normal,b-normal,b+normal,a+normal};
		}
		/// @brief 実在する建物の占有面と農家位置。新しい農地が家を覆うことも防ぐ。
		struct Sites
		{
			RoadNetwork empty;
			ParcelRoadIndex occupied{empty};
			Array<Vec2> homes;
			explicit Sites(const World& world)
			{
				for (int z=0;z<WORLD_CHUNKS;++z) { for (int x=0;x<WORLD_CHUNKS;++x)
				{
					const auto* chunk=world.getChunk({x,z});if (!chunk) { continue; }
					for (const auto& patch:chunk->landPatches)
					{
						if (patch.type==LandPatchType::FarmField || patch.type==LandPatchType::PaddyField) { continue; }
						for (size_t i=1;i+1<patch.polygon.size();++i) { occupied.add({patch.polygon[0],patch.polygon[i],patch.polygon[i+1],patch.polygon[i+1]}); }
					}
					for (int row=0;row<ZONE_CELLS;++row) { for (int col=0;col<ZONE_CELLS;++col)
					{
						const auto& building=chunk->buildingGrid[{col,row}];
						if (building.type==BuildingType::None || building.type==BuildingType::Farmland) { continue; }
						const Vec2 center{(x*ZONE_CELLS+col+.5)*kCellSize+building.offsetX,(z*ZONE_CELLS+row+.5)*kCellSize+building.offsetZ};
						occupied.add(ParcelGeometry::footprint(center,buildingFootprintXZ(building.type)*.5+3,building.angle));
						// 平野の耕作者は農家だけでなく、集落縁の戸建てにも住む。
						if (building.type==BuildingType::RuralHouse || building.type==BuildingType::VillageHouse || building.type==BuildingType::Detached) { homes << center; }
					} }
				} }
			}
			bool served(const Array<Vec2>& polygon,double maximum) const
			{
				for (const Vec2 home:homes)
				{
					if (home.distanceFromSq(polygon.front())>maximum*maximum) { continue; }
					bool within=true;for (const Vec2 point:polygon) { within &= point.distanceFromSq(home)<=maximum*maximum; }
					if (within) { return true; }
				}
				return false;
			}
		};
		int prepareHomes(World& world,const RoadNetwork& roads,uint64 seed,const TrainNetwork* railway,const Array<Frame>& frames)
		{
			const auto& config=GenerationSettings::get();ParcelRoadIndex occupied{roads,true};if (railway) { occupied.addRailway(*railway); }
			Sites sites{world};int placed=0;
			const double half=buildingFootprintXZ(BuildingType::RuralHouse)*.5;
			/// 道路の指定位置の左右どちらかへ、道に面した農家を1軒置く。
			const auto place=[&](const RoadEdge& edge,const CubicBezier& curve,float arc,bool farmRoad)
			{
				const Vec3 road=curve.positionAt(arc),right3=tangentToRight(curve.tangentAt(arc));
				for (const int side:{-1,1})
				{
					const Vec2 outward=horizontal(right3)*side,point=horizontal(road)+outward*(edge.totalWidth()*.5+half+config.agriculture_homeSetback);
					if (!agricultural(world,point) || (!farmRoad && !farmingZone(world,point))) { continue; }
					const double angle=Atan2(-outward.x,outward.y);const auto footprint=ParcelGeometry::footprint(point,half+config.agriculture_homeFootprintMargin,angle);
					if (occupied.overlaps(footprint) || sites.occupied.overlaps(footprint)) { continue; }
					double low=Math::Inf,high=-Math::Inf;bool valid=true;
					for (const auto corner:footprint)
					{
						const auto ground=groundPoint(world,corner);low=Min(low,ground.y);high=Max(high,ground.y);
						valid &= agricultural(world,corner) && ground.y>world.waterPlanningHeight(corner.x,corner.y)+config.agriculture_minimumFreeboard;
					}
					if (!valid || high-low>config.development_maximumBuildingRelief) { continue; }
					const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
					auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});auto& building=chunk->buildingGrid[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}];
					if (building.type!=BuildingType::None && building.type!=BuildingType::Farmland) { continue; }
					building.type=BuildingType::RuralHouse;building.angle=static_cast<float>(angle);building.edgeId=edge.id;building.edgeT=curve.tFromArcLength(arc);
					building.offsetX=static_cast<float>(point.x-(cell.x+.5)*kCellSize);building.offsetZ=static_cast<float>(point.y-(cell.y+.5)*kCellSize);
					chunk->zoneMap[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}]=ZoneType::LowResidential;chunk->markMeshDirty();
					sites.occupied.add(footprint);sites.homes << point;++placed;
					return true;
				}
				return false;
			};
			// 日本の平野の農家は等間隔に散らばらず、数軒が道沿いに寄り合う小さな集落をつくる。
			constexpr int kMaximumNeighbors=4;
			constexpr double kNeighborGap=8;
			const double neighborStep=half*2+kNeighborGap;
			for (const auto& edge:roads.edges())
			{
				if (edge.id<0 || edge.useElevation || edge.tunnel) { continue; }
				// 農家は農道だけでなく、田園を通る集落道・旧道沿いにも点在する。一般道では農業地域に限る。
				const bool farmRoad=edge.farmAccess;
				if (!farmRoad && (edge.roadType!=RoadType::LocalRoad || !edge.hasRoadLanes() || !edge.isRoadbedBuilt())) { continue; }
				const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
				for (float arc=curve->totalLength*.5f;arc<curve->totalLength;arc+=static_cast<float>(config.agriculture_homeSpacing))
				{
					const Vec2 road=horizontal(curve->positionAt(arc));
					if (settlementDistance(road,frames)>config.agriculture_maximumRadius) { continue; }
					if (sites.served({road},config.agriculture_homeSpacing)) { continue; }
					if (!place(edge,*curve,arc,farmRoad)) { continue; }
					const int neighbors=static_cast<int>(hash(seed,road)%(kMaximumNeighbors+1));
					for (int index=1;index<=neighbors;++index)
					{
						const double offset=neighborStep*((index+1)/2)*(index%2 ? 1 : -1);
						if (arc+offset<half || arc+offset>curve->totalLength-half) { continue; }
						place(edge,*curve,static_cast<float>(arc+offset),farmRoad);
					}
				}
			}
			return placed;
		}
	}

	Stats prepare(World& world,RoadNetwork& roads,uint64 seed,const Array<Frame>& inputFrames,const TrainNetwork* railway)
	{
		Stats stats;const auto& config=GenerationSettings::get();const auto frames=localFrames(roads,inputFrames);
		constexpr double kJunctionMargin = 32;
		constexpr double kFieldProbeHalfDepth = 20;
		constexpr double kMinimumSupportRatio = .65;
		/// @brief 描画用の短いエッジ分割を、ほ場計画の区切りと取り違えない連続沿道。
		struct Frontage
		{
			Array<CubicBezier> curves;
			float totalLength = 0;
			Vec3 positionAt(float arc) const
			{
				for (const auto& curve : curves) { if (arc <= curve.totalLength) { return curve.positionAt(arc); } arc -= curve.totalLength; }
				return curves.back().p3;
			}
			Vec3 tangentAt(float arc) const
			{
				for (const auto& curve : curves) { if (arc <= curve.totalLength) { return curve.tangentAt(arc); } arc -= curve.totalLength; }
				return curves.back().tangentAt(curves.back().totalLength);
			}
		};
		const auto eligible = [](const RoadEdge& edge)
		{
			return edge.id >= 0 && edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.useElevation && !edge.tunnel
				&& !edge.farmAccess && edge.roadType != RoadType::Expressway;
		};
		const auto nextEdge = [&](int node,int previous) -> Optional<int>
		{
			const auto* incoming = roads.getEdge(previous);
			const auto curve = roads.getBezier(previous);
			const Vec3 reverse = curve->tangentAt(incoming->nodeA == node ? 0 : curve->totalLength) * (incoming->nodeA == node ? 1 : -1);
			Optional<int> next;
			for (const auto& attachment : roads.getNode(node)->attachments)
			{
				const auto* edge = roads.getEdge(attachment.edgeId);
				if (attachment.edgeId == previous || !edge || !eligible(*edge)) { continue; }
				if (next || edge->roadType != incoming->roadType) { return none; }
				const auto other = roads.getBezier(edge->id);
				const Vec3 direction = other->tangentAt(edge->nodeA == node ? 0 : other->totalLength) * (edge->nodeA == node ? 1 : -1);
				if (horizontal(reverse).normalized().dot(horizontal(direction).normalized()) > -.9) { return none; }
				next = edge->id;
			}
			return next;
		};
		Array<Frontage> frontages; HashSet<int> visited;
		for (const auto& edge : roads.edges())
		{
			if (!eligible(edge) || visited.contains(edge.id)) { continue; }
			int first = edge.id, start = edge.nodeA; HashSet<int> backwards{first};
			while (const auto previous = nextEdge(start,first))
			{
				if (visited.contains(*previous) || !backwards.insert(*previous).second) { break; }
				const auto* item = roads.getEdge(*previous); start = item->nodeA == start ? item->nodeB : item->nodeA; first = *previous;
			}
			Frontage frontage; int id = first, from = start;
			while (visited.insert(id).second)
			{
				const auto* item = roads.getEdge(id); const auto curve = *roads.getBezier(id);
				const bool forward = item->nodeA == from;
				frontage.curves << (forward ? curve : CubicBezier{curve.p3,curve.p2,curve.p1,curve.p0});
				frontage.totalLength += curve.totalLength;
				const int to = forward ? item->nodeB : item->nodeA; const auto next = nextEdge(to,id);
				if (!next || visited.contains(*next)) { break; }
				id = *next; from = to;
			}
			if (!frontage.curves.isEmpty()) { frontages << std::move(frontage); }
		}
		ParcelRoadIndex occupied{roads,true}; RoadNetwork empty; ParcelRoadIndex rail{empty};
		if (railway) { rail.addRailway(*railway); }
		Sites buildings{world}; RoadPlanSnapIndex connections; connections.rebuild(roads);
		const auto probe = [&](Vec3 position)
		{
			constexpr double kSnapTolerance = 2;
			return connections.find(roads,position,kSnapTolerance,kSnapTolerance,false,
				[](const RoadEdge& edge) { return edge.hasRoadLanes() && !edge.farmAccess; });
		};
		/// 接続点で既存道を分割し、取り付け先の節点を返す。
		const auto attachHit = [&](const RoadPlanSnapIndex::Hit& hit) -> Optional<int>
		{
			if (!hit.edgeId) { return none; }
			const auto* edge = roads.getEdge(*hit.edgeId);
			if (!edge) { return none; }
			if (hit.curveT == 0) { return edge->nodeA; }
			if (hit.curveT == 1) { return edge->nodeB; }
			const auto curve = roads.getBezier(edge->id);
			const float sample = hit.curveT * CubicBezier::SAMPLES;
			const int index = Min(CubicBezier::SAMPLES - 1,static_cast<int>(sample));
			const int node = roads.splitEdgeAt(edge->id,Math::Lerp(curve->arcTable[index],curve->arcTable[index + 1],sample - index));
			if (node < 0) { return none; }
			for (const int child : roads.getNode(node)->edgeIds()) { connections.appendEdge(roads,child); }
			return node;
		};
		/// @brief 両端の節点が決まった農道を追加する。途中で追加に失敗したら、この農道の辺と節点を戻す。
		const auto commitTrack = [&](int first,int last,const Array<CubicBezier>& curves)
		{
			Array<int> nodes{first}, addedEdges;
			for (size_t i = 0; i + 1 < curves.size(); ++i) { nodes << roads.addNode(curves[i].p3,NodeType::Joint); }
			nodes << last;
			bool valid = true;
			for (size_t i = 0; i < curves.size(); ++i)
			{
				const auto id = roads.addEdge(nodes[i],nodes[i + 1],curves[i].p1,curves[i].p2,RoadType::LocalRoad,2);
				if (!id) { valid = false; break; }
				auto* edge = roads.getEdge(*id); auto profile = GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess);
				profile.laneWidth = static_cast<float>(config.agriculture_trackWidth / profile.lanes);
				GeneratedStreet::apply(*edge,profile); edge->edgeState = EdgeState::Existing; edge->farmAccess = true;
				edge->parts.remove_if([](const RoadPart& part) { return part.type == RoadPartType::UtilityPole; });
				addedEdges << *id;
			}
			if (!valid)
			{
				for (const int id : addedEdges) { roads.removeEdge(id); }
				for (size_t i = 1; i + 1 < nodes.size(); ++i) { roads.removeNode(nodes[i]); }
				return false;
			}
			for (const int id : addedEdges)
			{
				const auto curve = roads.getBezier(id);
				for (int i = 1; i <= 8; ++i) { occupied.add(ribbon(horizontal(curve->evaluate((i - 1) / 8.0f)),horizontal(curve->evaluate(i / 8.0f)),config.agriculture_trackWidth * .5 + 1)); }
				connections.appendEdge(roads,id);
			}
			for (const int node : nodes) { roads.updateNodeCutoffs(node); roads.rebuildLaneConnections(node); }
			stats.tracks += static_cast<int>(addedEdges.size()); stats.connections += 2;
			return true;
		};
		int blocks = 0, rejected = 0;
		Array<Vec2> blockFrontage;
		// 耕区の長辺に沿う農道を先に計画し、両端だけを既存道へ取り付ける。
		// 全区間と耕作可能な帯を確認するまで、道路や交差点を追加しない。
		for (const auto& frontage : frontages)
		{
			for (double begin = kJunctionMargin; begin + config.agriculture_accessSpacing < frontage.totalLength - kJunctionMargin;
				begin += config.agriculture_trackReach + config.agriculture_accessSpacing)
			{
				const double end = Min(begin + config.agriculture_trackReach, frontage.totalLength - kJunctionMargin);
				const Vec2 center = horizontal(frontage.positionAt(static_cast<float>((begin + end) * .5)));
				const double probability = density(center,frames);
				if (probability < config.agriculture_minimumDensity || (hash(seed,center) % 10000) / 10000.0 > probability) { continue; }
				for (const int side : {-1,1})
				{
					const double depth = config.agriculture_plotWidth * 2;
					const int sections = Max(3,static_cast<int>(Ceil((end - begin) / config.agriculture_trackStep)));
					Array<Vec3> points{frontage.positionAt(static_cast<float>(begin))};
					int supported = 0;
					for (int section = 0; section <= sections; ++section)
					{
						const float arc = static_cast<float>(Math::Lerp(begin,end,static_cast<double>(section) / sections));
						const Vec3 base = frontage.positionAt(arc);
						const Vec2 normal = horizontal(tangentToRight(frontage.tangentAt(arc))) * side;
						const Vec2 point = horizontal(base) + normal * depth;
						points << groundPoint(world,point);
						// 並行道の両側に、道路幅とは別に実際のほ場を置ける土地が必要。
						const Vec2 along = horizontal(frontage.tangentAt(arc)).normalized();
						bool usable = true; double low = Math::Inf, high = -Math::Inf;
						for (const int x : {-1,1}) { for (const int z : {-1,1})
						{
							const Vec2 fieldProbe = point + along * (config.agriculture_plotLength * .2 * x) + normal * (kFieldProbeHalfDepth * z);
							const Vec3 ground = groundPoint(world,fieldProbe);
							usable &= agricultural(world,fieldProbe) && ground.y > world.waterPlanningHeight(fieldProbe.x,fieldProbe.y) + config.agriculture_minimumFreeboard
								&& !rail.overlaps(ParcelGeometry::footprint(fieldProbe,kFieldProbeHalfDepth * .5,0))
								&& !buildings.occupied.overlaps(ParcelGeometry::footprint(fieldProbe,kFieldProbeHalfDepth * .5,0));
							low = Min(low,ground.y); high = Max(high,ground.y);
						} }
						supported += usable && high - low <= config.agriculture_maximumFieldRelief;
					}
					points << frontage.positionAt(static_cast<float>(end));
					if (supported < (sections + 1) * kMinimumSupportRatio) { ++rejected; continue; }
					Array<CubicBezier> curves; bool valid = true;
					for (size_t piece = 1; piece < points.size(); ++piece)
					{
						const Vec3 a = points[piece - 1], b = points[piece];
						const CubicBezier curve{a,a.lerp(b,1.0/3),a.lerp(b,2.0/3),b};
						const int samples = Max(1,static_cast<int>(Ceil(curve.totalLength / config.agriculture_sampleStep)));
						Vec3 last = a;
						for (int sample = 1; sample <= samples; ++sample)
						{
							const Vec3 p = curve.positionAt(curve.totalLength * sample / samples), ground = groundPoint(world,horizontal(p));
							const auto footprint = ParcelGeometry::footprint(horizontal(p),config.agriculture_trackWidth * .5 + 1,0);
							const bool atJunction = (piece == 1 && p.distanceFrom(points.front()) < kJunctionMargin)
								|| (piece + 1 == points.size() && p.distanceFrom(points.back()) < kJunctionMargin);
							valid &= (atJunction || agricultural(world,horizontal(p)))
								&& ground.y > world.waterPlanningHeight(p.x,p.z) + config.agriculture_minimumFreeboard
								&& Abs(ground.y - p.y) <= config.agriculture_maximumTrackCutFill
								&& Abs(ground.y - last.y) <= horizontal(ground - last).length() * config.agriculture_maximumTrackGrade
								&& !rail.overlaps(footprint) && !buildings.occupied.overlaps(footprint)
								&& (atJunction || !occupied.overlaps(footprint));
							last = ground;
						}
						curves << curve;
					}
					if (!valid) { ++rejected; continue; }
					// 片端だけ分割して取り付けに失敗すると不要な節点が残るため、両端の接続先を先に確かめる。
					if (!probe(points.front()).edgeId || !probe(points.back()).edgeId) { ++rejected; continue; }
					const auto first = attachHit(probe(points.front())), last = attachHit(probe(points.back()));
					if (!first || !last || !commitTrack(*first,*last,curves)) { ++rejected; continue; }
					++blocks;
					for (double arc = begin; arc <= end; arc += kJunctionMargin) { blockFrontage << horizontal(frontage.positionAt(static_cast<float>(arc))); }
				}
			}
		}
		DBG_LOG(U"[AgriculturalBlocks] seed={} blocks={} rejected={} tracks={} connections={}"_fmt(seed,blocks,rejected,stats.tracks,stats.connections));
		// 区画農道を通せない細長い耕地や山際の谷田には、既存道から地形に沿う枝農道を伸ばす。
		// 経路全体を計画し、別の道へ戻れる枝だけを追加するので、行き止まりや使われない分岐点を残さない。
		struct Root { Vec3 position; Optional<int> node; Vec2 direction; double reach; };
		Array<Root> roots; HashSet<Point> rootCells;
		const double rootSpacing = config.agriculture_accessSpacing * config.agriculture_rootSpacingRatio;
		const auto addRoots = [&](Vec3 position,Optional<int> node,Vec2 normal)
		{
			const Vec2 point = horizontal(position); const double probability = density(point,frames);
			if (probability < config.agriculture_minimumDensity || (hash(seed,point) % 10000) / 10000.0 > probability) { return; }
			if (Abs(groundPoint(world,point).y - position.y) > 1) { return; }
			// 区画農道を整備した沿道では、その間へ直交する枝道を足さず、耕区の並行配置を保つ。
			if (blockFrontage.any([&](Vec2 block) { return block.distanceFromSq(point) < Square(config.agriculture_accessSpacing * .5); })) { return; }
			if (!rootCells.insert(Point{static_cast<int>(point.x / rootSpacing),static_cast<int>(point.y / rootSpacing)}).second) { return; }
			const double reach = config.agriculture_trackReach * (config.agriculture_minimumReachRatio + (1 - config.agriculture_minimumReachRatio) * probability);
			for (const int side : {-1,1})
			{
				// 町の道路沿いにある住宅帯を越え、最初の区間の先が農地なら取り付けを許可する。
				if (agricultural(world,point + normal * side * config.agriculture_trackStep)) { roots << Root{position,node,normal * side,reach}; }
			}
		};
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0 || node.attachments.isEmpty() || node.attachments.size() > 3) { continue; }
			const auto* edge = roads.getEdge(node.attachments.front().edgeId);
			if (!edge || !eligible(*edge)) { continue; }
			const auto curve = roads.getBezier(edge->id);
			addRoots(node.position,node.id,horizontal(tangentToRight(curve->tangentAt(edge->nodeA == node.id ? 0 : curve->totalLength))));
		}
		// 長い沿道では交差点の間からも枝を出す。分岐点は枝を採用したときだけ作る。
		for (const auto& frontage : frontages)
		{
			for (double arc = config.agriculture_accessSpacing; arc < frontage.totalLength - config.agriculture_accessSpacing * .5; arc += config.agriculture_accessSpacing)
			{
				const Vec3 position = frontage.positionAt(static_cast<float>(arc));
				if (!agricultural(world,horizontal(position))) { continue; }
				addRoots(position,none,horizontal(tangentToRight(frontage.tangentAt(static_cast<float>(arc)))));
			}
		}
		const auto connectable = [](const RoadEdge& edge)
		{
			return edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.tunnel && !edge.useElevation && edge.roadType != RoadType::Expressway;
		};
		/// @brief 枝農道の1区間が、耕地・勾配・切盛り・既存施設の条件を満たすか調べる。
		const auto spurValid = [&](const CubicBezier& curve,bool leaving,double joiningWidth)
		{
			Vec3 last = curve.p0;
			const int samples = Max(3,static_cast<int>(Ceil(curve.totalLength / config.agriculture_sampleStep)));
			for (int i = 1; i <= samples; ++i)
			{
				const Vec3 p = curve.positionAt(curve.totalLength * i / samples), ground = groundPoint(world,horizontal(p));
				const auto footprint = ParcelGeometry::footprint(horizontal(p),config.agriculture_trackWidth * .5 + 1,0);
				const bool joining = p.distanceFrom(curve.p3) < joiningWidth;
				if (!(leaving || joining || agricultural(world,horizontal(p)))
					|| ground.y <= world.waterPlanningHeight(p.x,p.z) + config.agriculture_minimumFreeboard
					|| Abs(ground.y - last.y) > horizontal(p - last).length() * config.agriculture_maximumTrackGrade
					|| Abs(ground.y - p.y) >= config.agriculture_maximumTrackCutFill
					|| rail.overlaps(footprint) || buildings.occupied.overlaps(footprint)
					|| (p.distanceFrom(curve.p0) > 12 && !joining && occupied.overlaps(footprint))) { return false; }
				last = ground;
			}
			return true;
		};
		int spurs = 0, abandoned = 0;
		for (const auto& root : roots)
		{
			if (root.node && (!roads.getNode(*root.node) || roads.getNode(*root.node)->id < 0)) { continue; }
			const auto awayFromRoot = [&](const RoadEdge& edge)
			{
				return connectable(edge) && (!root.node || (edge.nodeA != *root.node && edge.nodeB != *root.node));
			};
			Array<CubicBezier> path; Optional<RoadPlanSnapIndex::Hit> finish;
			Vec3 start = root.position; Vec2 direction = root.direction;
			const double phase = (hash(seed,horizontal(root.position)) % 10000) / 10000.0 * Math::TwoPi;
			for (double run = 0; run < root.reach && !finish; run += config.agriculture_trackStep)
			{
				Optional<CubicBezier> best; Optional<RoadPlanSnapIndex::Hit> bestConnection; Vec2 bestDirection = direction; double bestCost = Math::Inf;
				// 乱数を区間ごとに振らず、連続した蛇行の目標方向を地形費用と比較する。
				const double bend = config.agriculture_meanderAngle * (Sin(phase + run * Math::TwoPi / config.agriculture_meanderWavelength) - Sin(phase));
				const Vec2 preferred{root.direction.x * Cos(bend) - root.direction.y * Sin(bend),root.direction.x * Sin(bend) + root.direction.y * Cos(bend)};
				for (const int turn : {0,-1,1,-2,2})
				{
					const double angle = turn * config.agriculture_trackHeadingStep;
					const Vec2 next{direction.x * Cos(angle) - direction.y * Sin(angle),direction.x * Sin(angle) + direction.y * Cos(angle)};
					if (next.dot(root.direction) < .25) { continue; }
					Vec3 end = groundPoint(world,horizontal(start) + (direction + next).normalized() * config.agriculture_trackStep);
					Optional<RoadPlanSnapIndex::Hit> connection;
					const auto hit = connections.find(roads,end,config.agriculture_connectionRadius,config.agriculture_maximumTrackCutFill,false,awayFromRoot);
					if (hit.edgeId)
					{
						const Vec2 span = horizontal(hit.position - start);
						if (span.length() > 12 && span.normalized().dot(direction) > .5 && hit.position.distanceFrom(root.position) > config.agriculture_trackStep) { end = hit.position; connection = hit; }
					}
					if (!connection && !agricultural(world,horizontal(end))) { continue; }
					const double handle = horizontal(end - start).length() / 3;
					const CubicBezier curve{start,{start.x + direction.x * handle,Math::Lerp(start.y,end.y,1.0/3),start.z + direction.y * handle},
						{end.x - next.x * handle,Math::Lerp(start.y,end.y,2.0/3),end.z - next.y * handle},end};
					if (curve.minimumHorizontalRadius() < RoadDesignLimits::forType(RoadType::LocalRoad).minimumRadius) { continue; }
					const double joiningWidth = connection ? roads.getEdge(*connection->edgeId)->totalWidth() * .5 + config.agriculture_trackWidth * .5 + 3 : 0;
					if (!spurValid(curve,run == 0,joiningWidth)) { continue; }
					double reliefCost = 0;
					for (int i = 1; i <= 4; ++i) { reliefCost += Square(groundPoint(world,horizontal(curve.evaluate(i / 4.0f))).y - groundPoint(world,horizontal(curve.evaluate((i - 1) / 4.0f))).y); }
					const double cost = reliefCost + Square(angle) * config.agriculture_trackTurnPenalty + (next - preferred).lengthSq() * config.agriculture_trackDirectionPenalty;
					if ((!bestConnection && connection) || (static_cast<bool>(bestConnection) == static_cast<bool>(connection) && cost < bestCost))
					{
						best = curve; bestDirection = next; bestCost = cost; bestConnection = connection;
					}
				}
				if (!best) { break; }
				path << *best; start = best->p3; direction = bestDirection; finish = bestConnection;
			}
			if (path.isEmpty()) { continue; }
			if (!finish)
			{
				// 届かなかった枝は、最寄りの道へ緩い曲線で戻せる場合だけ残す。
				const auto hit = connections.find(roads,start,config.agriculture_deadEndConnectionRadius,
					config.agriculture_deadEndConnectionRadius * config.agriculture_maximumTrackGrade,false,awayFromRoot);
				if (hit.edgeId && hit.position.distanceFrom(root.position) > config.agriculture_trackStep)
				{
					const Vec3 tangent = (path.back().p3 - path.back().p2).normalized(), span = hit.position - start;
					const double length = horizontal(span).length();
					if (length >= 24 && horizontal(span).normalized().dot(horizontal(tangent).normalized()) >= -.25)
					{
						Vec3 handle = tangent * (length / 3); handle.y = span.y / 3;
						const CubicBezier curve{start,start + handle,hit.position - span / 3,hit.position};
						const double joiningWidth = roads.getEdge(*hit.edgeId)->totalWidth() * .5 + config.agriculture_trackWidth * .5 + 3;
						if (RoadAlignment::respectsLimits(curve,RoadType::LocalRoad) && spurValid(curve,false,joiningWidth)) { path << curve; finish = hit; }
					}
				}
			}
			if (!finish || (!root.node && !probe(root.position).edgeId)) { ++abandoned; continue; }
			// 終点側を先に分割する。始点が同じ辺上でも、分割後の辺から位置で取り直せる。
			const auto last = attachHit(*finish);
			const auto first = root.node ? root.node : (last ? attachHit(probe(root.position)) : none);
			if (!first || !last || *first == *last || !commitTrack(*first,*last,path)) { ++abandoned; continue; }
			++spurs;
		}
		DBG_LOG(U"[AgriculturalSpurs] seed={} roots={} spurs={} abandoned={} tracks={}"_fmt(seed,roots.size(),spurs,abandoned,stats.tracks));
		// 伸長が終わった農道だけを再検査し、最寄りの接続可能な道まで枝を延長する。
		Array<int> deadEnds;
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0 || node.attachments.size() != 1)
			{
				continue;
			}
			const auto* edge = roads.getEdge(node.attachments.front().edgeId);
			if (edge && edge->farmAccess)
			{
				deadEnds << node.id;
			}
		}
		for (int id : deadEnds)
		{
			const auto* node = roads.getNode(id);
			if (!node || node->attachments.size() != 1)
			{
				continue;
			}
			const auto* incoming = roads.getEdge(node->attachments.front().edgeId);
			const int parent = incoming->nodeA == id ? incoming->nodeB : incoming->nodeA;
			const Vec3 start = node->position;
			const auto incomingCurve = roads.getBezier(incoming->id);
			const Vec3 tangent = incomingCurve->tangentAt(incoming->nodeB == id ? incomingCurve->totalLength : 0) *
								 (incoming->nodeB == id ? 1 : -1);
			const auto hit = connections.find(roads, start, config.agriculture_deadEndConnectionRadius,
				config.agriculture_deadEndConnectionRadius * config.agriculture_maximumTrackGrade, false,
				[&](const RoadEdge& edge)
			{
				return edge.nodeA != id && edge.nodeB != id && edge.nodeA != parent && edge.nodeB != parent &&
					   edge.hasRoadLanes() && edge.isRoadbedBuilt() && !edge.tunnel && !edge.useElevation &&
					   edge.roadType != RoadType::Expressway;
			});
			if (!hit.edgeId)
			{
				continue;
			}
			const Vec3 end = hit.position, span = end - start;
			const double length = horizontal(span).length();
			if (length < 24 || horizontal(span).normalized().dot(horizontal(tangent).normalized()) < -.25)
			{
				continue;
			}
			Vec3 handle = tangent * (length / 3);
			handle.y = span.y / 3;
			const CubicBezier curve{start, start + handle, end - span / 3, end};
			if (!RoadAlignment::respectsLimits(curve, RoadType::LocalRoad))
			{
				continue;
			}
			const auto* target = roads.getEdge(*hit.edgeId);
			const double joinWidth = target->totalWidth() * .5 + config.agriculture_trackWidth * .5 + 3;
			bool valid = true;
			Vec3 last = start;
			const int samples = Max(1, static_cast<int>(Ceil(curve.totalLength / 8)));
			for (int sample = 1; sample <= samples; ++sample)
			{
				const double arc = curve.totalLength * sample / samples;
				const Vec3 point = curve.positionAt(static_cast<float>(arc)),
						   ground = groundPoint(world, horizontal(point));
				const auto footprint =
					ParcelGeometry::footprint(horizontal(point), config.agriculture_trackWidth * .5 + 1, 0);
				valid &= ground.y > world.waterPlanningHeight(point.x, point.z) + config.agriculture_minimumFreeboard &&
						 Abs(ground.y - point.y) < config.agriculture_maximumTrackCutFill &&
						 Abs(ground.y - last.y) <=
							 horizontal(ground - last).length() * config.agriculture_maximumTrackGrade &&
						 !rail.overlaps(footprint) && !buildings.occupied.overlaps(footprint);
				if (arc > 12 && point.distanceFrom(end) > joinWidth && occupied.overlaps(footprint))
				{
					valid = false;
				}
				if (!valid)
				{
					break;
				}
				last = ground;
			}
			if (!valid)
			{
				continue;
			}
			int endpoint = hit.curveT == 0 ? target->nodeA : target->nodeB;
			if (hit.curveT > 0 && hit.curveT < 1)
			{
				const auto targetCurve = roads.getBezier(target->id);
				const float sample = hit.curveT * CubicBezier::SAMPLES;
				const int index = Min(CubicBezier::SAMPLES - 1, static_cast<int>(sample));
				endpoint = roads.splitEdgeAt(target->id,
					Math::Lerp(targetCurve->arcTable[index], targetCurve->arcTable[index + 1], sample - index));
				if (endpoint < 0)
				{
					continue;
				}
				for (int child : roads.getNode(endpoint)->edgeIds())
				{
					connections.appendEdge(roads, child);
				}
			}
			if (const auto added = roads.addEdge(id, endpoint, curve.p1, curve.p2, RoadType::LocalRoad, 2))
			{
				auto* edge = roads.getEdge(*added);
				auto profile = GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess);
				profile.laneWidth = static_cast<float>(config.agriculture_trackWidth / profile.lanes);
				GeneratedStreet::apply(*edge, profile);
				edge->edgeState = EdgeState::Existing;
				edge->farmAccess = true;
				edge->parts.remove_if([](const RoadPart& part) { return part.type == RoadPartType::UtilityPole; });
				roads.rebuildNodeConnectivity(id, endpoint);
				connections.appendEdge(roads, *added);
				for (int i = 1; i <= 16; ++i)
				{
					occupied.add(ribbon(horizontal(curve.evaluate((i - 1) / 16.0f)),
						horizontal(curve.evaluate(i / 16.0f)), edge->totalWidth() * .5 + 1));
				}
				++stats.tracks;
				++stats.connections;
			}
		}
		stats.homes=prepareHomes(world,roads,seed,railway,frames);stats.drains=stats.tracks*2;return stats;
	}

	Stats generate(World& world,RoadNetwork& roads,uint64 seed,const Array<Frame>& inputFrames,bool prepareAccess,const TrainNetwork* railway)
	{
		Stats stats=prepareAccess ? prepare(world,roads,seed,inputFrames,railway) : Stats{};
		const auto& config=GenerationSettings::get();const auto frames=localFrames(roads,inputFrames);
		ParcelRoadIndex occupied{roads,true};if (railway) { occupied.addRailway(*railway); }Sites sites{world};
		// 田畑は道路に平行な長方形の短冊。1列目から順に置き、重なる区画は奥行きを縮めるか見送る。
		int sparse=0,unsuitable=0,crowded=0,blocked=0,uneven=0;
		constexpr double kFullCultivationDensity=1.25;
		struct Candidate { Vec2 center; Vec2 left, right, normalLeft, normalRight; double depth; int tier; uint32 salt; };
		Array<Candidate> candidates;
		const double bucketSize=config.agriculture_plotLength*2;
		const auto bucket=[&](Vec2 p){return Point{static_cast<int>(Floor(p.x/bucketSize)),static_cast<int>(Floor(p.y/bucketSize))};};
		for (const auto& edge:roads.edges())
		{
			if (edge.id<0) { continue; }
			// 平野の田畑は農道だけでなく一般の生活道路・県道の両側にも連続して広がる。
			if (!edge.farmAccess && (edge.roadType==RoadType::Expressway || edge.useElevation)) { continue; }
			if (!prepareAccess && edge.farmAccess) { ++stats.tracks;stats.drains+=2; }
			const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
			const int count=Max(1,static_cast<int>(Round(curve->totalLength/(config.agriculture_plotLength*.5))));
			// 平野の田畑は道路沿いの1列目から奥へ、畦道でつながって途切れずに続く。
			constexpr int kFieldTiers=7;
			for (int tier=0;tier<kFieldTiers;++tier) for (int section=0;section<count;++section) { for (int side:{-1,1})
			{
				const float begin=curve->totalLength*section/count,end=curve->totalLength*(section+1)/count;
				const Vec3 a0=curve->positionAt(begin),b0=curve->positionAt(end),n1=tangentToRight(curve->tangentAt(begin))*side,n2=tangentToRight(curve->tangentAt(end))*side;
				const double tierOffset=tier*config.agriculture_plotWidth*2;
				const Vec3 a=a0+n1*tierOffset,b=b0+n2*tierOffset;
				const Vec2 center=horizontal((a+b)*.5+(n1+n2)*config.agriculture_plotWidth*.5);
				const uint32 salt=hash(seed,center)&~kManagedField;
				// Fields occur in contiguous farming districts rather than isolated random plots.
				const Vec2 districtCell{Floor(center.x/bucketSize)*bucketSize,Floor(center.y/bucketSize)*bucketSize};
				const double districtRoll=(hash(seed,districtCell)%10000)/10000.0;
				const double plotRoll=(salt%10000)/10000.0;
				++stats.candidates;
				// 集落に近い平野はほぼ全面が耕作地になり、遠い縁辺部だけ地区単位で耕作放棄地や原野が混じる。
				if (districtRoll*.72+plotRoll*.28>Min(1.0,density(center,frames)*kFullCultivationDensity)) { ++sparse;continue; }
				constexpr int kTownFieldTiers=2;
				if (!agricultural(world,center) || builtUp(world,center) || insidePlannedTown(center,frames) || (tier>=kTownFieldTiers && insideTown(center,frames)))
				{
					++unsuitable;
					continue;
				}
				const double near=(tier==0 ? edge.totalWidth()*.5+config.agriculture_drainWidth : 0.0)+config.agriculture_bundWidth;
				const double depth=config.agriculture_plotWidth*2*(1+config.agriculture_fieldShapeVariation*(static_cast<double>((salt>>8)%100)/50-1))-config.agriculture_bundWidth;
				candidates << Candidate{center,horizontal(a+n1*near),horizontal(b+n2*near),horizontal(n1),horizontal(n2),depth,tier,salt};
			} }
		}
		candidates.stable_sort_by([](const Candidate& x,const Candidate& y){ return x.tier<y.tier; });
		HashTable<Point,Array<Polygon>> accepted;
		const auto overlapsAccepted=[&](const Polygon& shape,Vec2 center)
		{
			const Point cell=bucket(center);
			for (int dz=-1;dz<=1;++dz) { for (int dx=-1;dx<=1;++dx)
			{
				if (const auto found=accepted.find(cell+Point{dx,dz});found!=accepted.end())
				{
					for (const auto& other:found->second) { if (other.intersects(shape)) { return true; } }
				}
			} }
			return false;
		};
		enum class Placement { Placed, Crowded, Blocked, Uneven, Disconnected, Missing };
		/// @brief 区画の外形を地形・水域・道路・建物・既存の田畑と照合し、成立すれば田または畑として登録する。
		const auto placeField=[&](const Array<Vec2>& outline,uint32 salt)
		{
			const Vec2 center=Polygon{outline}.centroid();
			// 隣の田との間に畦の幅を残す。
			Array<Vec2> polygon;for (const Vec2 vertex:outline) { polygon << center.lerp(vertex,.97); }
			if (overlapsAccepted(Polygon{polygon},center)) { return Placement::Crowded; }
			bool valid=true;double low=Math::Inf,high=-Math::Inf;
			for (const Vec2 vertex:polygon)
			{
				const int samples=Max(1,static_cast<int>(Ceil(vertex.distanceFrom(center)/config.agriculture_sampleStep)));
				for (int i=0;i<=samples && valid;++i)
				{
					const Vec2 point=center.lerp(vertex,static_cast<double>(i)/samples);const auto ground=groundPoint(world,point);low=Min(low,ground.y);high=Max(high,ground.y);
					valid &= agricultural(world,point) && ground.y>world.waterPlanningHeight(point.x,point.y)+config.agriculture_minimumFreeboard;
				}
			}
			for (size_t i=1;i+1<polygon.size() && valid;++i)
			{
				const ParcelGeometry::Quad triangle{polygon[0],polygon[i],polygon[i+1],polygon[i+1]};
				valid &= !occupied.overlaps(triangle) && !sites.occupied.overlaps(triangle);
			}
			if (!valid) { return Placement::Blocked; }
			if (high-low>config.agriculture_maximumFieldRelief) { return Placement::Uneven; }
			if (!sites.served(polygon,config.agriculture_homeMaximumDistance)) { return Placement::Disconnected; }
			const Point coord{static_cast<int>(center.x/CHUNK_SIZE),static_cast<int>(center.y/CHUNK_SIZE)};auto* chunk=world.getChunk(coord);if (!chunk) { return Placement::Missing; }
			accepted[bucket(center)] << Polygon{polygon};
			LandPatch patch;patch.id=static_cast<int>(chunk->landPatches.size());patch.polygon=std::move(polygon);patch.elevationOffset=config.agriculture_surfaceLift;
			const bool paddy=high-low<config.agriculture_paddyRelief && salt%config.agriculture_dryFieldDivisor!=0;
			patch.type=paddy ? LandPatchType::PaddyField : LandPatchType::FarmField;patch.materialVariant=salt|kManagedField;
			chunk->landPatches << std::move(patch);chunk->markMeshDirty();++stats.fields;stats.paddies+=paddy;
			return Placement::Placed;
		};
		const auto count=[&](Placement result)
		{
			crowded+=result==Placement::Crowded;blocked+=result==Placement::Blocked;uneven+=result==Placement::Uneven;stats.disconnected+=result==Placement::Disconnected;
		};
		for (const auto& candidate:candidates)
		{
			for (const double fraction:{1.0,.7,.45})
			{
				const double depth=candidate.depth*fraction;
				// 奥の畦は区画ごとにわずかに折れ、地形に沿った不整形さを残す。
				const Vec2 farLeft=candidate.left+candidate.normalLeft*depth,farRight=candidate.right+candidate.normalRight*depth;
				const double bend=depth*(static_cast<double>((candidate.salt>>12)%9)-4.0)*.012;
				Array<Vec2> outline{candidate.left,candidate.right,farRight,farRight.lerp(farLeft,.5)+(candidate.normalLeft+candidate.normalRight)*.5*bend,farLeft};
				UrbanParcel::normalize(outline);if (outline.size()<3 || Polygon{outline}.area()<config.agriculture_minimumFieldArea) { break; }
				const auto result=placeField(outline,candidate.salt);count(result);
				if (result==Placement::Placed || result==Placement::Disconnected || result==Placement::Missing) { break; }
			}
		}
		// 道路沿いの列で埋まらなかった平地を、最寄りの道路と同じ向きの格子に並ぶ短冊で埋める。
		// 区画の格子は道路の起点を基準にするため、隣り合う区画は畦をはさんで揃う。
		int infill=0;const Stopwatch infillTimer{StartImmediately::Yes};
		{
			RoadPlanSnapIndex roadIndex;roadIndex.rebuild(roads);
			constexpr double kInfillStep=24;
			constexpr double kInfillSearch=400;
			const double plotLength=config.agriculture_plotLength*.8,plotWidth=config.agriculture_plotWidth*.6;
			HashSet<Point> visited;
			for (const auto& frame:frames)
			{
				const double radius=config.agriculture_maximumRadius;
				for (double z=frame.center.y-radius;z<=frame.center.y+radius;z+=kInfillStep) { for (double x=frame.center.x-radius;x<=frame.center.x+radius;x+=kInfillStep)
				{
					const Vec2 point{x,z};
					if (!visited.insert(Point{static_cast<int>(Floor(x/kInfillStep)),static_cast<int>(Floor(z/kInfillStep))}).second) { continue; }
					const uint32 salt=hash(seed,point)&~kManagedField;
					const Vec2 districtCell{Floor(x/bucketSize)*bucketSize,Floor(z/bucketSize)*bucketSize};
					const double roll=(hash(seed,districtCell)%10000)/10000.0*.72+(salt%10000)/10000.0*.28;
					if (roll>Min(1.0,density(point,frames)*kFullCultivationDensity)) { continue; }
					bool covered=false;
					if (const auto found=accepted.find(bucket(point));found!=accepted.end())
					{
						for (const auto& field:found->second) { if (field.contains(point)) { covered=true;break; } }
					}
					if (covered || !agricultural(world,point) || builtUp(world,point) || insidePlannedTown(point,frames)) { continue; }
					const auto hit=roadIndex.find(roads,groundPoint(world,point),kInfillSearch,Math::Inf,false,[](const RoadEdge& edge) { return edge.hasRoadLanes() && !edge.useElevation; });
					if (!hit.edgeId) { continue; }
					const auto* edge=roads.getEdge(*hit.edgeId);const auto curve=roads.getBezier(*hit.edgeId);if (!edge || !curve) { continue; }
					Vec2 along=horizontal(curve->tangentAt(curve->totalLength*hit.curveT));if (along.lengthSq()<1e-6) { continue; }
					along.normalize();const Vec2 across{-along.y,along.x};
					const Vec2 anchor=horizontal(roads.getNode(edge->nodeA)->position);
					const double u=Floor((point-anchor).dot(along)/plotLength)*plotLength,v=Floor((point-anchor).dot(across)/plotWidth)*plotWidth;
					// 家や道に挟まれた土地では、町なかの小さな田のように短辺・長辺とも縮めて収める。
					for (const Vec2 scale:{Vec2{1.0,1.0},Vec2{.6,1.0},Vec2{.35,1.0},Vec2{.35,.6}})
					{
						const double length=plotLength*scale.x,width=plotWidth*scale.y;
						const Vec2 cellCenter=anchor+along*(u+plotLength*.5)+across*(v+plotWidth*.5);
						const Vec2 toward=point-cellCenter;
						const Vec2 origin=cellCenter+along*Clamp(toward.dot(along),-(plotLength-length)*.5,(plotLength-length)*.5)
							+across*Clamp(toward.dot(across),-(plotWidth-width)*.5,(plotWidth-width)*.5)-along*(length*.5)-across*(width*.5);
						const Array<Vec2> outline{origin,origin+along*length,origin+along*length+across*width,origin+across*width};
						const auto result=placeField(outline,salt);
						if (result==Placement::Placed) { ++infill;break; }
						if (result!=Placement::Crowded && result!=Placement::Blocked) { break; }
					}
				} }
			}
		}

		DBG_LOG(U"[AgriculturalFields] fields={} infill={} infillMs={:.0f} sparse={} unsuitable={} crowded={} blocked={} uneven={} disconnected={}"_fmt(
			stats.fields,infill,infillTimer.msF(),sparse,unsuitable,crowded,blocked,uneven,stats.disconnected));
		return stats;
	}
}
