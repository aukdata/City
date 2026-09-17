#pragma once
#include "Vehicle.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 運転入力。経路や車線に操舵を固定せず、車体の姿勢を直接動かす。
struct DrivingInput
{
	double throttle=0, brakeReverse=0, steering=0;
	bool handbrake=false;
};

/// @brief 道路と交差点の実路面上を走る、プレイヤー用の自動車。
class DrivingController
{
public:
	static constexpr double kHalfLength=2.25, kHalfWidth=.9, kWheelbase=2.65;
	bool enter(Vec3 nearPosition,const World& world,const RoadNetwork& roads,const Array<Vehicle>& traffic={});
	void leave();
	void update(double dt,const DrivingInput& input,const World& world,const RoadNetwork& roads,
		const Array<Vehicle>& traffic={},bool enabled=true);
	void stopMotion();
	[[nodiscard]] bool active() const { return m_active; }
	[[nodiscard]] const Vehicle& vehicle() const { return m_vehicle; }
	[[nodiscard]] double steering() const { return m_steering; }
	[[nodiscard]] double distance() const { return m_distance; }
	[[nodiscard]] float speedLimit() const { return m_speedLimit; }
	[[nodiscard]] bool blocked() const { return m_blocked; }
	[[nodiscard]] bool reversing() const { return m_vehicle.speed<-.05f; }
	/// @brief 車体矩形による低速接触判定。上下の異なる道路は分ける。
	[[nodiscard]] static bool overlaps(const Vehicle& first,const Vehicle& second,double margin=0);
private:
	struct SurfaceTriangle { Vec3 a,b,c;RectF bounds;int edgeId=-1;float speedLimit=0; };
	struct SurfacePoint { double height=0;int edgeId=-1;float speedLimit=0; };
	Array<SurfaceTriangle> m_surface;
	Point m_cell{-999,-999};size_t m_edgeCount=0;double m_surfaceAge=0;
	Vehicle m_vehicle;
	bool m_active=false,m_blocked=false;
	double m_steering=0,m_reverseHold=0,m_distance=0;
	float m_speedLimit=0;
	[[nodiscard]] static double contactDepth(const Vehicle& first,const Vehicle& second,double margin);
	void rebuildSurface(Vec3 focus,const World& world,const RoadNetwork& roads);
	[[nodiscard]] Optional<SurfacePoint> surfaceAt(Vec3 position,double edgeTolerance=0) const;
	[[nodiscard]] bool fitToRoad(Vehicle& candidate) const;
	[[nodiscard]] bool blockedByTraffic(const Vehicle& candidate,const Array<Vehicle>& traffic,bool allowEscape=false) const;
};
