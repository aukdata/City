#pragma once
#include "TrackTypes.hpp"
#include "../road/BezierUtil.hpp"

/// @brief 鉄道線路グラフ管理クラス
class TrainNetwork
{
public:
	/// @brief 線路ノードを追加し、id を返す
	int addNode(Vec3 pos, TrackNodeType type = TrackNodeType::Joint,
	            const String& name = U"");

	/// @brief 線路エッジを追加し、id を返す
	int addEdge(int nodeA, int nodeB,
	            Vec3 ctrlA, Vec3 ctrlB,
	            float speedLimit = 130.0f);

	/// @brief 駅を追加し、TrackNode の id を返す
	int addStation(Vec3 pos, const String& name);

	/// @brief id でノードを取得する（存在しなければ nullptr）
	TrackNode*       getNode(int id);
	const TrackNode* getNode(int id) const;

	/// @brief id でエッジを取得する（存在しなければ nullptr）
	TrackEdge*       getEdge(int id);
	const TrackEdge* getEdge(int id) const;

	const Array<TrackNode>& nodes() const { return m_nodes; }
	const Array<TrackEdge>& edges() const { return m_edges; }

	/// @brief ベジェ曲線を取得する
	Optional<CubicBezier> getBezier(int edgeId) const;

	/// @brief 閉塞区間を占有する（失敗なら false）
	bool tryOccupy(int edgeId, int trainId);

	/// @brief 閉塞区間を解放する
	void releaseOccupy(int edgeId, int trainId);

	/// @brief ダイヤを追加する
	void addSchedule(TrainSchedule schedule);
	const Array<TrainSchedule>& schedules() const { return m_schedules; }
	Array<TrainSchedule>&       schedules()       { return m_schedules; }

private:
	Array<TrackNode>     m_nodes;
	Array<TrackEdge>     m_edges;
	Array<TrainSchedule> m_schedules;
	int m_nextNodeId = 0;
	int m_nextEdgeId = 0;

	int nodeIndex(int id) const;
	int edgeIndex(int id) const;
};
