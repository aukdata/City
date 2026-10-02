#include "ZoneManager.hpp"
#include "../world/ZoneGrid.hpp"
#include "../gen/ParcelGeometry.hpp"
#include "../gen/UrbanParcel.hpp"
#include "../road/RoadGeometry.hpp"

namespace
{
	bool canDevelop(ZoneType zone)
	{
		return zone >= ZoneType::LowResidential && zone <= ZoneType::Agriculture;
	}
	uint32 plotHash(Point chunk, Point cell)
	{
		return static_cast<uint32>(chunk.x*ZONE_CELLS+cell.x)*73856093u
			^ static_cast<uint32>(chunk.y*ZONE_CELLS+cell.y)*19349663u;
	}
	BuildingType initialBuilding(ZoneType zone, uint32 hash)
	{
		switch (zone)
		{
		case ZoneType::LowResidential: return BuildingType::Detached;
		case ZoneType::Residential: return hash%3 == 0 ? BuildingType::Detached : BuildingType::LowApartment;
		case ZoneType::Commercial: return hash%5 == 0 ? BuildingType::Office : BuildingType::Shop;
		case ZoneType::Industrial: return BuildingType::Factory;
		case ZoneType::Agriculture: return BuildingType::Farmland;
		default: return BuildingType::None;
		}
	}
	struct NearbyRoad
	{
		const RoadEdge* edge = nullptr;
		CubicBezier curve;
		float arc = 0, parameter = 0;
		Vec3 position, right;
		double distance = 0;
		RoadGeometry::LateralRange range;
	};
	Array<NearbyRoad> roadsNear(const RoadNetwork& network, Vec2 center)
	{
		Array<NearbyRoad> result;
		constexpr double kRadius = 48;
		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) { continue; }
			const auto* a = network.getNode(edge.nodeA);
			const auto* b = network.getNode(edge.nodeB);
			if (!a || !b) { continue; }
			double minX = a->position.x, maxX = minX, minZ = a->position.z, maxZ = minZ;
			for (const Vec3 p : {edge.ctrlA, edge.ctrlB, b->position})
			{
				minX=Min(minX,p.x); maxX=Max(maxX,p.x); minZ=Min(minZ,p.z); maxZ=Max(maxZ,p.z);
			}
			if (center.x < minX-kRadius || center.x > maxX+kRadius
				|| center.y < minZ-kRadius || center.y > maxZ+kRadius) { continue; }
			const auto curve = network.getBezier(edge.id);
			if (!curve || curve->totalLength < 1) { continue; }
			const auto distanceAt = [&](float arc)
			{
				const Vec3 position = curve->positionAt(arc);
				return center.distanceFromSq(Vec2{position.x,position.z});
			};
			int best = 0;
			for (int i=1; i<=32; ++i)
			{
				if (distanceAt(curve->totalLength*i/32) < distanceAt(curve->totalLength*best/32)) { best=i; }
			}
			float low=curve->totalLength*Max(0,best-1)/32, high=curve->totalLength*Min(32,best+1)/32;
			for (int i=0; i<18; ++i)
			{
				const float first=low+(high-low)/3, second=high-(high-low)/3;
				if (distanceAt(first)<distanceAt(second)) { high=second; } else { low=first; }
			}
			NearbyRoad road;
			road.edge=&edge; road.curve=*curve; road.arc=(low+high)*.5f;
			road.parameter=curve->tFromArcLength(road.arc); road.position=curve->evaluate(road.parameter);
			road.right=tangentToRight(curve->tangent(road.parameter));
			road.distance=Sqrt(distanceAt(road.arc)); road.range=RoadGeometry::structuralRangeAt(edge,road.parameter);
			if (road.distance<kRadius && road.range.valid) { result << std::move(road); }
		}
		result.sort_by([](const NearbyRoad& a,const NearbyRoad& b)
		{
			const auto frontage=[](const NearbyRoad& road)
			{
				const double outer=Max(Abs(road.range.left),Abs(road.range.right));
				const double arterialBonus=road.edge->roadType==RoadType::Arterial || road.edge->totalWidth()>=18.0f ? 4.0 : 0.0;
				return Max(0.0,road.distance-outer)-arterialBonus;
			};
			return frontage(a)<frontage(b);
		});
		return result;
	}
	bool overlapsBuildings(const World& world, Point coord, Point cell, const ParcelGeometry::Quad& footprint)
	{
		const int gx=coord.x*ZONE_CELLS+cell.x, gz=coord.y*ZONE_CELLS+cell.y;
		for (int z=Max(0,gz-5); z<=gz+5; ++z) for (int x=Max(0,gx-5); x<=gx+5; ++x)
		{
			const Point otherCoord{x/ZONE_CELLS,z/ZONE_CELLS};
			const Chunk* chunk=world.getChunk(otherCoord); if (!chunk) { continue; }
			const auto& building=chunk->buildingGrid[{x%ZONE_CELLS,z%ZONE_CELLS}];
			if (building.type==BuildingType::None) { continue; }
			const Vec2 center=ZoneGrid::cellCenterXZ(otherCoord,x%ZONE_CELLS,z%ZONE_CELLS)+Vec2{building.offsetX,building.offsetZ};
			if (ParcelGeometry::overlaps(footprint,ParcelGeometry::footprint(center,buildingFootprintXZ(building.type)*.5+.35,building.angle))) { return true; }
		}
		return false;
	}
	void trimNeighborParcels(World& world, Point coord, Point cell, Vec2 center, const Building& building)
	{
		const int gx=coord.x*ZONE_CELLS+cell.x, gz=coord.y*ZONE_CELLS+cell.y;
		for (int z=Max(0,gz-5); z<=gz+5; ++z) for (int x=Max(0,gx-5); x<=gx+5; ++x)
		{
			if (x==gx && z==gz) { continue; }
			const Point neighborCoord{x/ZONE_CELLS,z/ZONE_CELLS},neighborCell{x%ZONE_CELLS,z%ZONE_CELLS};
			auto* chunk=world.getChunk(neighborCoord); if (!chunk) { continue; }
			const auto& neighbor=chunk->buildingGrid[neighborCell];
			if (neighbor.type==BuildingType::None || neighbor.type==BuildingType::Farmland) { continue; }
			const Vec2 neighborCenter=ZoneGrid::cellCenterXZ(neighborCoord,neighborCell.x,neighborCell.y)+Vec2{neighbor.offsetX,neighbor.offsetZ};
			const int64 key=ZoneGrid::zoneCellKey(neighborCoord,neighborCell.x,neighborCell.y);
			for (auto& parcel : chunk->landPatches)
			{
				if (parcel.sourceParcelKey!=key) { continue; }
				auto clipped=UrbanParcel::clipBetweenSites(parcel.polygon,neighborCenter,neighbor,center,building);
				UrbanParcel::normalize(clipped);
				if (clipped!=parcel.polygon)
				{
					parcel.polygon=std::move(clipped); chunk->meshDirty=true;
				}
			}
		}
	}
	bool overlapsRoads(const ParcelGeometry::Quad& footprint, const Array<NearbyRoad>& roads)
	{
		// Reserve the actual road strip, including bends, junction approaches and future road construction.
		for (const auto& road : roads)
		{
			const float begin=Max(0.0f,road.arc-24), end=Min(road.curve.totalLength,road.arc+24);
			for (float arc=begin; arc<end; arc+=2)
			{
				const float next=Min(end,arc+2);
				const float ta=road.curve.tFromArcLength(arc), tb=road.curve.tFromArcLength(next);
				const Vec3 a=road.curve.evaluate(ta), b=road.curve.evaluate(tb);
				const Vec3 ra=tangentToRight(road.curve.tangent(ta)), rb=tangentToRight(road.curve.tangent(tb));
				const auto wa=RoadGeometry::structuralRangeAt(*road.edge,ta), wb=RoadGeometry::structuralRangeAt(*road.edge,tb);
				const Vec3 p0=a+ra*(wa.left-.5), p1=a+ra*(wa.right+.5), p2=b+rb*(wb.right+.5), p3=b+rb*(wb.left-.5);
				if (ParcelGeometry::overlaps(footprint,{{p0.xz(),p1.xz(),p2.xz(),p3.xz()}})) { return true; }
			}
		}
		return false;
	}
	struct Site
	{
		ZoneDevelopmentState state=ZoneDevelopmentState::NeedsRoad;
		Building building;
		Array<Vec2> parcel;
	};
	Site inspectSite(const World& world, const RoadNetwork& network, Point coord, Point cell, ZoneType zone, const ParcelRoadIndex* railway)
	{
		Site site;
		const Vec2 cellCenter=ZoneGrid::cellCenterXZ(coord,cell.x,cell.y);
		const auto roads=roadsNear(network,cellCenter);
		const uint32 hash=plotHash(coord,cell);
		site.building.type=initialBuilding(zone,hash);
		const double half=buildingFootprintXZ(site.building.type)*.5;
		for (const auto& road : roads)
		{
			if ((road.edge->edgeState!=EdgeState::Open && road.edge->edgeState!=EdgeState::Existing)
				|| road.edge->tunnel || road.edge->useElevation
				|| road.edge->roadType==RoadType::Expressway || road.edge->roadType==RoadType::Highway) { continue; }
			const double lateral=(cellCenter-road.position.xz()).dot(road.right.xz());
			const Vec2 outward=road.right.xz()*(lateral>=0 ? 1 : -1);
			const double outer=lateral>=0 ? road.range.right : -road.range.left;
			if (road.distance>outer+half+10) { continue; }
			site.state=ZoneDevelopmentState::NeedsSpace;
			// Keep the building in its painted cell. A small setback aligns doors and gardens to the road.
			if (Abs(lateral)<outer+2 || road.arc<half+3 || road.arc>road.curve.totalLength-half-3) { continue; }
			const double target=outer+half+2.2;
			const Vec2 center=cellCenter+outward*Clamp(target-Abs(lateral),-5.0,5.0);
			const double angle=Atan2(-outward.x,outward.y);
			const auto footprint=ParcelGeometry::footprint(center,half+.25,angle);
			if (overlapsBuildings(world,coord,cell,footprint) || overlapsRoads(footprint,roads)
				|| (railway && railway->overlaps(footprint))) { continue; }
			site.state=ZoneDevelopmentState::UnsuitableTerrain;
			double lowest=Math::Inf, highest=-Math::Inf;
			bool dry=true;
			Array<Vec2> samples{center};
			for (size_t i=0;i<footprint.size();++i) { samples << footprint[i] << (footprint[i]+footprint[(i+1)%4])*.5; }
			for (const Vec2 sample : samples)
			{
				const double height=world.sampleHeight(static_cast<float>(sample.x),static_cast<float>(sample.y));
				lowest=Min(lowest,height); highest=Max(highest,height);
				dry=dry && sample.x>=0 && sample.y>=0 && sample.x<WORLD_SIZE && sample.y<WORLD_SIZE
					&& height>world.waterSurfaceHeight(sample.x,sample.y)+.3;
			}
			const double roadTerrain=world.sampleHeight(static_cast<float>(road.position.x),static_cast<float>(road.position.z));
			const double roadHeight=RoadGeometry::surfaceY(*road.edge,road.position,roadTerrain);
			if (!dry || highest-lowest>1.2 || Abs((highest+lowest)*.5-roadHeight)>1.5) { continue; }
			site.state=ZoneDevelopmentState::Developing;
			site.building.angle=static_cast<float>(angle); site.building.edgeId=road.edge->id;
			site.building.edgeT=road.parameter;
			site.building.offsetX=static_cast<float>(center.x-cellCenter.x); site.building.offsetZ=static_cast<float>(center.y-cellCenter.y);
			const Vec2 along{-outward.y,outward.x};
			const double back=(center-road.position.xz()).dot(outward)+half+1;
			const Vec2 frontCenter=road.position.xz()+outward*(outer+.06);
			const Vec2 backCenter=road.position.xz()+outward*back;
			site.parcel={frontCenter-along*(half+.6),frontCenter+along*(half+.6),backCenter+along*(half+.6),backCenter-along*(half+.6)};
			return site;
		}
		return site;
	}
}

