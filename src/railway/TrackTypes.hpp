#pragma once
#include "Train.hpp"
#include "../road/RoadTypes.hpp"

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

/// @brief 共通路盤と軌道車線を持つエッジ。形状の正本は RoadNetwork。
// 形状・路盤・車線を鉄道側へ複製しない。RoadNetwork の同じエッジを参照する。
using TrackEdge = RoadEdge;

/// @brief 駅に接続する2本の留置線。独立した車両在庫管理は持たない。
struct RailDepot
{
	int stationNodeId = -1;
	int throatNodeId = -1;
	Array<int> sidingNodes;
	String name;
};

/// @brief 運行ダイヤ
struct TrainSchedule
{
	int           id       = -1;
	String        name;
	bool          enabled = true;
	int           firstDepartureMinute = 8 * 60; ///< ゲーム内の始発時刻（0〜1439分）
	int           lastDepartureMinute = 23 * 60; ///< 終発。始発より前なら翌日。
	Array<StopEntry> stops;   ///< 停車駅リスト（順序通り）
	float         headwaySec = 600.0f;  ///< 運行間隔 [ゲーム秒]
	TrainType     type = TrainType::Local;
	bool          reverseNext = false; ///< 次便は停車駅列の逆順を走る
	GameTime      lastSpawnAt = -9999.0;  ///< 最後にスポーンした時刻（初回即時スポーンのため大負数）
};
