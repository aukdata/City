#include "DrivingController.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/JunctionGeometry.hpp"

namespace
{
	constexpr double kSurfaceRadius=112, kSurfaceCell=64, kHeightTolerance=1.25;
	constexpr double kWheelHalfTrack=.72, kWheelEdgeTolerance=.18, kContactMargin=.02;
	constexpr double kMaximumStep=1.0/120, kMaximumFrame=.1;
	constexpr double kForwardSpeed=120.0/3.6, kReverseSpeed=15.0/3.6;
	Vec3 forward(double heading) { return {Sin(heading),0,Cos(heading)}; }
	double approach(double value,double target,double amount)
	{
		return value<target ? Min(value+amount,target) : Max(value-amount,target);
	}
	bool drivable(const Lane& lane)
	{
		return lane.allows(TransportMode::Road) && (lane.op==OpState::Open || lane.op==OpState::Provisional)
			&& lane.type!=LaneType::KeepOut && lane.type!=LaneType::TrafficIsland
			&& lane.type!=LaneType::ParkingBay && lane.type!=LaneType::EmergencyStop;
	}
	bool usable(const RoadEdge& edge)
	{
		return edge.id>=0 && (edge.edgeState==EdgeState::Open || edge.edgeState==EdgeState::Existing)
			&& edge.isRoadbedBuilt() && edge.lanes.any(drivable);
	}
	Vec2 contactSize(const Vehicle& vehicle)
	{
		// 車体幅とバンパーまでの全長 [m]。ミラーの張り出しで道を塞がない。
		switch (vehicle.type)
		{
		case VehicleType::PassengerCar: return {1.80,4.63};
		case VehicleType::KeiCar: return {1.50,3.52};
		case VehicleType::SmallTruck: return {2.10,6.21};
		case VehicleType::LargeTruck: return {2.50,12.11};
		case VehicleType::Bus: return {2.50,10.63};
		case VehicleType::Emergency: return (vehicle.id&1) ? Vec2{2.10,6.63} : Vec2{1.80,4.63};
		default: return {1.8,4.0}; // 現在の二輪車の簡易表示と一致させる。
		}
	}
}

double DrivingController::contactDepth(const Vehicle& first,const Vehicle& second,double margin)
{
	if (Abs(first.position.y-second.position.y)>2.5) { return 0; }
	const Vec3 delta=second.position-first.position;
	const Vec3 firstForward=forward(first.heading),secondForward=forward(second.heading);
	const Vec3 firstRight=tangentToRight(firstForward),secondRight=tangentToRight(secondForward);
	const Vec2 sizeA=contactSize(first)*.5,sizeB=contactSize(second)*.5;
	double depth=Math::Inf;
	for (const Vec3 axis : {firstForward,firstRight,secondForward,secondRight})
	{
		const double radiusA=sizeA.y*Abs(axis.dot(firstForward))+sizeA.x*Abs(axis.dot(firstRight));
		const double radiusB=sizeB.y*Abs(axis.dot(secondForward))+sizeB.x*Abs(axis.dot(secondRight));
		const double overlap=radiusA+radiusB+margin-Abs(delta.dot(axis));
		if (overlap<=0) { return 0; }
		depth=Min(depth,overlap);
	}
	return depth;
}

bool DrivingController::overlaps(const Vehicle& first,const Vehicle& second,double margin)
{
	return contactDepth(first,second,margin)>0;
}

