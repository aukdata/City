#pragma once
#include "../time/GameClock.hpp"

/// @brief 列車種別
enum class TrainType : uint8
{
	Local,       ///< 普通列車
	Express,     ///< 急行列車
	LimitedExpress, ///< 特急列車
	Shinkansen,  ///< 新幹線
	Freight,     ///< 貨物列車
};

/// @brief 列車の動作状態
enum class TrainState : uint8
{
	Running,           ///< 走行中
	WaitingStation,    ///< 駅停車中
	WaitingSignal,     ///< 信号待ち（閉塞待ち）
	OutOfService,      ///< 回送・休止
};

/// @brief 列車エージェント
struct Train
{
	int         id          = -1;
	TrainType   type        = TrainType::Local;
	TrainState  state       = TrainState::Running;

	// 線路上の位置
	int         currentEdge = -1;   ///< 現在の TrackEdge id
	float       arcPos      = 0.0f; ///< エッジ上の弧長位置 [m]
	bool        forward     = true; ///< 進行方向（true = nodeA→nodeB）

	// 物理状態
	float       speed       = 0.0f;   ///< 現在速度 [m/s]
	Vec3        position;             ///< ワールド 3D 座標（描画用）
	float       heading     = 0.0f;   ///< 進行方向 [rad]

	// ダイヤ
	int         scheduleId  = -1;  ///< 所属 TrainSchedule id
	int         nextStopIdx = 0;   ///< 次の停車駅インデックス
	float       waitRemaining = 0.0f; ///< 駅停車の残り待機 [ゲーム秒]

	// 経路（TrackEdge id の列）
	Array<int>  routeEdges;
	int         routeProgress = 0;

	// ライフサイクル
	GameTime    departedAt = 0.0;
	int         scheduleIteration = 0;  ///< 何周目か（ループ運行の折り返し）
};
