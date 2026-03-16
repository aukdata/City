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
	WaitingBusStop,
	YieldingEmergency,
	Parking,
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
	Vec3         position;           ///< ワールド 3D 座標（描画用）
	float        heading     = 0.0f; ///< 進行方向 [rad]（Y軸周り）

	// AI 状態
	VehicleState state       = VehicleState::Moving;
	int          leadVehicle = -1;   ///< 前方車両 id（-1=なし）
	float        gapToLead   = 1e9f; ///< 前方車両との車頭距離 [m]

	// 緊急車両専用
	bool         sirenActive = false;

	// 経路（ダイクストラ結果）
	Array<int>   routeNodeIds;              ///< LaneNode / BorderNode の ID 列
	int          routeProgress  = 0;        ///< 消化済みノード数

	// 再探索管理
	GameTime     lastReroute    = 0.0;
	float        rerouteSpeedThreshold = 3.0f;  ///< この速度以下で再探索を検討 [m/s]

	// ライフサイクル
	GameTime     departedAt  = 0.0;
	int          goalEdgeId  = -1;   ///< 目的地エッジ id
};
