#pragma once
#include "../time/GameClock.hpp"

/// @brief 車両種別
enum class VehicleType : uint8
{
	PassengerCar,  ///< 普通乗用車
	KeiCar,        ///< 軽自動車
	Moped,         ///< 原動機付自転車
	LightVehicle,  ///< 軽車両（自転車等）
	Bus,           ///< 路線バス
	SmallTruck,    ///< 小型トラック
	LargeTruck,    ///< 大型トラック
	Emergency,     ///< 緊急車両
};

/// @brief 車両の動作状態
enum class VehicleState : uint8
{
	Moving,
	WaitingSignal,
	WaitingStopSign,   ///< 一時停止中（タイマー消化待ち）
	WaitingBusStop,
	YieldingEmergency,
	Parking,
};

/// @brief 車両の描画/更新モード
enum class VehicleMode : uint8
{
	Active,   ///< 画面内: IDM + bezier 位置計算
	Dormant,  ///< 画面外: タイマーベースのエッジ遷移のみ
};

/// @brief 車両の位置状態
enum class VehicleLocation : uint8
{
	OnLane,        ///< エッジの車線上
	OnConnection,  ///< 交差点の旋回パス上
	ChangingLane,  ///< 車線変更中（2車線間をブレンド）
};

/// @brief IDM（Intelligent Driver Model）パラメータ
struct IDMParams
{
	float aMax = 2.5f;  ///< 最大加速度 [m/s²]
	float b    = 3.0f;  ///< 快適減速度 [m/s²]
	float T    = 1.2f;  ///< 車頭時間 [s]
	float s0   = 2.0f;  ///< 最小車間距離 [m]
	float v0   = 16.7f; ///< 目標速度 [m/s]（≈ 60km/h）
};

/// @brief 車種ごとのデフォルト IDM パラメータを返す
inline IDMParams getDefaultIDMParams(VehicleType type, float speedLimitKmh = 60.0f)
{
	IDMParams p;
	p.v0 = speedLimitKmh / 3.6f;
	switch (type)
	{
	case VehicleType::PassengerCar:
		p.aMax = 2.5f; p.b = 3.0f; p.T = 1.2f; p.s0 = 2.0f; break;
	case VehicleType::KeiCar:
		p.aMax = 2.8f; p.b = 3.2f; p.T = 1.1f; p.s0 = 1.8f; break;
	case VehicleType::Moped:
		p.aMax = 2.0f; p.b = 2.5f; p.T = 1.5f; p.s0 = 1.0f; break;
	case VehicleType::LightVehicle:
		p.aMax = 0.8f; p.b = 1.5f; p.T = 2.0f; p.s0 = 0.8f; break;
	case VehicleType::Bus:
		p.aMax = 1.0f; p.b = 1.5f; p.T = 2.0f; p.s0 = 5.0f; break;
	case VehicleType::SmallTruck:
		p.aMax = 1.8f; p.b = 2.5f; p.T = 1.5f; p.s0 = 3.0f; break;
	case VehicleType::LargeTruck:
		p.aMax = 0.8f; p.b = 1.2f; p.T = 2.5f; p.s0 = 6.0f; break;
	case VehicleType::Emergency:
		p.aMax = 3.5f; p.b = 4.0f; p.T = 0.8f; p.s0 = 1.5f; break;
	}
	return p;
}

/// @brief 経路上の1エッジ分の情報
struct RouteWaypoint
{
	int   edgeId          = -1;
	int   laneIndex       = 0;
	float entryArcPos     = 0.0f;   ///< このエッジに入る弧長位置
	float edgeLength      = 0.0f;   ///< エッジ全長 [m]
	float estimatedTimeSec = 1.0f;  ///< 推定通過時間 [game sec]
};

/// @brief 車両エージェント
struct Vehicle
{
	int          id          = -1;
	VehicleType  type        = VehicleType::PassengerCar;

	// 経路状態
	int          currentEdge = -1;   ///< 現在の RoadEdge id
	int          currentLane = 0;    ///< 現在の車線インデックス
	float        arcPos      = 0.0f; ///< エッジ上の弧長位置 [m]

	// 物理状態
	float        speed       = 0.0f; ///< 現在速度 [m/s]
	Vec3         position;           ///< ワールド 3D 座標（描画用、Active 時のみ有効）
	float        heading     = 0.0f; ///< 進行方向 [rad]（Y軸周り）
	float        pitch       = 0.0f; ///< 傾斜角 [rad]（道路勾配追従）

	// AI 状態
	VehicleState state       = VehicleState::Moving;

	// 経路（RouteResponse で受け取ったウェイポイント列）
	Array<RouteWaypoint> routeWaypoints;
	int          routeIdx       = 0;     ///< 次に使うウェイポイントのインデックス
	int          goalEdgeId     = -1;    ///< 目的地エッジ id
	bool         routeRequested = false; ///< RouteRequest 送信済みフラグ
	int          routeFailCount = 0;     ///< 経路探索連続失敗回数

	// Active / Dormant モード
	VehicleMode  mode             = VehicleMode::Active;
	float        dormantTimer     = 0.0f;  ///< 現在エッジの残り通過時間 [game sec]
	float        dormantTotalTime = 0.0f;  ///< 現在エッジの推定全通過時間 [game sec]

	// 位置状態
	VehicleLocation location     = VehicleLocation::OnLane;
	int          connectionNodeId = -1;   ///< OnConnection 時の交差点ノード ID
	int          connectionId    = -1;    ///< OnConnection 時の LaneConnection ID

	// 車線変更
	int          laneFrom        = -1;    ///< ChangingLane 時の元車線
	int          laneTo          = -1;    ///< ChangingLane 時の目標車線
	float        laneChangeBlend = 0.0f;  ///< ChangingLane 時のブレンド [0,1]

	// 一時停止
	float        stopSignWait    = 0.0f;  ///< 一時停止の残り待機時間 [game sec]

	// バス専用フィールド
	int    busRouteId       = -1;
	int    busNextStopIdx   = 0;
	float  busWaitRemaining = 0.0f;
};
