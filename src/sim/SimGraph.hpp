#pragma once
#include "../road/RoadTypes.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief シミュレーション用の軽量道路グラフ（位置情報を持たない）
/// @details RoadNetwork からエッジ長・速度制限・車線構造のみを抽出する。
///   バックグラウンドの Sim スレッドが経路探索・車両物理に使用する。
///   メインスレッドで RoadNetwork を編集した後、rebuild() で再構築し shared_ptr でスワップする。
struct SimGraph
{
	/// @brief エッジのシミュレーション用データ
	struct Edge
	{
		int      id       = -1;
		int      nodeA    = -1;
		int      nodeB    = -1;
		float    length   = 0.0f;      ///< 弧長 [m]
		float    speedLimit = 60.0f;   ///< 制限速度 [km/h]
		float    congestion = 0.0f;    ///< 渋滞度 [0,1]
		RoadType roadType = RoadType::LocalRoad;
		Array<RoadPart> parts;         ///< 道路部品（走行可否判定に使用）
		Array<Lane> lanes;             ///< 車線構造（走行可否・方向判定に使用）
		float    tangentAngleA = 0.0f; ///< nodeA 端のベジェ接線角 [rad] (atan2(tz,tx))
		float    tangentAngleB = 0.0f; ///< nodeB 端のベジェ接線角 [rad]

		/// @brief 路盤パーツが建設済みかどうか
		[[nodiscard]] bool isRoadbedBuilt() const
		{
			for (const auto& p : parts)
				if (p.type == RoadPartType::Roadbed && p.build == BuildState::Built) return true;
			return false;
		}
	};

	/// @brief ノードのシミュレーション用データ
	struct Node
	{
		int        id = -1;
		Array<int> edgeIds;   ///< 接続エッジ ID リスト
		HashTable<int, TrafficControl> edgeControl;  ///< edgeId → 交通規制
	};

	HashTable<int, Edge> edges;
	HashTable<int, Node> nodes;

	/// @brief RoadNetwork から SimGraph を構築する
	static SimGraph build(const RoadNetwork& network)
	{
		SimGraph g;

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			Edge se;
			se.id         = edge.id;
			se.nodeA      = edge.nodeA;
			se.nodeB      = edge.nodeB;
			se.length     = edge.length;
			se.speedLimit = edge.speedLimit;
			se.congestion = edge.congestion;
			se.roadType   = edge.roadType;
			se.parts      = edge.parts;
			se.lanes      = edge.lanes;

			// ベジェ接線角を計算（ターン判定用）
			if (const auto bez = network.getBezier(edge.id))
			{
				const Vec3 tA = bez->tangentAt(0.0f);
				const Vec3 tB = bez->tangentAt(bez->totalLength);
				se.tangentAngleA = static_cast<float>(Math::Atan2(tA.x, tA.z));
				se.tangentAngleB = static_cast<float>(Math::Atan2(tB.x, tB.z));
			}
			g.edges[edge.id] = std::move(se);
		}

		for (const auto& node : network.nodes())
		{
			if (node.id < 0) continue;
			Node sn;
			sn.id      = node.id;
			sn.edgeIds = node.edgeIds();
			for (const auto& att : node.attachments)
				sn.edgeControl[att.edgeId] = att.control;
			g.nodes[node.id] = std::move(sn);
		}

		return g;
	}

	/// @brief 全エッジ ID リストを返す
	Array<int> edgeIds() const
	{
		Array<int> ids;
		for (const auto& [id, e] : edges) ids << id;
		return ids;
	}

	/// @brief エッジを取得する（存在しなければ nullptr）
	const Edge* getEdge(int id) const
	{
		const auto it = edges.find(id);
		return (it != edges.end()) ? &it->second : nullptr;
	}

	/// @brief ノードを取得する（存在しなければ nullptr）
	const Node* getNode(int id) const
	{
		const auto it = nodes.find(id);
		return (it != nodes.end()) ? &it->second : nullptr;
	}
};
