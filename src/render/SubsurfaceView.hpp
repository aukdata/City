#pragma once
#include "RailFacilities.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 地形で隠れている部分だけを描く断面表示。実際の交通グラフと座標は変更しない。
class SubsurfaceView
{
public:
	struct Hit
	{
		Vec3 position;
		Optional<int> edge, station;
		double distance = 0;
	};
	static constexpr double kCover = .5;
	static constexpr double kEdgeSampleLength = 32;
	static constexpr double kDrawDistance = 60000;
	static constexpr double kStationPickRadius = 100;
	static bool below(Vec3 point, const World& world);
	static MeshData clip(const MeshData& source, const World& world, bool underground = true);
	static Optional<double> hitDistance(const MeshData& data, const Ray& ray);
	void invalidate() { m_dirty = true; }
	void prepare(const World& world, const RoadNetwork& roads, const TrainNetwork& railway);
	void draw(Vec3 eye) const;
	void drawSelection(int id, bool station, ColorF color) const;
	Optional<Hit> hit(const Ray& ray) const;
	bool containsEdge(int id) const { return m_edges.contains(id); }
	/// @brief 地上駅は駅舎、地下駅は入口を含む実メッシュから選択する。
	static Optional<Hit> stationHit(const World& world, const TrainNetwork& railway, const Ray& ray, bool underground);
	static Array<MeshData> stationGeometry(const World& world, const TrainNetwork& railway, int id, bool underground);

private:
	struct Part
	{
		MeshData data;
		Mesh mesh;
		ColorF color;
	};
	struct Item
	{
		Vec3 center;
		double radius = 0;
		Array<Part> parts;
	};
	static void add(Item& item, MeshData data, ColorF color);
	HashTable<int, Item> m_edges, m_stations;
	bool m_dirty = true;
	size_t m_edgeCount = 0, m_stationCount = 0;
};
