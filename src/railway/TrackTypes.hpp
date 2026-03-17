#pragma once
#include "../time/GameClock.hpp"

/// @brief 線路ノード種別
enum class TrackNodeType : uint8
{
	Joint,    ///< 接続点
	Station,  ///< 駅
	Signal,   ///< 信号所
	Buffer,   ///< 車止め（端点）
};

/// @brief 線路ノード（交差点・駅・端点）
struct TrackNode
{
	int           id       = -1;
	Vec3          position;
	TrackNodeType type     = TrackNodeType::Joint;
	Array<int>    edgeIds;          ///< 接続する TrackEdge id リスト
	String        name;             ///< 駅名（type==Station の場合）

	/// @brief 有効なノードか
	bool isValid() const { return id >= 0; }
};

/// @brief 線路エッジ（ベジェ曲線）
struct TrackEdge
{
	int       id      = -1;
	int       nodeA   = -1;
	int       nodeB   = -1;
	Vec3      ctrlA;            ///< ベジェ制御点 A
	Vec3      ctrlB;            ///< ベジェ制御点 B
	float     length  = 0.0f;  ///< 弧長 [m]
	float     speedLimit = 130.0f;  ///< 制限速度 [km/h]（新幹線 = 200+）
	bool      electrified = true;   ///< 電化区間か
	int       occupiedBy = -1;  ///< 占有中の Train id（閉塞制御）

	/// @brief 有効なエッジか
	bool isValid() const { return id >= 0; }
};

/// @brief 駅停車情報（ダイヤ）
struct StopEntry
{
	int       stationNodeId = -1; ///< 駅 TrackNode id
	float     dwellSec      = 30.0f; ///< 停車時間 [ゲーム秒]
	float     arrivalOffset = 0.0f;  ///< 始発からの到着時刻オフセット [ゲーム秒]
};

/// @brief 運行ダイヤ
struct TrainSchedule
{
	int           id       = -1;
	Array<StopEntry> stops;   ///< 停車駅リスト（順序通り）
	float         headwaySec = 600.0f;  ///< 運行間隔 [ゲーム秒]
	bool          loop       = true;    ///< 折り返し運転か
	GameTime      lastSpawnAt = -9999.0;  ///< 最後にスポーンした時刻（初回即時スポーンのため大負数）
};