void ZoneManager::paintCell(Chunk& chunk, Point cell, ZoneType zone)
{
	chunk.zoneMap[cell]=zone;
	m_editedCells.insert(Point{chunk.coord.x*ZONE_CELLS+cell.x,chunk.coord.y*ZONE_CELLS+cell.y});
	const int64 key=ZoneGrid::zoneCellKey(chunk.coord,cell.x,cell.y);
	const auto found=m_developmentIndex.find(key);
	if (found!=m_developmentIndex.end())
	{
		if (m_development[found->second].zone==zone && canDevelop(zone)
			&& chunk.buildingGrid[cell].type==BuildingType::None) { return; }
		removeDevelopment(found->second);
	}
	if (!canDevelop(zone) || chunk.buildingGrid[cell].type!=BuildingType::None) { return; }
	m_developmentIndex[key]=m_development.size();
	DevelopmentPlot plot; plot.chunk=chunk.coord; plot.cell=cell; plot.zone=zone; plot.checkedAt=m_developmentTime;
	m_development << plot;
}
void ZoneManager::removeDevelopment(size_t index)
{
	const auto& plot=m_development[index];
	m_developmentIndex.erase(ZoneGrid::zoneCellKey(plot.chunk,plot.cell.x,plot.cell.y));
	if (index+1<m_development.size())
	{
		m_development[index]=m_development.back();
		const auto& moved=m_development[index];
		m_developmentIndex[ZoneGrid::zoneCellKey(moved.chunk,moved.cell.x,moved.cell.y)]=index;
	}
	m_development.pop_back();
}
ZoneDevelopmentResult ZoneManager::updateDevelopment(World& world, const RoadNetwork& network,
	const ZoneDevelopmentDemand& demand, double simulationSeconds, GameTime gameNow, const TrainNetwork* railway)
{
	ZoneDevelopmentResult result;
	if (simulationSeconds<=0) { return result; }
	if (railway && (!m_railwayCorridor || m_railwayEdgeCount!=railway->edges().size()))
	{
		m_railwayCorridor.emplace(RoadNetwork{});
		m_railwayCorridor->addRailway(*railway);
		m_railwayEdgeCount=railway->edges().size();
	}
	m_developmentTime+=simulationSeconds;
	const size_t budget=Min(size_t{16},m_development.size());
	for (size_t i=0; i<budget && !m_development.isEmpty(); ++i)
	{
		m_developmentCursor%=m_development.size();
		const size_t index=m_developmentCursor++;
		auto& plot=m_development[index];
		if (plot.nextCheck>m_developmentTime) { continue; }
		auto* chunk=world.getChunk(plot.chunk);
		if (!chunk || chunk->zoneMap[plot.cell]!=plot.zone || chunk->buildingGrid[plot.cell].type!=BuildingType::None)
		{
			removeDevelopment(index); continue;
		}
		const double elapsed=m_developmentTime-plot.checkedAt;
		plot.checkedAt=m_developmentTime; plot.nextCheck=m_developmentTime+1;
		const Site site=inspectSite(world,network,plot.chunk,plot.cell,plot.zone,railway && m_railwayCorridor ? &*m_railwayCorridor : nullptr);
		plot.state=site.state;
		if (site.state!=ZoneDevelopmentState::Developing) { continue; }
		const double zoneDemand=(plot.zone==ZoneType::LowResidential || plot.zone==ZoneType::Residential) ? demand.residential
			: plot.zone==ZoneType::Commercial ? demand.commercial : plot.zone==ZoneType::Industrial ? demand.industrial : .5;
		const double duration=10+20*(1-Clamp(zoneDemand,0.0,1.0))+plotHash(plot.chunk,plot.cell)%5;
		plot.progress=Min(1.0,plot.progress+elapsed/duration);
		if (plot.progress<1 || result.buildings>=2) { continue; }
		Building building=site.building; building.builtAt=gameNow;
		chunk->buildingGrid[plot.cell]=building;
		LandPatch patch; patch.id=static_cast<int>(chunk->landPatches.size());
		patch.sourceParcelKey=ZoneGrid::zoneCellKey(plot.chunk,plot.cell.x,plot.cell.y);
		patch.type=plot.zone==ZoneType::Agriculture ? LandPatchType::FarmField : isResidentialBuildingType(building.type) ? LandPatchType::GardenSoil : LandPatchType::ParcelGravel;
		patch.elevationOffset=.012f; patch.materialVariant=plotHash(plot.chunk,plot.cell);
		const Vec2 center=ZoneGrid::cellCenterXZ(plot.chunk,plot.cell.x,plot.cell.y)+Vec2{building.offsetX,building.offsetZ};
		patch.polygon=UrbanParcel::partition(world,plot.chunk,plot.cell.x,plot.cell.y,center,site.parcel);
		trimNeighborParcels(world,plot.chunk,plot.cell,center,building);
		if (patch.polygon.size()>=3) { chunk->landPatches << std::move(patch); }
		chunk->meshDirty=true;
		++result.buildings; result.housing+=buildingCapacity(building.type);
		if (isResidentialBuildingType(building.type)) { ++result.residential; }
		if (plot.zone==ZoneType::Commercial) { ++result.commercial; }
		if (plot.zone==ZoneType::Industrial) { ++result.industrial; }
		++m_completed;
		removeDevelopment(index);
	}
	return result;
}
ZoneDevelopmentSummary ZoneManager::developmentSummary() const
{
	ZoneDevelopmentSummary summary; summary.completed=m_completed;
	for (const auto& plot : m_development)
	{
		switch (plot.state)
		{
		case ZoneDevelopmentState::Checking: ++summary.checking; break;
		case ZoneDevelopmentState::Developing: ++summary.developing; break;
		case ZoneDevelopmentState::NeedsRoad: ++summary.needsRoad; break;
		case ZoneDevelopmentState::NeedsSpace: ++summary.needsSpace; break;
		case ZoneDevelopmentState::UnsuitableTerrain: ++summary.unsuitableTerrain; break;
		}
	}
	return summary;
}
JSON ZoneManager::developmentSnapshot() const
{
	JSON result; result[U"count"]=m_development.size(); result[U"completed"]=m_completed;
	result[U"time"]=m_developmentTime; result[U"cursor"]=m_developmentCursor;
	for (size_t i=0;i<m_development.size();++i)
	{
		const auto& plot=m_development[i]; JSON item;
		item[U"x"]=plot.chunk.x*ZONE_CELLS+plot.cell.x; item[U"z"]=plot.chunk.y*ZONE_CELLS+plot.cell.y;
		item[U"zone"]=static_cast<int>(plot.zone); item[U"progress"]=plot.progress;
		item[U"state"]=static_cast<int>(plot.state);
		item[U"checkedAt"]=plot.checkedAt; item[U"nextCheck"]=plot.nextCheck; result[U"plots"][i]=item;
	}
	return result;
}
void ZoneManager::restoreDevelopment(const JSON& snapshot, const World& world)
{
	m_development.clear(); m_developmentIndex.clear(); m_developmentCursor=0; m_developmentTime=0; m_completed=0;
	if (!snapshot || !snapshot.contains(U"count")) { return; }
	m_completed=Max(0,snapshot[U"completed"].getOr<int>(0));
	const double time=snapshot[U"time"].getOr<double>(0);
	m_developmentTime=IsFinite(time) ? Max(0.0,time) : 0;
	const int count=snapshot[U"count"].getOr<int>(0);
	if (count<=0 || !snapshot.contains(U"plots")) { return; }
	for (int i=0;i<Min(count,static_cast<int>(snapshot[U"plots"].size()));++i)
	{
		const auto item=snapshot[U"plots"][i];
		const int x=item[U"x"].getOr<int>(-1), z=item[U"z"].getOr<int>(-1);
		if (x<0 || z<0 || x>=WORLD_CHUNKS*ZONE_CELLS || z>=WORLD_CHUNKS*ZONE_CELLS) { continue; }
		DevelopmentPlot plot; plot.chunk={x/ZONE_CELLS,z/ZONE_CELLS}; plot.cell={x%ZONE_CELLS,z%ZONE_CELLS};
		plot.zone=static_cast<ZoneType>(item[U"zone"].getOr<int>(0));
		const auto* chunk=world.getChunk(plot.chunk);
		if (!canDevelop(plot.zone) || !chunk || chunk->zoneMap[plot.cell]!=plot.zone || chunk->buildingGrid[plot.cell].type!=BuildingType::None) { continue; }
		const int64 key=ZoneGrid::zoneCellKey(plot.chunk,plot.cell.x,plot.cell.y);
		if (m_developmentIndex.contains(key)) { continue; }
		plot.progress=Clamp(item[U"progress"].getOr<double>(0),0.0,1.0);
		plot.state=static_cast<ZoneDevelopmentState>(Clamp(item[U"state"].getOr<int>(0),0,4));
		const double checkedAt=item[U"checkedAt"].getOr<double>(0), nextCheck=item[U"nextCheck"].getOr<double>(0);
		plot.checkedAt=IsFinite(checkedAt) ? Clamp(checkedAt,0.0,m_developmentTime) : 0;
		plot.nextCheck=IsFinite(nextCheck) ? Max(0.0,nextCheck) : 0;
		m_developmentIndex[key]=m_development.size(); m_development << plot;
	}
	if (!m_development.isEmpty())
	{
		m_developmentCursor=snapshot[U"cursor"].getOr<size_t>(0)%m_development.size();
	}
}

