#pragma once
#include "BusRoute.hpp"
#include "VehicleManager.hpp"

/// @brief バス停・路線の所有とヘッドウェイ運行を管理する
class BusSystem
{
public:
	/// @brief 最寄り道路へ吸着したバス停を追加する。失敗時は -1
	int addStop(Vec3 position, String name, const RoadNetwork& network,
		float maximumSnapDistance = 40.0f);

	/// @brief 2停留所以上の路線を作成する。失敗時は -1
	int addRoute(const Array<int>& stopIds, float headwaySec = 120.0f);

	/// @brief 発車間隔に従ってバスを生成する
	void update(GameTime gameNow, const SimGraph& simGraph, VehicleManager& vehicles);

	const Array<BusStop>& stops() const { return m_stops; }
	const Array<BusRoute>& routes() const { return m_routes; }
	int activeRouteCount() const { return static_cast<int>(m_routes.size()); }

private:
	Array<BusStop> m_stops;
	Array<BusRoute> m_routes;
	int m_nextStopId = 0;
	int m_nextRouteId = 0;

	const BusStop* getStop(int id) const;
};

