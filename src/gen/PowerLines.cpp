#include "PowerLines.hpp"
#include "GenerationSettings.hpp"
#include "../debug/DebugLog.hpp"
#include <queue>

/// @file
/// @brief 送電線の経路探索と鉄塔配置。

namespace
{
	using Settlement = MapGenerator::Settlement;
	const GenerationSettings::Values& settings() { return GenerationSettings::get(); }

	double groundAt(const World& world, Vec2 point) { return world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.y)); }
	bool wetAt(const World& world, Vec2 point) { return groundAt(world, point) <= world.waterPlanningHeight(point.x, point.y) + 1; }

	/// @brief 道路中心線の標本点を 64m 格子に入れ、鉄塔の離隔を確かめる。
	class RoadPoints
	{
	public:
		explicit RoadPoints(const RoadNetwork& roads)
		{
			for (const auto& edge : roads.edges())
			{
				if (edge.id < 0) { continue; }
				const auto curve = roads.getBezier(edge.id); if (!curve) { continue; }
				const float half = edge.totalWidth() * .5f;
				for (float arc = 0; arc <= curve->totalLength; arc += 8)
				{
					const Vec3 p = curve->positionAt(arc);
					m_cells[key(p.x, p.z)] << Float3{ static_cast<float>(p.x), static_cast<float>(p.z), half };
				}
			}
		}

		[[nodiscard]] bool near(Vec2 point, double clearance) const
		{
			const int cx = static_cast<int>(Math::Floor(point.x / kCell)), cz = static_cast<int>(Math::Floor(point.y / kCell));
			const int reach = 1 + static_cast<int>(clearance / kCell);
			for (int dz = -reach; dz <= reach; ++dz)
			{
				for (int dx = -reach; dx <= reach; ++dx)
				{
					const auto found = m_cells.find(pack(cx + dx, cz + dz));
					if (found == m_cells.end()) { continue; }
					for (const auto& sample : found->second)
					{
						const double limit = clearance + sample.z;
						if (Vec2{ sample.x, sample.y }.distanceFromSq(point) < limit * limit) { return true; }
					}
				}
			}
			return false;
		}

	private:
		static constexpr double kCell = 64;
		static int64 pack(int x, int z) { return (static_cast<int64>(x) << 32) ^ static_cast<uint32>(z); }
		static int64 key(double x, double z) { return pack(static_cast<int>(Math::Floor(x / kCell)), static_cast<int>(Math::Floor(z / kCell))); }
		HashTable<int64, Array<Float3>> m_cells;
	};

	bool buildingNear(const World& world, Vec2 point, double clearance)
	{
		constexpr double kCell = static_cast<double>(CHUNK_SIZE) / ZONE_CELLS;
		const int reach = static_cast<int>(Ceil(clearance / kCell));
		const int gx = static_cast<int>(Math::Floor(point.x / kCell)), gz = static_cast<int>(Math::Floor(point.y / kCell));
		for (int dz = -reach; dz <= reach; ++dz)
		{
			for (int dx = -reach; dx <= reach; ++dx)
			{
				const int x = gx + dx, z = gz + dz;
				if (x < 0 || z < 0) { continue; }
				const auto* chunk = world.getChunk(Point{ x / ZONE_CELLS, z / ZONE_CELLS });
				if (!chunk) { continue; }
				const auto& building = chunk->buildingGrid[{ x % ZONE_CELLS, z % ZONE_CELLS }];
				if (building.type != BuildingType::None) { return true; }
			}
		}
		return false;
	}

	/// @brief 地形・市街地・水面を費用にした 8 近傍の最短経路。
	class CostGrid
	{
	public:
		CostGrid(const World& world, const Array<Settlement>& settlements)
			: m_cell(settings().powerLines_routingCell), m_size(static_cast<int>(Ceil(WORLD_SIZE / m_cell)))
		{
			const auto& c = settings();
			m_cost.assign(static_cast<size_t>(m_size) * m_size, 1.0f);
			for (int z = 0; z < m_size; ++z)
			{
				for (int x = 0; x < m_size; ++x)
				{
					const Vec2 p = center({ x, z });
					const double h = groundAt(world, p);
					const double slope = Max(Abs(groundAt(world, p + Vec2{ m_cell * .5, 0 }) - h), Abs(groundAt(world, p + Vec2{ 0, m_cell * .5 }) - h)) / (m_cell * .5);
					double cost = 1 + c.powerLines_slopeWeight * slope;
					if (wetAt(world, p)) { cost += c.powerLines_waterPenalty; }
					for (const auto& town : settlements)
					{
						// 市街地の中は架空線を通さない。農村集落は縁を避ける程度。
						const double reach = town.kind == MapGenerator::SettlementKind::RuralSettlement ? town.radius * .6 : town.radius * .95;
						if (p.distanceFromSq(town.center) < reach * reach) { cost += c.powerLines_settlementPenalty; }
					}
					m_cost[index({ x, z })] = static_cast<float>(cost);
				}
			}
		}

		[[nodiscard]] Array<Vec2> route(Vec2 from, Vec2 to) const
		{
			const Point start = cellOf(from), goal = cellOf(to);
			Array<float> distance(m_cost.size(), Math::Inf);
			Array<int> previous(m_cost.size(), -1);
			using Item = std::pair<float, int>;
			std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
			distance[index(start)] = 0; open.push({ 0.0f, index(start) });
			const Vec2 target = center(goal);
			while (!open.empty())
			{
				const auto [estimate, current] = open.top(); open.pop();
				if (current == index(goal)) { break; }
				const Point at{ current % m_size, current / m_size };
				const float known = distance[current];
				if (estimate - static_cast<float>(center(at).distanceFrom(target) / m_cell) > known + 1e-3f) { continue; }
				for (int dz = -1; dz <= 1; ++dz)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						if (!dx && !dz) { continue; }
						const Point next{ at.x + dx, at.y + dz };
						if (next.x < 0 || next.y < 0 || next.x >= m_size || next.y >= m_size) { continue; }
						const float step = static_cast<float>((dx && dz) ? Math::Sqrt2 : 1.0) * (m_cost[current] + m_cost[index(next)]) * .5f;
						if (known + step >= distance[index(next)]) { continue; }
						distance[index(next)] = known + step; previous[index(next)] = current;
						open.push({ known + step + static_cast<float>(center(next).distanceFrom(target) / m_cell), index(next) });
					}
				}
			}
			if (previous[index(goal)] < 0 && index(goal) != index(start)) { return {}; }
			Array<Vec2> path{ to };
			for (int at = previous[index(goal)]; at >= 0 && at != index(start); at = previous[at]) { path << center({ at % m_size, at / m_size }); }
			path << from;
			path.reverse();
			return path;
		}

	private:
		double m_cell;
		int m_size;
		Array<float> m_cost;
		[[nodiscard]] int index(Point p) const { return p.y * m_size + p.x; }
		[[nodiscard]] Point cellOf(Vec2 p) const { return { Clamp(static_cast<int>(p.x / m_cell), 0, m_size - 1), Clamp(static_cast<int>(p.y / m_cell), 0, m_size - 1) }; }
		[[nodiscard]] Vec2 center(Point p) const { return { (p.x + .5) * m_cell, (p.y + .5) * m_cell }; }
	};

	/// @brief Douglas-Peucker で格子経路を少数の直線区間（角度鉄塔の間）にする。
	void simplify(const Array<Vec2>& path, size_t first, size_t last, double tolerance, Array<size_t>& keep)
	{
		double worst = 0; size_t split = first;
		const Vec2 a = path[first], b = path[last], ab = b - a;
		const double length = Max(1e-6, ab.length());
		for (size_t i = first + 1; i < last; ++i)
		{
			const double offset = Abs((path[i] - a).cross(ab)) / length;
			if (offset > worst) { worst = offset; split = i; }
		}
		if (worst <= tolerance) { return; }
		simplify(path, first, split, tolerance, keep);
		keep << split;
		simplify(path, split, last, tolerance, keep);
	}

	/// @brief 変電所: 町の外縁で、相手側を向いた位置。
	Vec2 substation(const Settlement& town, Vec2 toward)
	{
		Vec2 direction = toward - town.center;
		if (direction.lengthSq() < 1) { direction = { 1, 0 }; }
		return town.center + direction.normalized() * (town.radius * settings().powerLines_substationOffset);
	}
}

