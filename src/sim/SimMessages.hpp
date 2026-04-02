#pragma once
#include <Siv3D.hpp>
#include <variant>
#include <memory>
#include "../traffic/Vehicle.hpp"
#include "../debug/PerfStats.hpp"

struct SimGraph;   // 前方宣言

// RouteWaypoint は Vehicle.hpp で定義済み

// ═══════════════════════════════════════════════════════════════════
// Main → Sim リクエスト
// ═══════════════════════════════════════════════════════════════════

struct RouteRequest
{
	int vehicleId  = -1;
	int startEdge  = -1;
	int startLane  = 0;
	int goalEdge   = -1;
};

struct NetworkUpdate
{
	std::shared_ptr<const SimGraph> graph;
};

using SimRequest = std::variant<RouteRequest, NetworkUpdate>;

// ═══════════════════════════════════════════════════════════════════
// Sim → Main レスポンス
// ═══════════════════════════════════════════════════════════════════

struct RouteResponse
{
	int vehicleId = -1;
	Array<RouteWaypoint> waypoints;
	bool found = false;
};

struct PerfUpdate
{
	SimTickStats stats;
};

using SimResponse = std::variant<RouteResponse, PerfUpdate>;
