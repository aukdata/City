#include "AgriculturalLayout.hpp"
#include "ParcelRoadIndex.hpp"
#include "SettlementPlan.hpp"
#include "StreetProfile.hpp"
#include "UrbanParcel.hpp"
#include "RoadDesignLimits.hpp"
#include "../road/RoadPlanDraft.hpp"

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
			const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
			const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});
			return chunk && chunk->zoneMap[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}]==ZoneType::Agriculture;
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
						if (building.type==BuildingType::RuralHouse) { homes << center; }
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
		int prepareHomes(World& world,const RoadNetwork& roads,const TrainNetwork* railway,const Array<Frame>& frames)
		{
			const auto& config=GenerationSettings::get();ParcelRoadIndex occupied{roads,true};if (railway) { occupied.addRailway(*railway); }
			Sites sites{world};int placed=0;
			for (const auto& edge:roads.edges())
			{
				if (edge.id<0 || edge.useElevation || edge.tunnel || !edge.farmAccess) { continue; }
				const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
				for (float arc=curve->totalLength*.5f;arc<curve->totalLength;arc+=static_cast<float>(config.agriculture_homeSpacing))
				{
					const Vec3 road=curve->positionAt(arc),right3=tangentToRight(curve->tangentAt(arc));
					if (settlementDistance(horizontal(road),frames)>config.agriculture_maximumRadius) { continue; }
					if (sites.served({horizontal(road)},config.agriculture_homeSpacing)) { continue; }
					for (const int side:{-1,1})
					{
						const double half=buildingFootprintXZ(BuildingType::RuralHouse)*.5;
						const Vec2 outward=horizontal(right3)*side,point=horizontal(road)+outward*(edge.totalWidth()*.5+half+config.agriculture_homeSetback);
						if (!agricultural(world,point)) { continue; }
						const double angle=Atan2(-outward.x,outward.y);const auto footprint=ParcelGeometry::footprint(point,half+config.agriculture_homeFootprintMargin,angle);
						if (occupied.overlaps(footprint) || sites.occupied.overlaps(footprint)) { continue; }
						double low=Math::Inf,high=-Math::Inf;bool valid=true;
						for (const auto corner:footprint)
						{
							const auto ground=groundPoint(world,corner);low=Min(low,ground.y);high=Max(high,ground.y);
							valid &= agricultural(world,corner) && ground.y>world.waterSurfaceHeight(corner.x,corner.y)+config.agriculture_minimumFreeboard;
						}
						if (!valid || high-low>config.development_maximumBuildingRelief) { continue; }
						const Point cell{static_cast<int>(point.x/kCellSize),static_cast<int>(point.y/kCellSize)};
						auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS});auto& building=chunk->buildingGrid[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}];
						if (building.type!=BuildingType::None && building.type!=BuildingType::Farmland) { continue; }
						building.type=BuildingType::RuralHouse;building.angle=static_cast<float>(angle);building.edgeId=edge.id;building.edgeT=curve->tFromArcLength(arc);
						building.offsetX=static_cast<float>(point.x-(cell.x+.5)*kCellSize);building.offsetZ=static_cast<float>(point.y-(cell.y+.5)*kCellSize);
						chunk->zoneMap[{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}]=ZoneType::LowResidential;chunk->meshDirty=true;
						sites.occupied.add(footprint);sites.homes << point;++placed;break;
					}
				}
			}
			return placed;
		}
	}

	Stats prepare(World& world,RoadNetwork& roads,uint64 seed,const Array<Frame>& inputFrames,const TrainNetwork* railway)
	{
		Stats stats;const auto& config=GenerationSettings::get();const auto frames=localFrames(roads,inputFrames);
		Array<int> originals;for (const auto& edge:roads.edges()) { if (edge.id>=0 && !edge.useElevation && !edge.tunnel && !edge.farmAccess && edge.roadType!=RoadType::Expressway) { originals << edge.id; } }
		// Insert real junctions along long rural frontages before buildings receive edge references.
		for (int id:originals)
		{
			float arc=static_cast<float>(config.agriculture_accessSpacing);
			while (const auto curve=roads.getBezier(id))
			{
				if (arc>curve->totalLength-config.agriculture_accessSpacing*.5) { break; }
				const Vec3 point=curve->positionAt(arc);
				if (!agricultural(world,horizontal(point)) || density(horizontal(point),frames)<config.agriculture_minimumDensity) { arc+=static_cast<float>(config.agriculture_accessSpacing);continue; }
				const int end=roads.getEdge(id)->nodeB,mid=roads.splitEdgeAt(id,arc);if (mid<0) { break; }
				Optional<int> remaining;
				for (int child:roads.getNode(mid)->edgeIds()) { const auto* edge=roads.getEdge(child);if (edge && edge->nodeB==end) { remaining=child;break; } }
				if (!remaining) { break; }id=*remaining;arc=static_cast<float>(config.agriculture_accessSpacing);
			}
		}
		struct Root { int id; Vec2 direction; double reach; };
		Array<Root> roots;HashSet<Point> rootCells;
		for (const auto& node:roads.nodes())
		{
			if (node.id<0 || node.attachments.isEmpty() || node.attachments.size()>3) { continue; }
			const Vec2 point=horizontal(node.position);const double probability=density(point,frames);
			if (probability<config.agriculture_minimumDensity || (hash(seed,point)%10000)/10000.0>probability) { continue; }
			if (Abs(groundPoint(world,point).y-node.position.y)>1) { continue; }
			const Point cell{static_cast<int>(point.x/(config.agriculture_accessSpacing*config.agriculture_rootSpacingRatio)),static_cast<int>(point.y/(config.agriculture_accessSpacing*config.agriculture_rootSpacingRatio))};
			if (!rootCells.insert(cell).second) { continue; }
			const int edgeId=node.attachments.front().edgeId;const auto* edge=roads.getEdge(edgeId);const auto curve=roads.getBezier(edgeId);
			if (!edge || !curve || edge->useElevation || edge->tunnel || edge->farmAccess || edge->roadType==RoadType::Expressway) { continue; }
			const Vec2 normal=horizontal(tangentToRight(curve->tangentAt(edge->nodeA==node.id ? 0 : curve->totalLength)));
			for (int side:{-1,1})
			{
				// 町の道路沿いにある住宅帯を越え、最初の区間の先が農地なら取り付けを許可する。
				if (!agricultural(world,point+normal*side*config.agriculture_trackStep)) { continue; }
				roots << Root{node.id,normal*side,config.agriculture_trackReach*(config.agriculture_minimumReachRatio+(1-config.agriculture_minimumReachRatio)*probability)};
			}
		}
		ParcelRoadIndex occupied{roads,true};RoadNetwork empty;ParcelRoadIndex rail{empty};if (railway) { rail.addRailway(*railway); }
		Sites buildings{world};RoadPlanSnapIndex connections;connections.rebuild(roads);
		for (const auto& root:roots)
		{
			int previous=root.id;Vec2 direction=root.direction;
			for (double run=0;run<root.reach;run+=config.agriculture_trackStep)
			{
				const Vec3 start=roads.getNode(previous)->position;Optional<CubicBezier> best;Optional<RoadPlanSnapIndex::Hit> bestConnection;Vec2 bestDirection=direction;double bestCost=Math::Inf;
				for (const int turn:{0,-1,1,-2,2})
				{
					const double angle=turn*config.agriculture_trackHeadingStep;
					const Vec2 next{direction.x*Cos(angle)-direction.y*Sin(angle),direction.x*Sin(angle)+direction.y*Cos(angle)};
					if (next.dot(root.direction)<.25) { continue; }
					Vec3 end=groundPoint(world,horizontal(start)+(direction+next).normalized()*config.agriculture_trackStep);
					Optional<RoadPlanSnapIndex::Hit> connection;
					const auto hit=connections.find(roads,end,config.agriculture_connectionRadius,config.agriculture_maximumTrackCutFill,false);
					if(hit.edgeId)
					{
						const auto* target=roads.getEdge(*hit.edgeId);const Vec2 span=horizontal(hit.position-start);
						if(target && target->nodeA!=previous && target->nodeB!=previous && target->isRoadbedBuilt() && !target->tunnel && !target->useElevation && target->roadType!=RoadType::Expressway
							&& span.length()>12 && span.normalized().dot(direction)>.5)
						{ end=hit.position;connection=hit; }
					}
					if (!connection && !agricultural(world,horizontal(end))) { continue; }
					const double handle=horizontal(end-start).length()/3;
					const CubicBezier curve{start,{start.x+direction.x*handle,Math::Lerp(start.y,end.y,1.0/3),start.z+direction.y*handle},{end.x-next.x*handle,Math::Lerp(start.y,end.y,2.0/3),end.z-next.y*handle},end};
					if (curve.minimumHorizontalRadius()<RoadDesignLimits::forType(RoadType::LocalRoad).minimumRadius) { continue; }
					bool valid=true;double reliefCost=0;Vec3 last=start;
					const int samples=Max(3,static_cast<int>(Ceil(curve.totalLength/config.agriculture_sampleStep)));
					for (int i=1;i<=samples;++i)
					{
						const auto p=curve.positionAt(curve.totalLength*i/samples),ground=groundPoint(world,horizontal(p));const double distance=horizontal(p-last).length();
						const auto footprint=ParcelGeometry::footprint(horizontal(p),config.agriculture_trackWidth*.5+1,0);
						const double joiningWidth=connection ? roads.getEdge(*connection->edgeId)->totalWidth()*.5+config.agriculture_trackWidth*.5+3 : 0;
						const bool joining=connection && p.distanceFrom(end)<joiningWidth;
						valid &= (run==0 || joining || agricultural(world,horizontal(p))) && ground.y>world.waterSurfaceHeight(p.x,p.z)+config.agriculture_minimumFreeboard
							&& Abs(ground.y-last.y)<=distance*config.agriculture_maximumTrackGrade && Abs(ground.y-p.y)<config.agriculture_maximumTrackCutFill
							&& !rail.overlaps(footprint) && !buildings.occupied.overlaps(footprint);
						if (p.distanceFrom(start)>12 && !joining && occupied.overlaps(footprint)) { valid=false; }
						if (!valid) { break; }reliefCost+=Square(ground.y-last.y);last=ground;
					}
					const double cost=reliefCost+Square(angle)*config.agriculture_trackTurnPenalty+Square(1-next.dot(root.direction))*config.agriculture_trackDirectionPenalty;
					if (valid && ((!bestConnection && connection) || (static_cast<bool>(bestConnection)==static_cast<bool>(connection) && cost<bestCost))) { best=curve;bestDirection=next;bestCost=cost;bestConnection=connection; }
				}
				if (!best) { break; }
				int end;
				if(bestConnection)
				{
					const auto* target=roads.getEdge(*bestConnection->edgeId);const auto curve=roads.getBezier(target->id);
					if(bestConnection->curveT==0) { end=target->nodeA; }
					else if(bestConnection->curveT==1) { end=target->nodeB; }
					else
					{
						const float sample=bestConnection->curveT*CubicBezier::SAMPLES;const int index=Min(CubicBezier::SAMPLES-1,static_cast<int>(sample));
						end=roads.splitEdgeAt(target->id,Math::Lerp(curve->arcTable[index],curve->arcTable[index+1],sample-index));
						if(end<0) { break; }
						for(int child:roads.getNode(end)->edgeIds()) { connections.appendEdge(roads,child); }
					}
				}
				else { end=roads.addNode(best->p3,NodeType::Joint); }
				const auto edgeId=roads.addEdge(previous,end,best->p1,best->p2,RoadType::LocalRoad,2);
				if (!edgeId) { if(!bestConnection) { roads.removeNode(end); }break; }
				auto* edge=roads.getEdge(*edgeId);auto profile=GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess);profile.laneWidth=static_cast<float>(config.agriculture_trackWidth/profile.lanes);
				GeneratedStreet::apply(*edge,profile);edge->edgeState=EdgeState::Existing;edge->farmAccess=true;edge->parts.remove_if([](const RoadPart& part){return part.type==RoadPartType::UtilityPole;});
				for (int i=1;i<=8;++i) { occupied.add(ribbon(horizontal(best->evaluate((i-1)/8.0f)),horizontal(best->evaluate(i/8.0f)),edge->totalWidth()*.5+1)); }
				roads.rebuildNodeConnectivity(previous,end);connections.appendEdge(roads,*edgeId);
				previous=end;direction=bestDirection;++stats.tracks;
				if(bestConnection) { ++stats.connections;break; }
			}
		}
		stats.homes=prepareHomes(world,roads,railway,frames);stats.drains=stats.tracks*2;return stats;
	}

	Stats generate(World& world,RoadNetwork& roads,uint64 seed,const Array<Frame>& inputFrames,bool prepareAccess,const TrainNetwork* railway)
	{
		Stats stats=prepareAccess ? prepare(world,roads,seed,inputFrames,railway) : Stats{};
		const auto& config=GenerationSettings::get();const auto frames=localFrames(roads,inputFrames);
		ParcelRoadIndex occupied{roads,true};if (railway) { occupied.addRailway(*railway); }Sites sites{world};
		struct Candidate { Vec2 center; Array<Vec2> polygon; uint32 salt; };
		Array<Candidate> candidates;HashTable<Point,Array<size_t>> buckets;
		const double bucketSize=config.agriculture_plotLength*2;
		const auto bucket=[&](Vec2 p){return Point{static_cast<int>(Floor(p.x/bucketSize)),static_cast<int>(Floor(p.y/bucketSize))};};
		for (const auto& edge:roads.edges())
		{
			if (edge.id<0 || !edge.farmAccess) { continue; }
			if (!prepareAccess) { ++stats.tracks;stats.drains+=2; }
			const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
			const int count=Max(1,static_cast<int>(Round(curve->totalLength/(config.agriculture_plotLength*.5))));
			for (int section=0;section<count;++section) { for (int side:{-1,1})
			{
				const float begin=curve->totalLength*section/count,end=curve->totalLength*(section+1)/count;
				const Vec3 a=curve->positionAt(begin),b=curve->positionAt(end),n1=tangentToRight(curve->tangentAt(begin))*side,n2=tangentToRight(curve->tangentAt(end))*side;
				const Vec2 center=horizontal((a+b)*.5+(n1+n2)*config.agriculture_plotWidth*.5);
				const uint32 salt=hash(seed,center)&~kManagedField;
				++stats.candidates;if (salt%10000/10000.0>density(center,frames) || !agricultural(world,center)) { continue; }
				const double near=edge.totalWidth()*.5+config.agriculture_bundWidth+config.agriculture_drainWidth;
				const double depth=config.agriculture_plotWidth*2*(1+config.agriculture_fieldShapeVariation*(static_cast<double>((salt>>8)%100)/50-1));
				const Vec2 left=horizontal(a+n1*near),right=horizontal(b+n2*near),farLeft=horizontal(a+n1*(near+depth)),farRight=horizontal(b+n2*(near+depth*(.7+(salt%61)/100.0)));
				Array<Vec2> polygon{left,right,farRight,farRight.lerp(farLeft,.5)+horizontal((n1+n2)*.5)*depth*.12,farLeft};
				UrbanParcel::normalize(polygon);const size_t index=candidates.size();candidates << Candidate{center,std::move(polygon),salt};buckets[bucket(center)] << index;
			} }
		}
		for (size_t index=0;index<candidates.size();++index)
		{
			const auto& candidate=candidates[index];auto polygon=candidate.polygon;const Point cell=bucket(candidate.center);
			// A shared Voronoi boundary fills road gaps without overlapping independent plots.
			for (int dz=-1;dz<=1;++dz) { for (int dx=-1;dx<=1;++dx)
			{
				if (const auto found=buckets.find(cell+Point{dx,dz});found!=buckets.end())
				{
					for (const size_t other:found->second) { if (other!=index) { polygon=UrbanParcel::clipCloserTo(std::move(polygon),candidate.center,candidates[other].center); } }
				}
			} }
			UrbanParcel::normalize(polygon);if (polygon.size()<3 || Polygon{polygon}.area()<config.agriculture_minimumFieldArea) { continue; }
			bool valid=true;double low=Math::Inf,high=-Math::Inf;const Vec2 center=Polygon{polygon}.centroid();
			for (const Vec2 vertex:polygon)
			{
				const int samples=Max(1,static_cast<int>(Ceil(vertex.distanceFrom(center)/config.agriculture_sampleStep)));
				for (int i=0;i<=samples;++i)
				{
					const Vec2 point=center.lerp(vertex,static_cast<double>(i)/samples);const auto ground=groundPoint(world,point);low=Min(low,ground.y);high=Max(high,ground.y);
					valid &= agricultural(world,point) && ground.y>world.waterSurfaceHeight(point.x,point.y)+config.agriculture_minimumFreeboard;
				}
			}
			for (size_t i=1;i+1<polygon.size();++i)
			{
				const ParcelGeometry::Quad triangle{polygon[0],polygon[i],polygon[i+1],polygon[i+1]};
				valid &= !occupied.overlaps(triangle) && !sites.occupied.overlaps(triangle);
			}
			if (!valid || high-low>config.agriculture_maximumFieldRelief) { continue; }
			if (!sites.served(polygon,config.agriculture_homeMaximumDistance)) { ++stats.disconnected;continue; }
			const Point coord{static_cast<int>(center.x/CHUNK_SIZE),static_cast<int>(center.y/CHUNK_SIZE)};auto* chunk=world.getChunk(coord);if (!chunk) { continue; }
			LandPatch patch;patch.id=static_cast<int>(chunk->landPatches.size());patch.polygon=std::move(polygon);patch.elevationOffset=config.agriculture_surfaceLift;
			const bool paddy=high-low<config.agriculture_paddyRelief && candidate.salt%config.agriculture_dryFieldDivisor!=0;
			patch.type=paddy ? LandPatchType::PaddyField : LandPatchType::FarmField;patch.materialVariant=candidate.salt|kManagedField;
			chunk->landPatches << std::move(patch);chunk->meshDirty=true;++stats.fields;stats.paddies+=paddy;
		}
		return stats;
	}
}