JSON ZoneManager::saveState(const World& world) const
{
	JSON result=developmentSnapshot();
	Array<Point> cells(m_editedCells.begin(),m_editedCells.end());
	cells.sort_by([](Point a,Point b) { return a.y!=b.y ? a.y<b.y : a.x<b.x; });
	int index=0;
	for (Point cell : cells)
	{
		const auto* chunk=world.getChunk({cell.x/ZONE_CELLS,cell.y/ZONE_CELLS}); if (!chunk) { continue; }
		const Point local{cell.x%ZONE_CELLS,cell.y%ZONE_CELLS}; const auto& building=chunk->buildingGrid[local];
		JSON item; item[U"x"]=cell.x; item[U"z"]=cell.y; item[U"zone"]=static_cast<int>(chunk->zoneMap[local]);
		item[U"building"]=static_cast<int>(building.type); item[U"builtAt"]=building.builtAt;
		item[U"angle"]=building.angle; item[U"edge"]=building.edgeId; item[U"t"]=building.edgeT;
		item[U"offsetX"]=building.offsetX; item[U"offsetZ"]=building.offsetZ;
		result[U"edited"][index++]=item;
	}
	return result;
}
void ZoneManager::restoreState(const JSON& snapshot, World& world, bool preserveWorld)
{
	m_editedCells.clear();
	if (snapshot && snapshot.contains(U"edited"))
	{
		for (const auto& item : snapshot[U"edited"].arrayView())
		{
			const int x=item[U"x"].getOr<int>(-1), z=item[U"z"].getOr<int>(-1);
			if (x<0 || z<0 || x>=WORLD_CHUNKS*ZONE_CELLS || z>=WORLD_CHUNKS*ZONE_CELLS) { continue; }
			const Point coord{x/ZONE_CELLS,z/ZONE_CELLS},cell{x%ZONE_CELLS,z%ZONE_CELLS};
			auto* chunk=world.getChunk(coord); if (!chunk) { continue; }
			const int zone=item[U"zone"].getOr<int>(0), type=item[U"building"].getOr<int>(0);
			if (!InRange(zone,0,6) || type<0 || type>=static_cast<int>(BuildingType::Count)) { continue; }
			if (preserveWorld)
			{
				m_editedCells.insert({x,z});
				continue;
			}
			chunk->zoneMap[cell]=static_cast<ZoneType>(zone);
			Building building; building.type=static_cast<BuildingType>(type);
			building.builtAt=item[U"builtAt"].getOr<double>(0); building.angle=item[U"angle"].getOr<float>(0);
			building.edgeId=item[U"edge"].getOr<int>(-1); building.edgeT=item[U"t"].getOr<float>(0);
			building.offsetX=item[U"offsetX"].getOr<float>(0); building.offsetZ=item[U"offsetZ"].getOr<float>(0);
			chunk->buildingGrid[cell]=building; chunk->meshDirty=true;
			if (building.type==BuildingType::None)
			{
				const int64 key=ZoneGrid::zoneCellKey(coord,cell.x,cell.y);
				chunk->landPatches.remove_if([&](const LandPatch& patch) { return patch.sourceParcelKey==key; });
			}
			m_editedCells.insert({x,z});
		}
	}
	restoreDevelopment(snapshot,world);
}
