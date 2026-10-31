#pragma once
#include "TrafficSpawn.hpp"
#include "../gen/GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/Chunk.hpp"

/// @file
/// @brief 外部接続: 地図端で途切れる道路（一般道・高速道路の本線）を地図外の街との出入口として扱う。
/// @details 端点は地図端から traffic_outsideBorderDistance 以内にあり、接続エッジが 1 本だけの節点とする。
///   保存データは持たず、道路網から毎回求める。プレイヤーが地図端まで延ばした道路も同じ扱いになる。
namespace OutsideConnections
{
	/// @brief 1 つの出入口。流入車線は地図内へ向かう向き、流出車線は地図外へ向かう向き。
	struct Connection
	{
		int nodeId = -1;              ///< 地図端の節点
		int edgeId = -1;              ///< 端点に接続するエッジ
		Vec3 position;
		RoadType roadType = RoadType::LocalRoad;
		Array<int> inboundLanes;      ///< 地図外 → 地図内
		Array<int> outboundLanes;     ///< 地図内 → 地図外
		bool atNodeA = true;          ///< 端点がエッジの nodeA 側か
		double weight = 1;            ///< 交通量の配分比（車線数×道路種別）
	};

	/// @brief 地図端からの距離。
	[[nodiscard]] inline double borderDistance(Vec3 position)
	{
		return Min({ static_cast<double>(position.x), static_cast<double>(position.z),
			WORLD_SIZE - static_cast<double>(position.x), WORLD_SIZE - static_cast<double>(position.z) });
	}

	/// @brief 道路種別ごとの交通量の重み。高速道路は通過・広域交通を多く受け持つ。
	[[nodiscard]] inline double roadWeight(RoadType type)
	{
		switch (type)
		{
		case RoadType::Expressway: return GenerationSettings::get().traffic_outsideExpresswayWeight;
		case RoadType::Arterial: return 1.0;
		default: return .35;
		}
	}

	/// @brief 道路網と走行グラフから外部接続を集める。
	/// @brief find の除外理由の集計（診断用）。
	struct Diagnostics { int nearBorder = 0, branched = 0, missingEdge = 0, unbuilt = 0, short_ = 0, closed = 0; };

	[[nodiscard]] inline Array<Connection> find(const RoadNetwork& network, const SimGraph& graph, Diagnostics* diagnostics = nullptr)
	{
		Array<Connection> result;
		Diagnostics local;
		Diagnostics& d = diagnostics ? *diagnostics : local;
		const double limit = GenerationSettings::get().traffic_outsideBorderDistance;
		for (const auto& node : network.nodes())
		{
			if (node.id < 0 || borderDistance(node.position) > limit) { continue; }
			++d.nearBorder;
			if (node.attachments.size() != 1) { ++d.branched; continue; }
			const int edgeId = node.attachments.front().edgeId;
			const auto* edge = graph.getEdge(edgeId);
			if (!edge || (edge->nodeA != node.id && edge->nodeB != node.id)) { ++d.missingEdge; continue; }
			if (!edge->isRoadbedBuilt()) { ++d.unbuilt; continue; }
			if (edge->length < 30) { ++d.short_; continue; }
			Connection connection;
			connection.nodeId = node.id; connection.edgeId = edgeId; connection.position = node.position;
			connection.roadType = edge->roadType; connection.atNodeA = edge->nodeA == node.id;
			for (int lane = 0; lane < static_cast<int>(edge->lanes.size()); ++lane)
			{
				if (!TrafficSpawn::laneOpen(*edge, lane)) { continue; }
				// nodeA が地図端なら、A→B（順方向）の車線が地図内へ入る。
				const bool inward = TrafficCommon::isForwardLane(*edge, lane) == connection.atNodeA;
				(inward ? connection.inboundLanes : connection.outboundLanes) << lane;
			}
			if (connection.inboundLanes.isEmpty() && connection.outboundLanes.isEmpty()) { ++d.closed; continue; }
			connection.weight = roadWeight(edge->roadType) * Max<size_t>(connection.inboundLanes.size(), connection.outboundLanes.size());
			result << std::move(connection);
		}
		return result;
	}

	/// @brief 重みに比例して、指定した向きの車線を持つ出入口を選ぶ。なければ none。
	[[nodiscard]] inline Optional<size_t> choose(const Array<Connection>& connections, bool inbound)
	{
		double total = 0;
		for (const auto& connection : connections) { total += (inbound ? connection.inboundLanes : connection.outboundLanes).isEmpty() ? 0.0 : connection.weight; }
		if (total <= 0) { return none; }
		double pick = Random(0.0, total);
		for (size_t i = 0; i < connections.size(); ++i)
		{
			if ((inbound ? connections[i].inboundLanes : connections[i].outboundLanes).isEmpty()) { continue; }
			pick -= connections[i].weight;
			if (pick <= 0) { return i; }
		}
		return none;
	}
}
