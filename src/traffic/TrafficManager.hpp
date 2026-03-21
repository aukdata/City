#pragma once
#include "Vehicle.hpp"
#include "PathfindingGraph.hpp"
#include "TrafficLight.hpp"
#include "BusRoute.hpp"
#include "../sim/SimGraph.hpp"

/// @brief 車両生成・更新・管理クラス
/// @details SimGraph を使用してシミュレーションを行う。
///   RoadNetwork / World への依存を持たず、Sim スレッドで安全に実行できる。
class TrafficManager
{
public:
	/// @brief SimGraph を設定する
	void setSimGraph(std::shared_ptr<const SimGraph> graph) { m_simGraph = std::move(graph); }

	/// @brief フレーム更新
	/// @param dt      リアル経過秒
	/// @param gameNow 現在のゲーム時刻
	void update(double dt, GameTime gameNow);

	/// @brief 車両を直接追加する
	void addVehicle(Vehicle v);

	/// @brief ランダムなエッジに車両を生成する
	void spawnVehicle(VehicleType type = VehicleType::PassengerCar);

	/// @brief 道路ネットワーク変化を通知する（次 update でグラフ再構築）
	void markNetworkDirty() { m_graphDirty = true; }

	const Array<Vehicle>& vehicles() const { return m_vehicles; }
	int vehicleCount() const { return static_cast<int>(m_vehicles.size()); }

	/// @brief 指定ノードの信号機を取得する（なければ nullptr）
	const TrafficLight* getTrafficLight(int nodeId) const;

	/// @brief バス停を追加し、割り当てた id を返す
	int addBusStop(BusStop stop);

	/// @brief バス路線を追加し、割り当てた id を返す
	int addBusRoute(BusRoute route);

	/// @brief 路線にバス停を追加する
	void addStopToRoute(int routeId, int stopId);

	/// @brief バス停一覧を返す（読み取り専用）
	const Array<BusStop>& busStops() const { return m_busStops; }

	/// @brief バス路線一覧を返す（読み取り専用）
	const Array<BusRoute>& busRoutes() const { return m_busRoutes; }

private:
	static constexpr int   kReroutePerFrame      = 10;     ///< 毎フレームの最大再探索台数
	static constexpr float kPeriodicRerouteInterval = 60.0f; ///< 定期再探索間隔 [ゲーム秒]
	static constexpr float kSignalStopDist        = 15.0f; ///< 信号停止線手前の検出距離 [m]

	std::shared_ptr<const SimGraph> m_simGraph;
	Array<Vehicle> m_vehicles;
	int            m_nextId     = 0;
	GameTime       m_lastGameNow = 0.0;

	// 経路探索グラフ
	PathfindingGraph m_graph;
	bool             m_graphDirty = true;

	// 信号機（nodeId → TrafficLight）
	HashTable<int, TrafficLight> m_trafficLights;

	// バス路線・バス停
	Array<BusStop>  m_busStops;
	Array<BusRoute> m_busRoutes;
	int             m_nextStopId  = 0;
	int             m_nextRouteId = 0;

	// 再探索キュー（vehicle id リスト）
	Array<int> m_rerouteQueue;

	// --- グラフ管理 ---
	void rebuildGraph(GameTime now);

	// --- 車線変更 ---

	/// @brief 隣接車線への車線変更を試みる（キープレフト優先、安全ギャップ確認）
	void tryLaneChange(Vehicle& v);

	// --- 車両更新 ---
	void updateVehicle(Vehicle& v, double dt, GameTime gameNow);
	void advanceOnEdge(Vehicle& v, double dt, GameTime gameNow);
	bool transitToNextEdge(Vehicle& v, GameTime gameNow);

	// --- IDM ---

	/// @brief IDM 加速度を計算する [m/s²]
	/// @param fwdLane 進行方向（true=Forward, false=Backward）
	float idmAcceleration(const Vehicle& v, const IDMParams& params, bool fwdLane) const;

	// --- 経路探索 ---

	/// @brief 車両の経路を（再）計算してセットする
	void doReroute(Vehicle& v, GameTime now);

	/// @brief 再探索キューを毎フレーム kReroutePerFrame 台ずつ処理する
	void processRerouteQueue(GameTime now);

	/// @brief 再探索キューへ追加する（重複チェック付き）
	void enqueueReroute(int vehicleId);

	// --- 信号機 ---

	/// @brief ノードに信号機を自動生成する（3本以上のエッジがある交差点）
	void buildTrafficLights();

	/// @brief 全信号機を更新する
	void updateTrafficLights(GameTime gameNow);

	/// @brief バス路線に従いバスを生成・更新する
	void updateBusRoutes(GameTime gameNow);

	/// @brief バス停での停車処理を行う（Bus 車種専用）
	void updateBusStop(Vehicle& v, double dt, GameTime gameNow);
};
