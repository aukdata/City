#pragma once
#include "RoadTypes.hpp"
#include "BezierUtil.hpp"

/// @brief 道路グラフ管理クラス
/// @details エッジ・ノードの追加/削除・交差検出を担う
class RoadNetwork
{
public:
	/// @brief ノードを追加し、割り当てた id を返す
	int addNode(Vec3 pos, NodeType type = NodeType::Endpoint);

	/// @brief エッジを追加し、割り当てた id を返す
	/// @param nodeA    始点ノード id
	/// @param nodeB    終点ノード id
	/// @param ctrlA    ベジェ制御点A（始点側）
	/// @param ctrlB    ベジェ制御点B（終点側）
	/// @param numLanes 総車線数（双方向合計）
	int addEdge(int nodeA, int nodeB,
	            Vec3 ctrlA, Vec3 ctrlB,
	            RoadType rt   = RoadType::LocalRoad,
	            int numLanes  = 2);

	/// @brief エッジを削除する（id を -1 にマーク）
	void removeEdge(int edgeId);

	/// @brief ノードを削除する（id を -1 にマーク）
	void removeNode(int nodeId);

	/// @brief id でエッジを取得する（存在しなければ nullptr）
	RoadEdge*       getEdge(int id);
	const RoadEdge* getEdge(int id) const;

	/// @brief id でノードを取得する（存在しなければ nullptr）
	RoadNode*       getNode(int id);
	const RoadNode* getNode(int id) const;

	const Array<RoadEdge>& edges() const { return m_edges; }
	const Array<RoadNode>& nodes() const { return m_nodes; }

	/// @brief 指定位置に近いノードを探す
	/// @param radius 探索半径 [m]
	Optional<int> findNodeNear(Vec3 pos, float radius = 10.0f) const;

	/// @brief 既存エッジとの交差を処理しながらエッジを追加する
	/// @return 追加されたエッジの id
	int addEdgeWithIntersection(int nodeA, int nodeB,
	                            Vec3 ctrlA, Vec3 ctrlB,
	                            RoadType rt, int numLanes);

	/// @brief ベジェ曲線を取得する（エッジ id が有効なら）
	Optional<CubicBezier> getBezier(int edgeId) const;

private:
	Array<RoadEdge> m_edges;
	Array<RoadNode> m_nodes;
	int m_nextEdgeId = 0;
	int m_nextNodeId = 0;

	/// @brief エッジ id からインデックスを返す（-1 なら存在しない）
	int edgeIndex(int id) const;
	int nodeIndex(int id) const;

	/// @brief デフォルトの車線セットを生成する
	static Array<Lane> buildDefaultLanes(int numLanes, RoadType rt);
};
