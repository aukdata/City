#include "VehiclePose.hpp"
#include "../world/World.hpp"
#include "TrafficCommon.hpp"

namespace
{
	Vec3 lanePosition(const CubicBezier& curve,const RoadEdge& edge,int lane,float arc)
	{
		const float clamped=Clamp(arc,0.0f,curve.totalLength);
		Vec3 point=curve.positionAt(clamped);
		if (InRange(lane,0,static_cast<int>(edge.lanes.size())-1))
		{
			point+=tangentToRight(curve.tangentAt(clamped))*edge.lanes[lane].centerAt(curve.totalLength>0 ? clamped/curve.totalLength : 0);
		}
		return point;
	}
}

const CubicBezier* EdgeCurveCache::get(const RoadNetwork& roads,int edgeId)
{
	const RoadEdge* edge=roads.getEdge(edgeId);if (!edge) { return nullptr; }
	const RoadNode* a=roads.getNode(edge->nodeA);const RoadNode* b=roads.getNode(edge->nodeB);if (!a || !b) { return nullptr; }
	auto found=m_curves.find(edgeId);
	if (found!=m_curves.end() && found->second.p0==a->position && found->second.p1==edge->ctrlA
		&& found->second.p2==edge->ctrlB && found->second.p3==b->position)
	{
		return &found->second;
	}
	CubicBezier curve{a->position,edge->ctrlA,edge->ctrlB,b->position};
	if (found!=m_curves.end()) { found->second=std::move(curve); return &found->second; }
	return &m_curves.emplace(edgeId,std::move(curve)).first->second;
}

Optional<Vehicle> VehiclePose::resolve(const Vehicle& vehicle,const RoadNetwork& roads,const World* world)
{
	EdgeCurveCache curves;
	return resolve(vehicle,roads,world,curves);
}

Optional<Vehicle> VehiclePose::resolve(const Vehicle& vehicle,const RoadNetwork& roads,const World* world,EdgeCurveCache& curves)
{
	Vehicle result;
	result.id = vehicle.id; result.type = vehicle.type; result.speed = vehicle.speed;
	result.currentEdge = vehicle.currentEdge; result.currentLane = vehicle.currentLane; result.arcPos = vehicle.arcPos;
	result.location = vehicle.location; result.connectionNodeId = vehicle.connectionNodeId; result.connectionId = vehicle.connectionId;
	result.laneFrom = vehicle.laneFrom; result.laneTo = vehicle.laneTo; result.laneChangeBlend = vehicle.laneChangeBlend; result.laneChangeDuration = vehicle.laneChangeDuration;
	Vec3 tangent;bool designed=false;
	if (vehicle.location==VehicleLocation::OnConnection)
	{
		const auto* node=roads.getNode(vehicle.connectionNodeId);if (!node) { return none; }
		const auto found=std::find_if(node->laneConnections.begin(),node->laneConnections.end(),[&](const auto& connection){return connection.id==vehicle.connectionId;});
		if (found==node->laneConnections.end()) { return none; }
		const float arc=Clamp(vehicle.arcPos,0.0f,found->path.totalLength);
		result.position=found->path.positionAt(arc);tangent=found->path.tangentAt(arc);
		designed=roads.nodeUsesDesignHeight(node->id);
	}
	else
	{
		const auto* edge=roads.getEdge(vehicle.currentEdge);const auto* curve=curves.get(roads,vehicle.currentEdge);
		if (!edge || !curve) { return none; }
		const bool changing=vehicle.location==VehicleLocation::ChangingLane;
		const int lane=changing ? vehicle.laneFrom : vehicle.currentLane;
		result.position=lanePosition(*curve,*edge,lane,vehicle.arcPos);
		Vec3 lateral{};
		if (changing)
		{
			// 横移動は S 字の進み具合で、車体は横へ動く速さの分だけ進行方向を振る（横滑りに見せない）。
			const Vec3 target=lanePosition(*curve,*edge,vehicle.laneTo,vehicle.arcPos);
			lateral=target-result.position;
			result.position=result.position.lerp(target,TrafficConfig::laneChangeProgress(vehicle.laneChangeBlend));
		}
		tangent=curve->tangentAt(Clamp(vehicle.arcPos,0.0f,curve->totalLength));
		if (InRange(lane,0,static_cast<int>(edge->lanes.size())-1) && edge->lanes[lane].dir==LaneDir::Backward) { tangent=-tangent; }
		if (changing && !tangent.isZero())
		{
			const double rate=TrafficConfig::laneChangeProgressRate(vehicle.laneChangeBlend)/Max(.1,static_cast<double>(vehicle.laneChangeDuration));
			tangent=tangent.normalized()*Max(1.0,static_cast<double>(vehicle.speed))+Vec3{lateral.x,0,lateral.z}*rate;
		}
		designed=edge->usesDesignHeight();
	}
	if (!designed && world) { result.position.y=world->sampleHeight(static_cast<float>(result.position.x),static_cast<float>(result.position.z)); }
	result.position.y+=kRoadLineLift;
	result.heading=static_cast<float>(Atan2(tangent.x,tangent.z));
	result.pitch=static_cast<float>(Atan2(tangent.y,Vec2{tangent.x,tangent.z}.length()));
	return result;
}
