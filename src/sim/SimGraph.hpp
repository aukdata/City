#pragma once
#include "../road/RoadTypes.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief シミュレーション用の軽量道路グラフ（位置情報を持たない）
/// @details RoadNetwork からエッジ長・速度制限・車線構造のみを抽出する。
///   バックグラウンドの Sim スレッドが経路探索・車両物理に使用する。
///   メインスレッドで RoadNetwork を編集した後、rebuild() で再構築し shared_ptr でスワップする。
struct SimGraph
{
	// RoadNetwork から走行・探索に必要な最小情報だけを切り出し、SimThread で安全に共有できる形にする。
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
		EdgeState edgeState = EdgeState::Open;
		Array<RoadPart> parts;         ///< 道路部品（走行可否判定に使用）
		Array<Lane> lanes;             ///< 車線構造（走行可否・方向判定に使用）
		float    tangentAngleA = 0.0f; ///< nodeA 端のベジェ接線角 [rad] (atan2(tz,tx))
		float    tangentAngleB = 0.0f; ///< nodeB 端のベジェ接線角 [rad]

		/// @brief 路盤パーツが建設済みかどうか
		[[nodiscard]] bool isRoadbedBuilt() const
		{
			if (edgeState != EdgeState::Open && edgeState != EdgeState::Existing) return false;
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
		Array<LaneConnection> laneConnections;  ///< 交差点内の車線接続（旋回パス）
	};

	HashTable<int, Edge> edges;
	HashTable<int, Node> nodes;

	/// @brief RoadNetwork から SimGraph を構築する
	static SimGraph build(const RoadNetwork& network)
	{
		// 全件再構築ではエッジとノードを最初から作り直し、接線角も含めて完全なスナップショットを作る。
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
			se.edgeState  = edge.edgeState;
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
			sn.laneConnections = node.laneConnections;
			g.nodes[node.id] = std::move(sn);
		}

		return g;
	}

	/// @brief 指定ノード周辺のエッジ・ノードだけ差分更新する
	void updateAround(const Array<int>& dirtyNodeIds, const RoadNetwork& network)
	{
		// 局所編集時は影響ノードと隣接エッジだけ更新し、全体再構築より軽く同期を取り直す。
		// 変更ノードに接続するエッジを収集
		HashSet<int> edgeIds;
		HashSet<int> nodeIds;
		for (const int nid : dirtyNodeIds)
		{
			nodeIds.insert(nid);
			const RoadNode* rn = network.getNode(nid);
			if (rn)
			{
				for (const auto& att : rn->attachments)
				{
					edgeIds.insert(att.edgeId);
					const RoadEdge* re = network.getEdge(att.edgeId);
					if (re)
					{
						nodeIds.insert(re->nodeA);
						nodeIds.insert(re->nodeB);
					}
				}
			}
			else
			{
				// 削除されたノード
				nodes.erase(nid);
			}
		}

		// 古いエッジで、もう RoadNetwork に存在しないものを削除
		for (auto it = edges.begin(); it != edges.end(); )
		{
			if (edgeIds.contains(it->first) && !network.getEdge(it->first))
				it = edges.erase(it);
			else
				++it;
		}

		// エッジを更新
		for (const int eid : edgeIds)
		{
			const RoadEdge* re = network.getEdge(eid);
			if (!re) { edges.erase(eid); continue; }
			Edge se;
			se.id         = re->id;
			se.nodeA      = re->nodeA;
			se.nodeB      = re->nodeB;
			se.length     = re->length;
			se.speedLimit = re->speedLimit;
			se.congestion = re->congestion;
			se.roadType   = re->roadType;
			se.edgeState  = re->edgeState;
			se.parts      = re->parts;
			se.lanes      = re->lanes;
			if (const auto bez = network.getBezier(re->id))
			{
				se.tangentAngleA = static_cast<float>(Math::Atan2(bez->tangentAt(0.0f).x, bez->tangentAt(0.0f).z));
				se.tangentAngleB = static_cast<float>(Math::Atan2(bez->tangentAt(bez->totalLength).x, bez->tangentAt(bez->totalLength).z));
			}
			edges[eid] = std::move(se);
		}

		// ノードを更新
		for (const int nid : nodeIds)
		{
			const RoadNode* rn = network.getNode(nid);
			if (!rn) { nodes.erase(nid); continue; }
			Node sn;
			sn.id      = rn->id;
			sn.edgeIds = rn->edgeIds();
			for (const auto& att : rn->attachments)
				sn.edgeControl[att.edgeId] = att.control;
			sn.laneConnections = rn->laneConnections;
			nodes[nid] = std::move(sn);
		}
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
