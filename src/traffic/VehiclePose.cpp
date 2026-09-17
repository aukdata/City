#include "VehiclePose.hpp"
#include "../world/World.hpp"

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

Optional<Vehicle> VehiclePose::resolve(const Vehicle& vehicle,const RoadNetwork& roads,const World* world)
{
	Vehicle result;
	result.id = vehicle.id; result.type = vehicle.type; result.speed = vehicle.speed;
	result.currentEdge = vehicle.currentEdge; result.currentLane = vehicle.currentLane; result.arcPos = vehicle.arcPos;
	result.location = vehicle.location; result.connectionNodeId = vehicle.connectionNodeId; result.connectionId = vehicle.connectionId;
	result.laneFrom = vehicle.laneFrom; result.laneTo = vehicle.laneTo; result.laneChangeBlend = vehicle.laneChangeBlend;
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
		const auto* edge=roads.getEdge(vehicle.currentEdge);const auto curve=roads.getBezier(vehicle.currentEdge);
		if (!edge || !curve) { return none; }
		const bool changing=vehicle.location==VehicleLocation::ChangingLane;
		const int lane=changing ? vehicle.laneFrom : vehicle.currentLane;
		result.position=lanePosition(*curve,*edge,lane,vehicle.arcPos);
		if (changing) { result.position=result.position.lerp(lanePosition(*curve,*edge,vehicle.laneTo,vehicle.arcPos),vehicle.laneChangeBlend); }
		tangent=curve->tangentAt(Clamp(vehicle.arcPos,0.0f,curve->totalLength));
		if (InRange(lane,0,static_cast<int>(edge->lanes.size())-1) && edge->lanes[lane].dir==LaneDir::Backward) { tangent=-tangent; }
		designed=edge->usesDesignHeight();
	}
	if (!designed && world) { result.position.y=world->sampleHeight(static_cast<float>(result.position.x),static_cast<float>(result.position.z)); }
	result.position.y+=kRoadLineLift;
	result.heading=static_cast<float>(Atan2(tangent.x,tangent.z));
	result.pitch=static_cast<float>(Atan2(tangent.y,Vec2{tangent.x,tangent.z}.length()));
	return result;
}
