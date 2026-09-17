#include "RailDepotBuilder.hpp"
#include "../gen/ParcelRoadIndex.hpp"
#include "../debug/DebugLog.hpp"
#include "../gen/TransportClearance.hpp"

namespace RailDepotBuilder
{
	static bool addAt(TrainNetwork& network, const World& world, int station, double lead, double offset,
		const ParcelRoadIndex& roadSpace, const ParcelRoadIndex& railSpace, const TransportClearance& crossings, String& error)
	{
		error.clear();
		const auto* node = network.getNode(station);
		if (!node || node->type != TrackNodeType::Station) { error = U"車庫を接続する駅を選んでください"; return false; }
		for (const auto& depot : network.depots())
		{
			if (depot.stationNodeId == station) { error = U"この駅には車庫があります"; return false; }
		}
		if (node->edgeIds.size() != 1) { error = U"車庫は線路が1本の終端駅に接続します"; return false; }
		if (RailwaySite::stationMinimumRadius(network,station)<RailwaySite::kStationMinimumRadius) { error=U"駅のホーム区間は曲線半径500m以上が必要です"; return false; }
		const auto base = RailwaySite::stationFrame(network, station);
		if (!base) { error = U"駅から出る線路が必要です"; return false; }
		const RailwaySite::Frame yard{base->point(offset,0,lead), base->along, base->right};
		const auto footprint = RailwaySite::rectangle(yard,-5,13,-4,140);

		if (roadSpace.overlaps(footprint) || railSpace.overlaps(footprint)) { error = U"駅の横に車庫用の空地がありません（道路・線路）"; return false; }
		for (int z = -4; z <= 140; z += 8)
		{
			for (int x = -5; x <= 13; x += 3)
			{
				const Vec3 point = yard.point(x,0,z);
				if (point.x < 16 || point.z < 16 || point.x >= WORLD_SIZE-16 || point.z >= WORLD_SIZE-16) { error = U"車庫の敷地がマップの外です"; return false; }
				const float ground = world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
				if (ground > point.y+.3 || point.y-ground > 22 || ground < world.waterSurfaceHeight(point.x,point.z)+1)
				{
					error = U"駅の横の地形が車庫に適していません"; return false;
				}
			}
		}
		// 引込線の途中も道路と干渉しないことを確かめる。
		const CubicBezier approach{base->origin,base->point(0,0,lead*.45),yard.point(0,0,-lead*.45),yard.origin};
		if (approach.minimumHorizontalRadius()<RailwaySite::kDepotApproachMinimumRadius)
		{
			error=U"引込線の曲線半径150mを確保できる、駅から離れた敷地が必要です";return false;
		}
		for (const double side : {0.0,7.0})
		{
			const CubicBezier siding{yard.origin,yard.point(0,0,40),yard.point(side,0,75),yard.point(side,0,130)};
			if (siding.minimumHorizontalRadius()<RailwaySite::kSidingMinimumRadius) { error=U"留置線の曲線半径300mを確保できません";return false; }
		}
		for (float arc = 8; arc < approach.totalLength; arc += 4)
		{
			const Vec3 point = approach.positionAt(arc);
			const double ground = world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
			if (ground>point.y+.3 || point.y-ground>22 || ground<world.waterSurfaceHeight(point.x,point.z)+1)
			{
				error=U"引込線の地形が車庫に適していません"; return false;
			}
			if (roadSpace.overlaps(ParcelGeometry::footprint({point.x,point.z},3,0)) && point.y<crossings.minimumRailHeight({point.x,point.z},3))
			{
				error = U"引込線と道路の高さが近すぎるため設置できません"; return false;
			}
		}
		// 既存の家を壊さず、隣接区画まで含めて建物との交差を確認する。
		const Vec3 middle = yard.point(4,0,68);
		for (int gz = Max(0,static_cast<int>((Min(middle.z,base->origin.z)-100)/16)); gz <= Min(WORLD_CHUNKS*ZONE_CELLS-1,static_cast<int>((Max(middle.z,base->origin.z)+100)/16)); ++gz)
		{
			for (int gx = Max(0,static_cast<int>((Min(middle.x,base->origin.x)-100)/16)); gx <= Min(WORLD_CHUNKS*ZONE_CELLS-1,static_cast<int>((Max(middle.x,base->origin.x)+100)/16)); ++gx)
			{
				const auto* chunk = world.getChunk({gx/ZONE_CELLS,gz/ZONE_CELLS}); if (!chunk) { continue; }
				const auto& building = chunk->buildingGrid[{gx%ZONE_CELLS,gz%ZONE_CELLS}];
				if (building.type == BuildingType::None) { continue; }
				const auto site = ParcelGeometry::footprint({(gx+.5)*16+building.offsetX,(gz+.5)*16+building.offsetZ},buildingFootprintXZ(building.type)*.5,building.angle);
				bool occupied = ParcelGeometry::overlaps(site,footprint);
				for (float arc=8; !occupied && arc<approach.totalLength; arc+=4)
				{
					const Vec3 point=approach.positionAt(arc);
					occupied=ParcelGeometry::overlaps(site,ParcelGeometry::footprint({point.x,point.z},3,0));
				}
				if (occupied) { error = U"車庫・引込線の敷地に既存の建物があります"; return false; }
			}
		}
		const String name = node->name + U"車両基地";
		RailDepot depot; depot.stationNodeId = station; depot.name = name;
		depot.throatNodeId = network.addNode(yard.origin);
		int id = network.addEdge(station,depot.throatNodeId,approach.p1,approach.p2,25); network.getEdge(id)->depotTrack = true;
		network.infrastructure().updateEdgeElevation(id,world);
		network.infrastructure().generatePiersForEdge(id,world);
		for (const double side : {0.0,7.0})
		{
			const int end = network.addNode(yard.point(side,0,130),TrackNodeType::Buffer);
			id = network.addEdge(depot.throatNodeId,end,yard.point(0,0,40),yard.point(side,0,75),15);
			network.getEdge(id)->depotTrack = true;
			network.infrastructure().updateEdgeElevation(id,world);
			network.infrastructure().generatePiersForEdge(id,world); depot.sidingNodes << end;
		}
		network.depots() << std::move(depot);
		DBG_LOG(U"[RailDepot] station={} name={} sidings=2 lead={} offset={}"_fmt(station,name,lead,offset));
		return true;
	}
	bool add(TrainNetwork& network, const World& world, const RoadNetwork& roads, int station, String& error)
	{
		const ParcelRoadIndex roadSpace{roads,true,false};
		ParcelRoadIndex railSpace{RoadNetwork{}}; railSpace.addRailway(network);
		const TransportClearance crossings{&roads,world};
		for (const double lead : {80.0,160.0,280.0,440.0})
		{
			for (const double offset : {22.0,42.0,64.0})
			{
				if (addAt(network,world,station,lead,offset,roadSpace,railSpace,crossings,error)) { return true; }
			}
		}
		return false;
	}
	void generate(TrainNetwork& network, const World& world, const RoadNetwork& roads)
	{
		Array<int> candidates;
		for (const auto& node : network.nodes()) { if (node.type == TrackNodeType::Station && node.edgeIds.size() == 1) { candidates << node.id; } }
		for (const int station : candidates)
		{
			String error;
			if (!add(network,world,roads,station,error)) { DBG_LOG(U"[RailDepot] rejected station={} reason={}"_fmt(station,error)); }
			if (network.depots().size() >= 3) { break; }
		}
		DBG_LOG(U"[RailDepot] generated={} candidateStations={}"_fmt(network.depots().size(),candidates.size()));
	}
}
