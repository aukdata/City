#include "VehicleManager.hpp"
#include "VehiclePose.hpp"
#include "DrivingController.hpp"

void VehicleManager::avoidDrivenVehicle(Vehicle& vehicle,double dt,const RoadNetwork& roads) const
{
	if (!m_drivenVehicle || dt<=0) { return; }
	// Reject distant road bounds before resolving a pose (and copying its route).
	const auto* bounds = roads.getEdge(vehicle.currentEdge);
	if (!bounds) { return; }
	const auto* nodeA = roads.getNode(bounds->nodeA);
	const auto* nodeB = roads.getNode(bounds->nodeB);
	if (!nodeA || !nodeB) { return; }
	const auto& driver = m_drivenVehicle->position;
	constexpr double kBoundsMargin = 110;
	const double minX = Min(Min(nodeA->position.x, nodeB->position.x), Min(bounds->ctrlA.x, bounds->ctrlB.x));
	const double maxX = Max(Max(nodeA->position.x, nodeB->position.x), Max(bounds->ctrlA.x, bounds->ctrlB.x));
	const double minZ = Min(Min(nodeA->position.z, nodeB->position.z), Min(bounds->ctrlA.z, bounds->ctrlB.z));
	const double maxZ = Max(Max(nodeA->position.z, nodeB->position.z), Max(bounds->ctrlA.z, bounds->ctrlB.z));
	if (driver.x < minX - kBoundsMargin || driver.x > maxX + kBoundsMargin
		|| driver.z < minZ - kBoundsMargin || driver.z > maxZ + kBoundsMargin) { return; }
	const auto current=VehiclePose::resolve(vehicle,roads,m_drivingWorld);
	constexpr double kDetectionRange=100,kSampleStep=.5,kBraking=6,kClearance=.35;
	if (!current || current->position.distanceFromSq(m_drivenVehicle->position)>kDetectionRange*kDetectionRange) { return; }
	const auto* edge=roads.getEdge(vehicle.currentEdge);
	const int lane=vehicle.location==VehicleLocation::ChangingLane ? vehicle.laneFrom : vehicle.currentLane;
	const bool backward=vehicle.location!=VehicleLocation::OnConnection && edge
		&& InRange(lane,0,static_cast<int>(edge->lanes.size())-1) && edge->lanes[lane].dir==LaneDir::Backward;
	const double lookAhead=Min(kDetectionRange,Max(12.0,Square(vehicle.speed)/(2*kBraking)+vehicle.speed*dt+5));
	for (double distance=0;distance<=lookAhead;distance+=kSampleStep)
	{
		Vehicle projected=vehicle;projected.arcPos+=static_cast<float>(distance*(backward ? -1 : 1));
		const auto pose=VehiclePose::resolve(projected,roads,m_drivingWorld);
		if (!pose || !DrivingController::overlaps(*pose,*m_drivenVehicle,kClearance)) { continue; }
		const double gap=Max(0.0,distance-kSampleStep);
		vehicle.speed=static_cast<float>(Min(static_cast<double>(vehicle.speed),Min(Sqrt(2*kBraking*gap),gap/dt)));
		return;
	}
}
