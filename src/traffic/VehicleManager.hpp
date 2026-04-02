#pragma once
#include "Vehicle.hpp"
#include "TrafficLight.hpp"
#include "../sim/SimMessages.hpp"
#include "../sim/SimGraph.hpp"
#include "../debug/PerfStats.hpp"

/// @brief メインスレッド側の車両管理クラス
/// @details 車両の所有・IDM 更新・Active/Dormant 管理・経路リクエスト生成を行う。
///   SimThread とはメッセージキュー経由で通信し、shared_mutex は不要。
class VehicleManager
{
public:
	/// @brief SimGraph から信号機を構築する
	void init(const SimGraph& simGraph);

	/// @brief ネットワーク変更通知
	void onNetworkChanged(const SimGraph& simGraph);

	/// @brief 毎フレーム更新
	/// @param dt          ゲーム時間の経過秒（realDt * speedMultiplier）
	/// @param gameNow     ゲーム内時刻
	/// @param simGraph    道路トポロジ（Main 所有、読み取り専用）
	/// @param visibleEdges 前フレームの可視エッジ集合
	void update(double dt, GameTime gameNow,
	            const SimGraph& simGraph,
	            const HashSet<int>& visibleEdges);

	/// @brief Sim からの RouteResponse を適用する
	void applyRouteResponse(const RouteResponse& resp);

	/// @brief ランダムなエッジに車両をスポーンする
	void spawnRandom(const SimGraph& simGraph, VehicleType type = VehicleType::PassengerCar);

	/// @brief 送信待ちの SimRequest を全て取り出す
	Array<SimRequest> collectRequests();

	const Array<Vehicle>& vehicles() const { return m_vehicles; }
	int vehicleCount() const { return static_cast<int>(m_vehicles.size()); }

	const SimTickStats& lastStats() const { return m_stats; }

private:
	Array<Vehicle> m_vehicles;
	int            m_nextId = 0;

	// 信号機（Main 所有）
	HashTable<int, TrafficLight> m_trafficLights;
	bool m_lightsDirty = true;
	void buildTrafficLights(const SimGraph& simGraph);
	void updateTrafficLights(GameTime gameNow);
	const TrafficLight* getTrafficLight(int nodeId) const;

	// 送信待ちリクエスト
	Array<SimRequest> m_pendingRequests;

	// パフォーマンス計測
	SimTickStats m_stats;

	// --- 車両更新 ---
	void updateActiveVehicle(Vehicle& v, double dt, GameTime gameNow, const SimGraph& simGraph);
	void updateDormantVehicle(Vehicle& v, double dt);
	void advanceOnEdge(Vehicle& v, double dt, GameTime gameNow, const SimGraph& simGraph);
	bool transitToNextWaypoint(Vehicle& v, const SimGraph& simGraph);

	// --- Active/Dormant 遷移 ---
	void activateVehicle(Vehicle& v, const SimGraph& simGraph);
	void deactivateVehicle(Vehicle& v, const SimGraph& simGraph);

	// --- IDM ---
	static constexpr float kSignalStopDist = 15.0f;
	float idmAcceleration(const Vehicle& v, const IDMParams& params, bool fwdLane) const;

	// --- 車線変更 ---
	void tryLaneChange(Vehicle& v, const SimGraph& simGraph);

	// --- 経路リクエスト ---
	void requestRoute(Vehicle& v, const SimGraph& simGraph);
};
