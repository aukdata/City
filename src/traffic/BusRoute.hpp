#pragma once
#include "../time/GameClock.hpp"

/// @brief バス停
struct BusStop
{
	int    id       = -1;
	Vec3   position;         ///< ワールド座標（y=0 の地面上）
	int    edgeId   = -1;    ///< 所属する RoadEdge id（-1 = 未割り当て）
	float  arcPos   = 0.0f;  ///< エッジ上の弧長位置 [m]
	String name;             ///< 停留所名
};

/// @brief バス路線
struct BusRoute
{
	int          id        = -1;
	Array<int>   stopIds;          ///< BusStop id の順序列
	float        headwaySec = 120.0f;  ///< 運行間隔 [ゲーム秒]
	GameTime     lastSpawnAt = 0.0;    ///< 最後にバスを生成した時刻
};
