#pragma once
#include <Siv3D.hpp>
#include <variant>
#include <memory>
#include "../traffic/Vehicle.hpp"
#include "../debug/PerfStats.hpp"

struct SimGraph;   // 前方宣言

// RouteWaypoint は Vehicle.hpp で定義済み

// ═══════════════════════════════════════════════════════════════════
// Main から SimThread へ送る要求は、経路探索とグラフ差し替えの 2 系統に限定する。
// ═══════════════════════════════════════════════════════════════════

struct RouteRequest
{
	int vehicleId  = -1;
	int startEdge  = -1;
	int startLane  = 0;
	int goalEdge   = -1;
};

enum class NetworkChangeKind : uint8
{
	GenericEdit,
	MovedIntersectionNode,
};

struct NetworkChangeContext
{
	NetworkChangeKind kind = NetworkChangeKind::GenericEdit;
	Array<int> dirtyNodeIds;
	int movedNodeId = -1;
	Optional<Vec3> oldNodePos;
};

struct NetworkUpdate
{
	std::shared_ptr<const SimGraph> graph;
	NetworkChangeKind kind = NetworkChangeKind::GenericEdit;
	Array<int> dirtyNodeIds;
};

using SimRequest = std::variant<RouteRequest, NetworkUpdate>;

// ═══════════════════════════════════════════════════════════════════
// Sim から Main へ返す応答も、探索結果と計測結果の 2 種類へ揃える。
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
