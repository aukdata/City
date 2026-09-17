#pragma once
#include "../traffic/BuildingAccess.hpp"
#include "../railway/TrainNetwork.hpp"

/// @brief 徒歩の目的地。車道上の発着点と敷地の出入口を区別する。
struct PedestrianSite
{
	int64 key = -1;
	int node = -1;
	Vec3 entrance;
	BuildingAccessPoint vehicle;
	bool parking = false;
};
struct PedestrianStation
{
	int stationId = -1, node = -1;
	Vec3 platform;
};
/// @brief 変更時だけ作る歩道・路肩・横断歩道のグラフ。高速道路と専用線路を含めない。
class PedestrianNetwork
{
public:
	struct Node
	{
		Vec3 position;
		Array<int> links;
		int component = -1;
	};
	struct Link
	{
		int a = -1, b = -1, roadShape = -1, crossingNode = -1, crossingEdge = -1;
		float arcA = 0, arcB = 0, length = 0;
		int side = 0;
	};
	struct Route
	{
		Array<int> steps;
		float length = 0;
	}; ///< 符号付き(link ID + 1)で歩く向きを表す。
	void rebuild(
		const World& world, const RoadNetwork& roads, const BuildingAccessIndex& access, const TrainNetwork& trains);
	[[nodiscard]] Optional<Route> route(int from, int to, int expansionLimit);
	[[nodiscard]] Vec3 position(int step, float distance) const;
	[[nodiscard]] Optional<int> nearestNode(Vec3 point, double maximum = 800) const;
	[[nodiscard]] Optional<size_t> siteIndex(int64 key) const;
	[[nodiscard]] Optional<size_t> stationIndex(int id) const;
	[[nodiscard]] const Array<Node>& nodes() const { return m_nodes; }
	[[nodiscard]] const Array<Link>& links() const { return m_links; }
	[[nodiscard]] const Array<PedestrianSite>& sites() const { return m_sites; }
	[[nodiscard]] const Array<PedestrianStation>& stations() const { return m_stations; }
	[[nodiscard]] uint64 revision() const { return m_revision; }
	[[nodiscard]] int lastExpansions() const { return m_lastExpansions; }

private:
	struct Shape
	{
		CubicBezier curve;
		std::array<Vec2, 2> offsets;
		std::array<double, 2> heights;
	};
	Array<Node> m_nodes;
	Array<Link> m_links;
	Array<Shape> m_shapes;
	Array<PedestrianSite> m_sites;
	Array<PedestrianStation> m_stations;
	HashTable<int64, size_t> m_siteIndices;
	HashTable<int, size_t> m_stationIndices;
	HashTable<Point, Array<int>> m_spatial;
	HashTable<uint64, Route> m_cache;
	Array<uint64> m_cacheKeys;
	size_t m_cacheCursor = 0;
	Array<double> m_costs;
	Array<int> m_parents;
	Array<uint32> m_marks;
	uint32 m_search = 0;
	uint64 m_revision = 0;
	int m_lastExpansions = 0;
	int addNode(Vec3 point);
	void addLink(Link link);
	Vec3 onShape(int shape, int side, float arc) const;
	void components();
};