void DrivingController::rebuildSurface(Vec3 focus,const World& world,const RoadNetwork& roads)
{
	m_surface.clear();m_cell={static_cast<int>(Floor(focus.x/kSurfaceCell)),static_cast<int>(Floor(focus.z/kSurfaceCell))};
	m_edgeCount=roads.edges().size();m_surfaceAge=0;
	const RectF region{focus.x-kSurfaceRadius,focus.z-kSurfaceRadius,kSurfaceRadius*2,kSurfaceRadius*2};
	HashSet<int> nodes;
	const auto append=[&](const MeshData& mesh,int edgeId,float limit)
	{
		for (const auto face : mesh.indices)
		{
			const Vec3 a{mesh.vertices[face.i0].pos},b{mesh.vertices[face.i1].pos},c{mesh.vertices[face.i2].pos};
			const Vec2 low{Min(a.x,Min(b.x,c.x)),Min(a.z,Min(b.z,c.z))},high{Max(a.x,Max(b.x,c.x)),Max(a.z,Max(b.z,c.z))};
			const RectF bounds{low,high-low};
			if (bounds.intersects(region)) { m_surface<<SurfaceTriangle{a,b,c,bounds.stretched(.002),edgeId,limit}; }
		}
	};
	for (const auto& edge : roads.edges())
	{
		if (!usable(edge)) { continue; }
		const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
		Vec2 low{curve->p0.x,curve->p0.z},high=low;
		for (const Vec3 point : {curve->p1,curve->p2,curve->p3})
		{
			low.x=Min(low.x,point.x);low.y=Min(low.y,point.z);high.x=Max(high.x,point.x);high.y=Max(high.y,point.z);
		}
		if (!RectF{low,high-low}.stretched(edge.totalWidth()).intersects(region)) { continue; }
		RoadEdge pavement=edge;
		if (edge.hasRailLanes())
		{
			// 併用断面でも運転可能な面は車線だけ。軌道を横切って走り続けられない。
			pavement.parts.clear();
			for (const auto& lane : edge.lanes)
			{
				if (!drivable(lane)) { continue; }
				RoadPart part; part.type=RoadPartType::Roadbed;
				part.offsetA_L=lane.offsetA_L;part.offsetA_R=lane.offsetA_R;
				part.offsetB_L=lane.offsetB_L;part.offsetB_R=lane.offsetB_R;
				pavement.parts<<part;
			}
		}
		for (auto& part : pavement.parts) { if (part.type==RoadPartType::Shoulder) { part.type=RoadPartType::Roadbed; } }
		append(RoadGeometry::roadbedSurface(pavement,*curve,world),edge.id,edge.speedLimit);
		nodes.insert(edge.nodeA);nodes.insert(edge.nodeB);
	}
	for (const int nodeId : nodes) { append(JunctionGeometry::build(roads,nodeId,true,&world).asphalt,-1,0); }
}

Optional<DrivingController::SurfacePoint> DrivingController::surfaceAt(Vec3 position,double edgeTolerance) const
{
	Optional<SurfacePoint> best;double difference=kHeightTolerance;
	double nearestDistance=Square(edgeTolerance)+1e-10;
	for (const auto& face : m_surface)
	{
		if (!face.bounds.stretched(edgeTolerance).contains(Vec2{position.x,position.z})) { continue; }
		const Vec2 ab{face.b.x-face.a.x,face.b.z-face.a.z},ac{face.c.x-face.a.x,face.c.z-face.a.z};
		const Vec2 relative{position.x-face.a.x,position.z-face.a.z};
		const double area=ab.x*ac.y-ab.y*ac.x;if (Abs(area)<1e-8) { continue; }
		const double u=(relative.x*ac.y-relative.y*ac.x)/area,v=(ab.x*relative.y-ab.y*relative.x)/area;
		double height=face.a.y+(face.b.y-face.a.y)*u+(face.c.y-face.a.y)*v;
		double distance=0;
		if (u<-.00001 || v<-.00001 || u+v>1.00001)
		{
			if (edgeTolerance<=0) { continue; }
			distance=Math::Inf;
			const std::array<Vec3,3> vertices{face.a,face.b,face.c};
			for (size_t i=0;i<vertices.size();++i)
			{
				const Vec3 a=vertices[i],b=vertices[(i+1)%vertices.size()];
				const Vec2 delta{b.x-a.x,b.z-a.z},offset{position.x-a.x,position.z-a.z};
				const double fraction=Clamp(offset.dot(delta)/Max(1e-12,delta.lengthSq()),0.0,1.0);
				const double candidateDistance=(offset-delta*fraction).lengthSq();
				if (candidateDistance<distance) { distance=candidateDistance;height=Math::Lerp(a.y,b.y,fraction); }
			}
		}
		const double heightDifference=Abs(height-position.y);
		if (heightDifference<kHeightTolerance && distance<=nearestDistance
			&& (distance<nearestDistance || heightDifference<difference))
		{
			nearestDistance=distance;difference=heightDifference;best=SurfacePoint{height,face.edgeId,face.speedLimit};
		}
	}
	return best;
}

