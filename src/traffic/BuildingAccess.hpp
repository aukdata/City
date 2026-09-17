#pragma once
#include "TrafficSpawn.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 建物が接する道路の、建物側の端の走行車線上に置く発着点。
struct BuildingAccessPoint
{
	int64 buildingKey;
	int edgeId, lane;
	float arc;
	Vec2 position;
	BuildingType type;
};

/// @brief 全建物の発着点索引。初回以外はチャンクを巡回して開発・撤去を反映する。
class BuildingAccessIndex
{
public:
	void rebuild(const World& world, const RoadNetwork& roads, const SimGraph& graph);
	void refresh(const World& world, const RoadNetwork& roads, const SimGraph& graph);
	[[nodiscard]] const HashTable<int, Array<BuildingAccessPoint>>& edges() const { return m_edges; }
	[[nodiscard]] const Array<BuildingAccessPoint>& onEdge(int edge) const;
	[[nodiscard]] size_t size() const { return m_size; }
	/// @brief 件数が同じ敷地の入れ替えも検知する更新番号。
	uint64 revision() const { return m_revision; }
	/// @brief 開発・道路形状変更の検証にも使用する純粋な接道計算。
	static Optional<BuildingAccessPoint> project(const Chunk& chunk, Point cell,
		const RoadNetwork& roads, const SimGraph& graph);
private:
	HashTable<int, Array<BuildingAccessPoint>> m_edges;
	std::array<Array<int>, WORLD_CHUNKS * WORLD_CHUNKS> m_chunkEdges;
	std::array<uint64, WORLD_CHUNKS * WORLD_CHUNKS> m_fingerprints{};
	size_t m_size = 0;
	uint64 m_revision = 0;
	int m_cursor = 0;
	void updateChunk(int index, const World& world, const RoadNetwork& roads, const SimGraph& graph, bool force);
};
