#pragma once
#include "RoadNetwork.hpp"

/// @brief 道路計画の点列と履歴。プレビューは既存の道路網から分離する。
class RoadPlanDraft
{
public:
	static constexpr double kMinimumSegment = 2.0;
	bool place(Vec3 point, bool replaceEnd = false);
	bool undo();
	bool redo();
	void clear();
	bool rebuild(const World& world, const RoadEdge& roadTemplate, bool followTerrain);
	/// @brief 成功した計画全体だけを反映する。失敗時は道路網を変更しない。
	Array<int> apply(RoadNetwork& network, const World& world, const RoadEdge& roadTemplate, bool followTerrain) const;
	const Array<Vec3>& points() const { return m_points; }
	const RoadNetwork& preview() const { return m_preview; }
	bool canUndo() const { return !m_undo.isEmpty(); }
	bool canRedo() const { return !m_redo.isEmpty(); }
	bool valid() const { return !m_previewEdges.isEmpty(); }
	double length() const;
	static Vec3 constrainAngle(Vec3 origin, Vec3 cursor);
	static RoadEdge makeRoadTemplate(int preset);
private:
	Array<Vec3> m_points;
	Array<Array<Vec3>> m_undo;
	Array<Array<Vec3>> m_redo;
	RoadNetwork m_preview;
	Array<int> m_previewEdges;
};

/// @brief 接続候補の空間索引。マウス移動では近傍セルの道路だけを調べる。
class RoadPlanSnapIndex
{
public:
	struct Hit
	{
		Vec3 position;
		bool connected = false;
		bool node = false;
	};
	void rebuild(const RoadNetwork& network);
	Hit find(const RoadNetwork& network, Vec3 cursor, double radius = 12.0) const;
private:
	static constexpr double kCellSize = 128.0;
	HashTable<Point, Array<int>> m_nodes;
	HashTable<Point, Array<int>> m_edges;
};