bool DrivingController::fitToRoad(Vehicle& candidate) const
{
	const Vec3 along=forward(candidate.heading),right=tangentToRight(along);
	double front=0,back=0;
	for (const int end : {-1,1})
	{
		for (const int side : {-1,1})
		{
			const auto surface=surfaceAt(candidate.position+along*(end*kWheelbase*.5)+right*(side*kWheelHalfTrack),kWheelEdgeTolerance);
			if (!surface) { return false; }
			(end>0 ? front : back)+=surface->height*.5;
		}
	}
	const auto center=surfaceAt(candidate.position);if (!center) { return false; }
	candidate.position.y=center->height;candidate.pitch=static_cast<float>(Atan2(front-back,kWheelbase));
	if (center->edgeId>=0) { candidate.currentEdge=center->edgeId; }
	return true;
}

bool DrivingController::blockedByTraffic(const Vehicle& candidate,const Array<Vehicle>& traffic,bool allowEscape) const
{
	for (const auto& vehicle : traffic)
	{
		if (vehicle.mode!=VehicleMode::Active) { continue; }
		const double nextDepth=contactDepth(candidate,vehicle,kContactMargin);
		if (nextDepth<=0) { continue; }
		const double currentDepth=contactDepth(m_vehicle,vehicle,kContactMargin);
		// Existing contact can be escaped or slid along. Do not permit a new
		// collision or a move that penetrates further into the other body.
		const bool separating=nextDepth<currentDepth-1e-7
			|| (nextDepth<=currentDepth+1e-7 && candidate.position.distanceFromSq(vehicle.position)>=m_vehicle.position.distanceFromSq(vehicle.position)-1e-8);
		if (allowEscape && currentDepth>0 && separating) { continue; }
		return true;
	}
	return false;
}

bool DrivingController::enter(Vec3 nearPosition,const World& world,const RoadNetwork& roads,const Array<Vehicle>& traffic)
{
	struct Candidate { Vehicle vehicle;double score=0; };
	Array<Candidate> candidates;
	constexpr double kEntryRadius=1200;
	for (const auto& edge : roads.edges())
	{
		if (!usable(edge)) { continue; }
		const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
		const float start=Max(6.0f,edge.cutoffA+3),end=curve->totalLength-Max(6.0f,edge.cutoffB+3);
		if (end<start) { continue; }
		const int count=Max(1,static_cast<int>(Ceil((end-start)/12)));
		for (int index=0;index<=count;++index)
		{
			const float arc=Math::Lerp(start,end,static_cast<float>(index)/count);
			const Vec3 center=curve->positionAt(arc);
			if (Vec2{center.x-nearPosition.x,center.z-nearPosition.z}.lengthSq()>kEntryRadius*kEntryRadius) { continue; }
			for (int laneIndex=0;laneIndex<static_cast<int>(edge.lanes.size());++laneIndex)
			{
				const auto& lane=edge.lanes[laneIndex];
				if (!drivable(lane)) { continue; }
				Vehicle vehicle;vehicle.id=-2;vehicle.currentEdge=edge.id;vehicle.currentLane=laneIndex;vehicle.arcPos=arc;
				vehicle.position=center+tangentToRight(curve->tangentAt(arc))*lane.centerAt(arc/curve->totalLength);
				vehicle.position.y=RoadGeometry::surfaceY(edge,center,world.sampleHeight(static_cast<float>(center.x),static_cast<float>(center.z)));
				const Vec3 tangent=curve->tangentAt(arc)*(lane.dir==LaneDir::Forward ? 1 : -1);
				vehicle.heading=static_cast<float>(Atan2(tangent.x,tangent.z));
				const Vec3 delta=vehicle.position-nearPosition;
				candidates<<Candidate{vehicle,delta.x*delta.x+delta.z*delta.z+4*delta.y*delta.y};
			}
		}
	}
	candidates.sort_by([](const Candidate& a,const Candidate& b){return a.score<b.score;});
	for (size_t index=0;index<Min(size_t{48},candidates.size());++index)
	{
		auto vehicle=candidates[index].vehicle;rebuildSurface(vehicle.position,world,roads);
		if (!fitToRoad(vehicle) || blockedByTraffic(vehicle,traffic)) { continue; }
		m_vehicle=vehicle;m_active=true;m_blocked=false;m_steering=m_distance=m_reverseHold=0;
		m_speedLimit=roads.getEdge(vehicle.currentEdge)->speedLimit;
		return true;
	}
	return false;
}

