#pragma once
#include "Vehicle.hpp"
class World;
#include "TrafficLight.hpp"
#include "LaneVehicleIndex.hpp"
#include "BuildingAccess.hpp"
#include "TrafficCommon.hpp"
#include "../sim/SimMessages.hpp"
#include "../sim/SimGraph.hpp"
#include "../road/RoadNetwork.hpp"
#include "../debug/PerfStats.hpp"
#include "../gameplay/CitySimulation.hpp"

/// @brief メインスレッド側の車両管理クラス
/// @details 車両の所有・IDM 更新・Active/Dormant 管理・経路リクエスト生成を行う。
///   SimThread とはメッセージキュー経由で通信し、shared_mutex は不要。
class VehicleManager
{
public:
	/// @brief SimGraph から信号機を構築する
	void init(const SimGraph& simGraph, const RoadNetwork& network, const World* world = nullptr);
	const BuildingAccessIndex& buildingAccess() const { return m_buildingAccess; }

	/// @brief ネットワーク変更通知
	void onNetworkChanged(const SimGraph& simGraph, const RoadNetwork& network,
	                      const NetworkChangeContext& context);

	/// @brief 毎フレーム更新
	void update(double dt, GameTime gameNow,
	            const SimGraph& simGraph,
	            const RoadNetwork& network,
	            const HashSet<int>& visibleEdges);

	/// @brief Sim からの RouteResponse を適用する
	void applyRouteResponse(const RouteResponse& resp);

	/// @brief ランダムなエッジに車両をスポーンする
	void spawnRandom(const SimGraph& simGraph, VehicleType type = VehicleType::PassengerCar);

	/// @brief 指定エッジに車両をスポーンする（デバッグ用）
	/// @param goalEdgeId 目的地エッジ (-1 でランダム)
	void spawnOnEdge(int edgeId, const SimGraph& simGraph,
	                 VehicleType type = VehicleType::PassengerCar, int goalEdgeId = -1);

	/// @brief 車両のゴールを変更して経路を再探索する
	void setGoalAndReroute(int vehicleId, int goalEdgeId, const SimGraph& simGraph);

	/// @brief 送信待ちの SimRequest を全て取り出す
	Array<SimRequest> collectRequests();

	struct PopulationStats
	{
		int spawned = 0, completed = 0, recycled = 0, routeFailures = 0;
		int localVehicles = 0, localTarget = 0;
	};
	const PopulationStats& populationStats() const { return m_populationStats; }
	/// @brief 見ている街の通常交通を維持する。建物の前で発着する。
	void setTrafficFocus(Vec3 point)
	{
		const Vec2 next{point.x, point.z};
		if (!m_trafficFocus || m_trafficFocus->distanceFrom(next) > 100) { m_spawnRefresh = 0; }
		m_trafficFocus = next;
	}

	/// @brief 自由運転車を交通の障害物として公開する。降車時は none。
	void setDrivenVehicle(Optional<Vehicle> vehicle,const World* world=nullptr) { m_drivenVehicle=std::move(vehicle);m_drivingWorld=world; }
	const Array<Vehicle>& vehicles() const { return m_vehicles; }
	int vehicleCount() const { return static_cast<int>(m_vehicles.size()); }

	/// @brief 人口・用途から算出済みの交通需要を反映する
	void setTrafficDemand(const TrafficDemand& demand) { m_trafficDemand = demand; m_trafficDemandConfigured = true; }

	/// @brief イベント等による全車速度係数を設定する
	void setGlobalSpeedMultiplier(double multiplier)
	{
		m_globalSpeedMultiplier = Clamp(multiplier, 0.05, 2.0);
	}

	/// @brief イベントによる速度・交通量補正をまとめて反映する
	void applyTrafficEventEffect(const TrafficEventEffect& effect)
	{
		setGlobalSpeedMultiplier(effect.speedMultiplier);
		m_eventDemandMultiplier = Clamp(effect.demandMultiplier, 0.0, 4.0);
	}

	/// @brief 直近200件の完了トリップ時間 [分]
	const Array<double>& completedTripMinutes() const { return m_completedTripMinutes; }

