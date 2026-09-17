#include "NewTownLayout.hpp"
#include "StreetBlocks.hpp"
#include "../debug/DebugLog.hpp"

void NewTownLayout::finish(MapGenerator::Settlement& settlement, RoadNetwork& network)
{
	auto& plan = settlement.plan;
	if (plan.origin != UrbanMorphology::Origin::Planned || plan.frontageRoads)
	{
		return;
	}
	plan.greenways.clear();
	plan.neighborhoodParks.clear();
	const auto x = UrbanMorphology::streetCoordinates(plan, false), z = UrbanMorphology::streetCoordinates(plan, true);
	const auto local = [&](Vec3 position)
	{
		const Vec2 delta{position.x - settlement.center.x, position.z - settlement.center.y};
		return Vec2{delta.dot(settlement.gridAxisX), delta.dot(settlement.gridAxisZ)};
	};
	const auto oddCorridor = [](const Array<float>& coordinates, double value)
	{
		constexpr double kCoordinateTolerance = 0.1;
		for (size_t i = 1; i + 1 < coordinates.size(); i += 2)
		{
			if (Abs(value - coordinates[i]) < kCoordinateTolerance)
			{
				return true;
			}
		}
		return false;
	};
	int carRoads = 0;
	HashSet<int> changedNodes;
	for (const auto& item : network.edges())
	{
		if (item.id < 0 || !item.isRoadbedBuilt())
		{
			continue;
		}
		auto& edge = *network.getEdge(item.id);
		const auto* a = network.getNode(edge.nodeA);
		const auto* b = network.getNode(edge.nodeB);
		if (!a || !b)
		{
			continue;
		}
		const Vec2 from = local(a->position), to = local(b->position);
		if (!UrbanMorphology::inCore(plan, from, 0.1) || !UrbanMorphology::inCore(plan, to, 0.1))
		{
			continue;
		}
		// 幹線への入口・駅前道路は維持する。奇数番の住区内道路だけを緑道へ変更。
		const bool greenway =
			edge.roadType == RoadType::LocalRoad && ((Abs(from.x - to.x) < 0.1 && oddCorridor(x, from.x)) ||
														(Abs(from.y - to.y) < 0.1 && oddCorridor(z, from.y)));
		if (!greenway)
		{
			edge.parts.remove_if([](const RoadPart& part) { return part.type == RoadPartType::UtilityPole; });
			++carRoads;
			continue;
		}
		changedNodes.insert(edge.nodeA);
		changedNodes.insert(edge.nodeB);
		edge.laneVehicles.clear();
		edge.signs.clear();
		edge.lanes.clear();
		edge.parts.clear();
		edge.speedLimit = 0;
		edge.edgeState = EdgeState::Open;
		const float half = static_cast<float>(GenerationSettings::get().urbanFabric_newTownWalkwayWidth * .5);
		const auto add = [&](RoadPartType type, StringView id, float left, float right)
		{
			RoadPart part;
			part.type = type;
			part.defId = id;
			part.build = BuildState::Built;
			part.offsetA_L = part.offsetB_L = left;
			part.offsetA_R = part.offsetB_R = right;
			edge.parts << part;
		};
		add(RoadPartType::Roadbed, U"roadbed_asphalt", -half, half);
		add(RoadPartType::Sidewalk, U"sidewalk_tile", -half, 0);
		add(RoadPartType::Sidewalk, U"sidewalk_tile", 0, half);
		plan.greenways << Line{from, to};
	}
	// 削除した車線への古い交差点接続を残さない。細くなった道の交差点端の切り詰め量も再計算。
	for (const int node : changedNodes)
	{
		network.updateNodeCutoffs(node);
	}
	for (const int node : changedNodes)
	{
		network.rebuildLaneConnections(node);
	}
	// 緑道だけに囲まれた街区や外周緑地の角地を、接道する宅地だと誤認しない。
	// 車道から建物幅ぶん後退した位置を採れる面だけを住宅用に残す。
	for (const auto& block:StreetBlocks::collect(network))
	{
		Array<Vec2> boundary;
		bool inside=true;
		for (const Vec2 point:block.outline)
		{
			const Vec2 p=local({point.x,0,point.y}); boundary << p;
			inside &= UrbanMorphology::inCore(plan,p,.1);
		}
		if (!inside) { continue; }
		bool frontage=false;
		for (const int id:block.edges)
		{
			const auto* edge=network.getEdge(id);
			if (!edge || !edge->hasRoadLanes()) { continue; }
			const auto curve=network.getBezier(id);
			for (const float fraction:{.25f,.5f,.75f})
			{
				const Vec3 p=curve->evaluate(fraction),tangent=curve->tangentAt(curve->totalLength*fraction);
				const Vec2 right=Vec2{tangent.z,-tangent.x}.normalized();
				const double distance=edge->totalWidth()*.5+GenerationSettings::get().buildings_footprint_Office*.5+GenerationSettings::get().development_defaultBuildingSetbackM;
				for (const int side:{-1,1})
				{
					const Vec2 candidate=Vec2{p.x,p.z}+right*(distance*side);
					frontage |= block.contains(candidate) && !UrbanMorphology::isNewTownGreen(plan,local({candidate.x,0,candidate.y}));
				}
			}
		}
		if (!frontage) { plan.neighborhoodParks << Polygon{boundary}; }
	}
	DBG_LOG(U"[NewTownLayout] center=({}, {}) carRoads={} greenways={} parks={}"_fmt(
		settlement.center.x, settlement.center.y, carRoads, plan.greenways.size(),plan.neighborhoodParks.size()));
}