void DrivingController::stopMotion() { m_vehicle.speed=0;m_steering=0;m_reverseHold=0; }
void DrivingController::leave() { m_active=false;stopMotion();m_surface.clear(); }

void DrivingController::update(double dt,const DrivingInput& input,const World& world,const RoadNetwork& roads,
	const Array<Vehicle>& traffic,bool enabled)
{
	if (!m_active) { return; }
	if (!enabled) { stopMotion();return; }
	dt=Clamp(dt,0.0,kMaximumFrame);m_surfaceAge+=dt;
	const Point cell{static_cast<int>(Floor(m_vehicle.position.x/kSurfaceCell)),static_cast<int>(Floor(m_vehicle.position.z/kSurfaceCell))};
	if (cell!=m_cell || m_edgeCount!=roads.edges().size() || m_surfaceAge>.5) { rebuildSurface(m_vehicle.position,world,roads); }
	m_blocked=false;
	for (double remaining=dt;remaining>1e-8;)
	{
		const double step=Min(remaining,kMaximumStep);remaining-=step;
		double speed=m_vehicle.speed;
		const double throttle=Clamp(input.throttle,0.0,1.0),reverse=Clamp(input.brakeReverse,0.0,1.0);
		if (input.handbrake || (throttle>0 && reverse>0)) { speed=approach(speed,0,9*step);m_reverseHold=0; }
		else if (throttle>0)
		{
			m_reverseHold=0;speed+=step*throttle*(speed<0 ? 7 : 3.5*(1-Square(speed/kForwardSpeed)));
		}
		else if (reverse>0)
		{
			if (speed>.03) { speed=Max(0.0,speed-7*reverse*step);m_reverseHold=0; }
			else { m_reverseHold+=step;if (m_reverseHold>.35) { speed=Max(-kReverseSpeed,speed-2.5*reverse*step); } }
		}
		else { m_reverseHold=0;speed=approach(speed,0,(.28+.004*speed*speed)*step); }
		if (Abs(speed)>.05 && !input.handbrake) { speed-=9.81*Sin(m_vehicle.pitch)*step; }
		speed=Clamp(speed,-kReverseSpeed,kForwardSpeed);
		const double limit=Min(.55,Atan(kWheelbase*5.5/Max(25.0,speed*speed)));
		m_steering=approach(m_steering,Clamp(input.steering,-1.0,1.0)*limit,1.6*step);
		const double turn=speed/kWheelbase*Tan(m_steering)*step;
		Vehicle next=m_vehicle;next.speed=static_cast<float>(speed);
		next.heading=static_cast<float>(m_vehicle.heading+turn);
		next.position+=forward(m_vehicle.heading+turn*.5)*(speed*step);
		const bool roadContact=!fitToRoad(next);
		if (roadContact || blockedByTraffic(next,traffic,true))
		{
			m_vehicle.speed=0;m_blocked=true;break;
		}
		m_distance+=next.position.distanceFrom(m_vehicle.position);m_vehicle=next;
		if (const auto* edge=roads.getEdge(m_vehicle.currentEdge)) { m_speedLimit=edge->speedLimit; }
	}
}
