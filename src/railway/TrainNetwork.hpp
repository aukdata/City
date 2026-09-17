#pragma once
#include "TrackTypes.hpp"
#include "../road/RoadNetwork.hpp"
#include "../road/TransportCrossSection.hpp"

/// @brief 鉄道線路グラフ管理クラス
class TrainNetwork
{
public:
	// 形状と断面は共通道路網、駅・ダイヤ・車庫は運行側に置く。
	/// @brief 線路ノードを追加し、id を返す
	int addNode(Vec3 pos, TrackNodeType type = TrackNodeType::Joint,
	            const String& name = U"");

	/// @brief 線路エッジを追加し、id を返す
	int addEdge(int nodeA, int nodeB,
	            Vec3 ctrlA, Vec3 ctrlB,
	            float speedLimit = 130.0f, bool doubleTrack = false);

	/// @brief 駅を追加し、TrackNode の id を返す
	int addStation(Vec3 pos, const String& name);

	/// @brief id でノードを取得する（存在しなければ nullptr）
	TrackNode*       getNode(int id);
	const TrackNode* getNode(int id) const;

	/// @brief id でエッジを取得する（存在しなければ nullptr）
	TrackEdge*       getEdge(int id);
	const TrackEdge* getEdge(int id) const;

	/// @brief 共通道路網へ接続する。未接続の単体テストでは内部の RoadNetwork を使う。
	void bind(RoadNetwork* roads) { m_sharedRoads = roads; synchronize(); }
	RoadNetwork& infrastructure() { return m_sharedRoads ? *m_sharedRoads : m_ownedRoads; }
	const RoadNetwork& infrastructure() const { return m_sharedRoads ? *m_sharedRoads : m_ownedRoads; }
	bool sharedInfrastructure() const { return m_sharedRoads != nullptr; }
	/// @brief 断面編集・道路分割後に、運行側の参照を共通網から更新する。
	void synchronize();
	const Array<TrackNode>& nodes() const { return m_nodes; }

	/// @brief 軌道を持つエッジだけを参照する。形状や断面のコピーは作らない。
	class EdgeView
	{
	public:
		struct Iterator
		{
			const TrainNetwork* owner; size_t index;
			const TrackEdge& operator*() const { return *owner->getEdge(owner->m_edgeIds[index]); }
			Iterator& operator++() { ++index; return *this; }
			bool operator!=(const Iterator& other) const { return index != other.index; }
		};
		const TrainNetwork* owner;
		Iterator begin() const { return {owner,0}; }
		Iterator end() const { return {owner,owner->m_edgeIds.size()}; }
		size_t size() const { return owner->m_edgeIds.size(); }
		bool isEmpty() const { return owner->m_edgeIds.isEmpty(); }
		const TrackEdge& operator[](size_t index) const { return *owner->getEdge(owner->m_edgeIds[index]); }
		const TrackEdge& front() const { return (*this)[0]; }
		const TrackEdge& back() const { return (*this)[size()-1]; }
	};
	EdgeView edges() const { return {this}; }

	/// @brief ベジェ曲線を取得する
	Optional<CubicBezier> getBezier(int edgeId) const;

	/// @brief 接続された中間区間を含む最短経路。逆方向のエッジも通行できる。
	Array<int> findRoute(int from,int to) const;

	/// @brief 閉塞区間を占有する（失敗なら false）
	bool tryOccupy(int edgeId, int trainId, bool forward = true);
	bool canOccupy(int edgeId, int trainId, bool forward) const;

	/// @brief 閉塞区間を解放する
	void releaseOccupy(int edgeId, int trainId);

	/// @brief ダイヤを追加する
	int addSchedule(TrainSchedule schedule);
	TrainSchedule* getSchedule(int id);
	const TrainSchedule* getSchedule(int id) const;
	/// @brief 検証済みの設定だけ置換し、運行中の状態を保持する。
	bool applySchedule(const TrainSchedule& schedule, String& error);
	Array<RailDepot>& depots() { return m_depots; }
	const Array<RailDepot>& depots() const { return m_depots; }
	JSON saveState() const;
	bool restoreState(const JSON& state);
	const Array<TrainSchedule>& schedules() const { return m_schedules; }
	Array<TrainSchedule>&       schedules()       { return m_schedules; }

private:
	Array<TrackNode>     m_nodes;
	RoadNetwork m_ownedRoads;
	RoadNetwork* m_sharedRoads = nullptr;
	Array<int> m_edgeIds;
	HashTable<int, int> m_nodeIndex;
	Array<TrainSchedule> m_schedules;
	Array<RailDepot> m_depots;


	int nodeIndex(int id) const;
	void registerNode(int id);
};