	/// @brief 停留所エッジ列を循環するバスを生成し、生成IDを返す
	int spawnBus(const Array<int>& stopEdgeIds, int routeId, const SimGraph& simGraph);

	const SimTickStats& lastStats() const { return m_stats; }

	/// @brief 信号機の再構築を要求する（TrafficControl 変更時に呼ぶ）
	void markLightsDirty() { m_lightsDirty = true; }

	/// @brief 信号機マップを返す（描画用）
	const HashTable<int, TrafficLight>& trafficLights() const { return m_trafficLights; }

	/// @brief 指定ノードの信号機を返す（なければ nullptr）
	const TrafficLight* getTrafficLight(int nodeId) const;

private:
	Array<Vehicle> m_vehicles;
	Optional<Vehicle> m_drivenVehicle;
	const World* m_drivingWorld=nullptr;
	void avoidDrivenVehicle(Vehicle& vehicle,double dt,const RoadNetwork& roads) const;
	Optional<Vec2> m_trafficFocus;
	Array<int> m_localSpawnEdges, m_globalSpawnEdges;
	double m_spawnRefresh = 0, m_spawnCredit = 0;
	const World* m_world = nullptr;
	BuildingAccessIndex m_buildingAccess;
	LaneVehicleIndex m_laneTraffic;
	HashTable<int, size_t> m_vehicleIndices;
	HashSet<int> m_localSpawnSet;
	bool assignBuildingGoal(Vehicle& vehicle, const SimGraph& graph);
	int m_localTrafficTarget = 0;
	PopulationStats m_populationStats;
	bool tryAutomaticSpawn(int edgeId, const SimGraph& graph, VehicleType type, const RoadNetwork* network);
	void replenishTraffic(double dt, const SimGraph& graph, const RoadNetwork& network, const HashSet<int>& visibleEdges);
	int            m_nextId = 0;
	int            m_targetVehicleCount = 20;  ///< 自動スポーンの目標台数
	TrafficDemand  m_trafficDemand;
	bool           m_trafficDemandConfigured = false;
	double         m_globalSpeedMultiplier = 1.0;
	double         m_eventDemandMultiplier = 1.0;
	GameTime       m_lastGameNow = 0.0;
	Array<double>  m_completedTripMinutes;

	// 信号機（Main 所有）
	HashTable<int, TrafficLight> m_trafficLights;
	bool m_lightsDirty = true;
	void buildTrafficLights(const SimGraph& simGraph, const RoadNetwork* network = nullptr);
	void rebuildTrafficLightForNode(int nodeId, const SimGraph& simGraph, const RoadNetwork& network);
	void updateTrafficLights(GameTime gameNow);

	// 送信待ちリクエスト
	Array<SimRequest> m_pendingRequests;

	// パフォーマンス計測
	SimTickStats m_stats;

	// --- 車両更新 ---
	void updateActiveVehicle(Vehicle& v, double dt, const SimGraph& simGraph, const RoadNetwork& network);
	void advanceOnSegment(Vehicle& v, double dt, const SimGraph& simGraph, const RoadNetwork& network);
	void advanceOnConnection(Vehicle& v, double dt, const SimGraph& simGraph, const RoadNetwork& network);
	void advanceOnLane(Vehicle& v, double dt, const SimGraph& simGraph, const RoadNetwork& network);
	bool transitToNextWaypoint(Vehicle& v, const SimGraph& simGraph, const RoadNetwork& network);
	bool fallbackRandomTransit(Vehicle& v, const SimGraph& simGraph, const RoadNetwork& network);

	// --- 交通規制 ---
	TrafficControl getEdgeControl(int nodeId, int edgeId, const SimGraph& simGraph) const;
	bool hasConflictingTraffic(const Vehicle& v, int nodeId, int edgeId, const SimGraph& simGraph) const;

	// --- 車線変更 ---
	void tryLaneChange(Vehicle& v, const SimGraph& simGraph);

	// --- 経路リクエスト ---
	void requestRoute(Vehicle& v, const SimGraph& simGraph);
	VehicleType selectDemandVehicleType() const;
	void recordCompletedTrip(const Vehicle& vehicle, GameTime gameNow);
};