Array<PowerLines::Line> PowerLines::generate(uint64 seed, const World& world, const RoadNetwork& roads, const Array<Settlement>& settlements)
{
	const auto& c = settings();
	if (!c.powerLines_enabled) { return {}; }
	const Stopwatch timer{ StartImmediately::Yes };
	// 地域の中心都市と町を変電所の候補にし、大きい順に並べる。
	Array<size_t> hubs;
	for (size_t i = 0; i < settlements.size(); ++i) { if (settlements[i].kind != MapGenerator::SettlementKind::RuralSettlement) { hubs << i; } }
	hubs.sort_by([&](size_t a, size_t b) { return settlements[a].radius > settlements[b].radius; });
	if (hubs.size() > static_cast<size_t>(c.powerLines_maximumLines + 1)) { hubs.resize(c.powerLines_maximumLines + 1); }

	// 変電所どうしは最小全域木で結び、大きい町から順に地図外（発電所）からの引き込みを足す。
	Array<std::pair<Vec2, Vec2>> spans;
	if (!hubs.isEmpty())
	{
		Array<bool> joined(hubs.size(), false); joined[0] = true;
		for (size_t added = 1; added < hubs.size(); ++added)
		{
			double best = Math::Inf; size_t from = 0, to = 0;
			for (size_t a = 0; a < hubs.size(); ++a)
			{
				if (!joined[a]) { continue; }
				for (size_t b = 0; b < hubs.size(); ++b)
				{
					if (joined[b]) { continue; }
					const double d = settlements[hubs[a]].center.distanceFrom(settlements[hubs[b]].center);
					if (d < best) { best = d; from = a; to = b; }
				}
			}
			joined[to] = true;
			const auto& a = settlements[hubs[from]]; const auto& b = settlements[hubs[to]];
			spans << std::pair{ substation(a, b.center), substation(b, a.center) };
		}
		const double inset = c.powerLines_routingCell;
		for (int feed = 0; feed < c.powerLines_borderFeeds && feed < static_cast<int>(hubs.size()); ++feed)
		{
			const auto& town = settlements[hubs[feed]];
			// 最寄りの地図端へ。乱数で同距離の向きを崩し、毎回同じ辺へ集まらないようにする。
			const double jitter = static_cast<double>((seed * 2654435761ULL + feed * 40503ULL) % 1000) / 1000.0 * WORLD_SIZE * .2;
			const std::array<Vec2, 4> edges{ Vec2{ inset, town.center.y + jitter }, Vec2{ WORLD_SIZE - inset, town.center.y - jitter },
				Vec2{ town.center.x - jitter, inset }, Vec2{ town.center.x + jitter, WORLD_SIZE - inset } };
			Vec2 border = edges[0];
			for (const auto& candidate : edges) { if (candidate.distanceFrom(town.center) < border.distanceFrom(town.center)) { border = candidate; } }
			border = Vec2{ Clamp(border.x, inset, WORLD_SIZE - inset), Clamp(border.y, inset, WORLD_SIZE - inset) };
			spans << std::pair{ border, substation(town, border) };
		}
	}
	if (spans.size() > static_cast<size_t>(c.powerLines_maximumLines)) { spans.resize(c.powerLines_maximumLines); }

	const CostGrid grid{ world, settlements };
	const RoadPoints roadPoints{ roads };
	const auto clear = [&](Vec2 point)
	{
		return !wetAt(world, point) && !roadPoints.near(point, c.powerLines_roadClearance) && !buildingNear(world, point, c.powerLines_buildingClearance);
	};
	Array<Line> lines;
	int towers = 0, shifted = 0, blocked = 0;
	for (const auto& [from, to] : spans)
	{
		const auto path = grid.route(from, to);
		if (path.size() < 2) { continue; }
		Array<size_t> keep{ 0 };
		simplify(path, 0, path.size() - 1, c.powerLines_simplifyTolerance, keep);
		keep << path.size() - 1;
		Line line; line.height = c.powerLines_towerHeight;
		for (size_t k = 0; k + 1 < keep.size(); ++k)
		{
			const Vec2 a = path[keep[k]], b = path[keep[k + 1]];
			const double length = a.distanceFrom(b);
			if (length < 1) { continue; }
			const Vec2 along = (b - a) / length;
			const int spansInLeg = Max(1, static_cast<int>(Ceil(length / c.powerLines_towerSpacing)));
			for (int i = 0; i < spansInLeg; ++i)
			{
				const bool corner = i == 0;
				Vec2 point = a + along * (length * i / spansInLeg);
				if (!clear(point))
				{
					// 径間の方向へずらして道路・建物・水面を避ける。角度鉄塔は直角方向にも探す。
					bool found = false;
					for (double shift = 12; shift <= c.powerLines_minimumSpan * .5 && !found; shift += 12)
					{
						for (const double sign : { 1.0, -1.0 })
						{
							const Vec2 trial = point + (corner ? Vec2{ -along.y, along.x } : along) * shift * sign;
							if (clear(trial)) { point = trial; found = true; break; }
						}
					}
					shifted += found; blocked += !found;
					if (!found && !corner) { continue; }
				}
				line.towers << Tower{ Vec3{ point.x, groundAt(world, point), point.y }, along, corner };
			}
		}
		line.towers << Tower{ Vec3{ path.back().x, groundAt(world, path.back()), path.back().y }, Vec2{ 1, 0 }, true };
		// 鉄塔の腕は前後の径間を二等分する向き。端の鉄塔は隣の径間に揃える。
		for (size_t i = 0; i < line.towers.size(); ++i)
		{
			const Vec3 prev = line.towers[i > 0 ? i - 1 : i].base, next = line.towers[i + 1 < line.towers.size() ? i + 1 : i].base;
			Vec2 dir{ next.x - prev.x, next.z - prev.z };
			if (dir.lengthSq() > 1e-6) { line.towers[i].along = dir.normalized(); }
		}
		towers += static_cast<int>(line.towers.size());
		if (line.towers.size() >= 2) { lines << std::move(line); }
	}
	DBG_LOG(U"[PowerLines] lines={} towers={} shifted={} blocked={} hubs={} ms={:.0f}"_fmt(lines.size(), towers, shifted, blocked, hubs.size(), timer.msF()));
	return lines;
}
