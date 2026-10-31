#include "ExpresswayNetwork.hpp"
#include "ExpresswayCrossSection.hpp"
#include "GenerationSettings.hpp"
#include "RailCostProfile.hpp"
#include "RoadAlignment.hpp"
#include "RoadAutoPlace.hpp"
#include "RoadPathfinder.hpp"
#include "RoadTerrainFit.hpp"
#include "StreetProfile.hpp"
#include "TransportClearance.hpp"
#include <array>
#include "../road/GuideSign.hpp"
#include "../debug/DebugLog.hpp"

/// @file
/// @brief 自動車専用道路の生成。平面線形 → 縦断 → 上下線 → IC/JCT → 標識・土工の順に組み立てる。

namespace
{
	using Settlement = MapGenerator::Settlement;
	const GenerationSettings::Values& settings() { return GenerationSettings::get(); }

	/// @brief 日本の高速道路の案内標識の緑（RGB 0,110,74 付近）。
	constexpr ColorF kExpresswayGreen{ 0.0, 110.0 / 255.0, 74.0 / 255.0, 1.0 };

	double groundAt(const World& world, Vec2 point) { return world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.y)); }
	bool landAt(const World& world, Vec2 point) { return groundAt(world, point) > world.waterPlanningHeight(point.x, point.y) + 2; }
	bool insideWorld(Vec2 point, double inset) { return point.x >= inset && point.y >= inset && point.x <= WORLD_SIZE - inset && point.y <= WORLD_SIZE - inset; }
	Vec2 rightOf(Vec2 tangent) { return Vec2{ tangent.y, -tangent.x }; }
	Vec3 lift(Vec2 point, double height) { return Vec3{ point.x, height, point.y }; }
	Vec2 flat(Vec3 point) { return Vec2{ point.x, point.z }; }

	/// @brief 中心線の標本列。高さは縦断を解いた後に入る。
	struct Line
	{
		Array<Vec2> points;
		Array<double> stations;
		Array<double> heights;
		double structureScale = 1; ///< 最大トンネル土被りの倍率。山地を抜ける計画だけ長大トンネルを許す
		double viaductScale = 1;   ///< 最大高架高さの倍率

		[[nodiscard]] double length() const { return stations.isEmpty() ? 0.0 : stations.back(); }
		[[nodiscard]] size_t segmentAt(double station) const
		{
			const auto it = std::upper_bound(stations.begin(), stations.end(), station);
			const size_t index = static_cast<size_t>(Max<ptrdiff_t>(1, it - stations.begin()));
			return Min(index, stations.size() - 1);
		}
		[[nodiscard]] Vec2 at(double station) const
		{
			const size_t i = segmentAt(station);
			const double span = Max(1e-6, stations[i] - stations[i - 1]);
			return points[i - 1].lerp(points[i], Clamp((station - stations[i - 1]) / span, 0.0, 1.0));
		}
		[[nodiscard]] double height(double station) const
		{
			const size_t i = segmentAt(station);
			const double span = Max(1e-6, stations[i] - stations[i - 1]);
			return Math::Lerp(heights[i - 1], heights[i], Clamp((station - stations[i - 1]) / span, 0.0, 1.0));
		}
		[[nodiscard]] Vec2 tangent(double station) const
		{
			const double from = Clamp(station - 30, 0.0, length()), to = Clamp(station + 30, 0.0, length());
			const Vec2 direction = at(to) - at(from);
			return direction.lengthSq() > 1e-9 ? direction.normalized() : Vec2{ 1, 0 };
		}
		/// @brief 最寄りの標本の station と水平距離。
		[[nodiscard]] std::pair<double, double> nearest(Vec2 point) const
		{
			double best = Math::Inf, station = 0;
			for (size_t i = 0; i < points.size(); ++i)
			{
				const double distance = points[i].distanceFromSq(point);
				if (distance < best) { best = distance; station = stations[i]; }
			}
			return { station, std::sqrt(best) };
		}
		void measure()
		{
			stations.resize(points.size());
			double total = 0;
			for (size_t i = 0; i < points.size(); ++i)
			{
				if (i > 0) { total += points[i].distanceFrom(points[i - 1]); }
				stations[i] = total;
			}
		}
	};

	/// @brief 地区と既存道路への近接費用。探索セルごとに地区を総当たりしないよう格子へ焼き込む。
	class AvoidanceGrid
	{
	public:
		AvoidanceGrid(const Array<Settlement>& towns, const RoadNetwork& roads)
		{
			m_cell = settings().expressway_routingCell;
			m_size = static_cast<int>(Ceil(WORLD_SIZE / m_cell)) + 1;
			m_cost.assign(static_cast<size_t>(m_size) * m_size, 1.0f);
			for (const auto& town : towns)
			{
				const double radius = town.radius + settings().expressway_settlementClearance;
				const int x0 = Max(0, static_cast<int>((town.center.x - radius) / m_cell)), x1 = Min(m_size - 1, static_cast<int>((town.center.x + radius) / m_cell));
				const int z0 = Max(0, static_cast<int>((town.center.y - radius) / m_cell)), z1 = Min(m_size - 1, static_cast<int>((town.center.y + radius) / m_cell));
				for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x)
				{
					const double distance = Vec2{ (x + .5) * m_cell, (z + .5) * m_cell }.distanceFrom(town.center);
					if (distance >= radius) { continue; }
					m_cost[z * m_size + x] += static_cast<float>(settings().expressway_settlementPenalty * (1 - Square(distance / radius)));
				}
			}
			// 一般道に沿う区間は IC 以外で道路を並走させない。横断する 1 セルの費用は小さい。
			for (const auto& edge : roads.edges())
			{
				if (edge.id < 0 || !edge.hasRoadLanes()) { continue; }
				const auto curve = roads.getBezier(edge.id); if (!curve) { continue; }
				for (float arc = 0; arc <= curve->totalLength; arc += static_cast<float>(m_cell * .5))
				{
					const Vec3 p = curve->positionAt(arc);
					const int x = static_cast<int>(p.x / m_cell), z = static_cast<int>(p.z / m_cell);
					if (x < 0 || z < 0 || x >= m_size || z >= m_size) { continue; }
					auto& value = m_cost[z * m_size + x];
					value = Max(value, static_cast<float>(settings().expressway_roadParallelPenalty));
				}
			}
		}
		[[nodiscard]] double at(Vec2 point) const
		{
			const int x = Clamp(static_cast<int>(point.x / m_cell), 0, m_size - 1), z = Clamp(static_cast<int>(point.y / m_cell), 0, m_size - 1);
			return m_cost[z * m_size + x];
		}
	private:
		double m_cell = 100;
		int m_size = 1;
		Array<float> m_cost;
	};

	/// @brief トンネル区間の費用倍率。鉄道（複線）の費用を基準に、片側の車線数が増えるほど掘る断面が広がって高くなる。
	/// @details 断面積は幅のおよそ 1.5 乗で増えるとみなし、片側 2 車線で expressway_tunnelCostScale 倍。
	double tunnelCostScale()
	{
		const auto& c = settings();
		return c.expressway_tunnelCostScale * Pow(Max(1, c.expressway_lanesPerDirection) / 2.0, 1.5);
	}

	/// @brief 鉄道と同じ地形回廊探索。地区・既存道路の近接費用を掛ける。
	Array<Vec2> searchCorridor(const World& world, const AvoidanceGrid& avoid, Vec2 from, Vec2 to)
	{
		const double margin = settings().expressway_corridorMargin;
		const float cell = settings().expressway_routingCell;
		const Vec2 lower{ Max(0.0, Min(from.x, to.x) - margin), Max(0.0, Min(from.y, to.y) - margin) };
		const Vec2 upper{ Min(static_cast<double>(WORLD_SIZE), Max(from.x, to.x) + margin), Min(static_cast<double>(WORLD_SIZE), Max(from.y, to.y) + margin) };
		const double fromHeight = groundAt(world, from), toHeight = groundAt(world, to);
		const Vec2 delta = to - from;
		RoadPathfinder finder;
		finder.setRailwayRouting(true);
		finder.setRoadType(RoadType::Expressway);
		finder.setConstructionCost([&](Vec2 point, double ground)
		{
			const double t = Clamp((point - from).dot(delta) / Max(1.0, delta.lengthSq()), 0.0, 1.0);
			return (1 + RailCostProfile::unitCost(Math::Lerp(fromHeight, toHeight, t) - ground, tunnelCostScale()) * settings().expressway_terrainCostWeight) * avoid.at(point);
		});
		finder.setup(world, lower, Max(2, static_cast<int>((upper.x - lower.x) / cell)), Max(2, static_cast<int>((upper.y - lower.y) / cell)), cell);
		const auto path = finder.findPath(finder.worldToGrid(static_cast<float>(from.x), static_cast<float>(from.y)),
			finder.worldToGrid(static_cast<float>(to.x), static_cast<float>(to.y)));
		Array<Vec2> result;
		for (const auto& point : finder.samplePath(path, 2)) { result << flat(point); }
		if (result.size() < 2) { return {}; }
		result.front() = from; result.back() = to;
		return result;
	}

	/// @brief 粗い回廊を等間隔の標本へ直し、最小半径を満たすまで内側の点だけ均す。
	Optional<Line> shapeLine(Array<Vec2> coarse, size_t fixedPrefix)
	{
		const auto& c = settings();
		Array<Vec2> unique;
		for (const auto& point : coarse) { if (unique.isEmpty() || unique.back().distanceFrom(point) > 1) { unique << point; } }
		coarse = std::move(unique);
		if (coarse.size() < 2) { return none; }
		for (int pass = 0; pass < c.expressway_smoothingPasses; ++pass)
		{
			auto next = coarse;
			for (size_t i = Max<size_t>(1, fixedPrefix); i + 1 < coarse.size(); ++i) { next[i] = coarse[i] * .5 + (coarse[i - 1] + coarse[i + 1]) * .25; }
			coarse = std::move(next);
		}
		const auto resample = [&](const Array<Vec2>& source)
		{
			Array<Vec3> raised;
			for (const auto& point : source) { raised << lift(point, 0); }
			const auto curves = RoadAlignment::fit(raised);
			Array<Vec2> dense;
			for (const auto& curve : curves)
			{
				const int count = Max(1, static_cast<int>(Ceil(curve.totalLength / c.expressway_profileSampleLength)));
				for (int i = 0; i < count; ++i) { dense << flat(curve.positionAt(curve.totalLength * i / count)); }
			}
			dense << source.back();
			return dense;
		};
		Line line;
		line.points = resample(coarse);
		if (line.points.size() < 3) { return none; }
		// 標本化後の固定区間（JCT の分岐直線）を保つため、先頭の拘束長を距離で持つ。
		double fixedLength = 0;
		for (size_t i = 1; i < Min(fixedPrefix, coarse.size()); ++i) { fixedLength += coarse[i].distanceFrom(coarse[i - 1]); }
		// 3 点の外接円で半径を見積もり、最小曲線半径を割る点の前後だけを繰り返し均す。
		// 全体を均すと緩い曲線まで中へ寄るため、急な箇所の周囲（半径の半分の幅）に限る。
		{
			const double target = c.expressway_minimumRadius * 1.15;
			const int window = Max(2, static_cast<int>(Ceil(c.expressway_minimumRadius * .5 / c.expressway_profileSampleLength)));
			// 折れ線を弧長で等間隔（profileSampleLength）に取り直す。端点は保つ。
			const auto uniform = [&](const Array<Vec2>& source)
			{
				double total = 0;
				for (size_t i = 1; i < source.size(); ++i) { total += source[i].distanceFrom(source[i - 1]); }
				const int segments = Max(2, static_cast<int>(Math::Round(total / c.expressway_profileSampleLength)));
				const double step = total / segments;
				Array<Vec2> result{ source.front() };
				double walked = 0, next = step;
				for (size_t i = 1; i < source.size(); ++i)
				{
					const double piece = source[i].distanceFrom(source[i - 1]);
					while (piece > 1e-9 && next <= walked + piece && static_cast<int>(result.size()) < segments)
					{
						result << source[i - 1].lerp(source[i], (next - walked) / piece);
						next += step;
					}
					walked += piece;
				}
				result << source.back();
				return result;
			};
			const auto circumradius = [](Vec2 a, Vec2 b, Vec2 d)
			{
				const double cross = Abs((b - a).x * (d - a).y - (b - a).y * (d - a).x);
				return cross < 1e-9 ? Math::Inf : a.distanceFrom(b) * b.distanceFrom(d) * d.distanceFrom(a) / (2 * cross);
			};
			for (int pass = 0; pass < c.expressway_radiusPasses * 20; ++pass)
			{
				const size_t count = line.points.size();
				Array<bool> marked(count, false);
				bool tight = false;
				for (size_t i = 1; i + 1 < count; ++i)
				{
					// 3 点おきに測り、標本間隔の細かな揺れを曲率と取り違えない。
					const size_t lo = i >= 3 ? i - 3 : 0, hi = Min(count - 1, i + 3);
					if (circumradius(line.points[lo], line.points[i], line.points[hi]) >= target) { continue; }
					tight = true;
					// 均しても残る急曲線（通過点での折り返しなど）は、均す幅を広げて角を大きく丸める。
					const size_t reach = static_cast<size_t>(window) * (1 + static_cast<size_t>(pass) / 30);
					for (size_t j = (i > reach ? i - reach : 1); j <= Min(count - 2, i + reach); ++j) { marked[j] = true; }
				}
				if (!tight) { DBG_LOG(U"[ExpresswayCurve] smoothed passes={}"_fmt(pass)); break; }
				if (pass + 1 == c.expressway_radiusPasses * 20)
				{
					double worst = Math::Inf; Vec2 at; size_t worstIndex = 0;
					for (size_t i = 3; i + 3 < count; ++i) { const double r = circumradius(line.points[i - 3], line.points[i], line.points[i + 3]); if (r < worst) { worst = r; at = line.points[i]; worstIndex = i; } }
					DBG_LOG(U"[ExpresswayCurve] unresolved passes={} radius={:.0f} at={} index={}/{}"_fmt(pass + 1, worst, at, worstIndex, count));
					// 分岐の固定直線に続く支線は、最小曲線半径を守れなければ作らない（JCT は任意）。本線は必ず通す。
					if (fixedPrefix > 1) { return none; }
				}
				auto softened = line.points; double along = 0;
				for (size_t i = 1; i + 1 < count; ++i)
				{
					along += line.points[i].distanceFrom(line.points[i - 1]);
					if (!marked[i] || along <= fixedLength + 1) { continue; }
					// 離れた点どうしの中点へ寄せると、曲がりが速く広い範囲へ分散する。
					const size_t k = Min<size_t>({ 1 + static_cast<size_t>(pass) / 40, i, count - 1 - i });
					softened[i] = line.points[i].lerp((line.points[i - k] + line.points[i + k]) * .5, .5);
				}
				line.points = std::move(softened);
				// 均すと曲線の内側へ点が寄って間隔が縮み、同じ点数の幅が短くなる。定期的に等間隔へ戻す。
				if (pass % 8 == 7) { line.points = uniform(line.points); }
			}
		}
		for (int pass = 0; pass < c.expressway_radiusPasses; ++pass)
		{
			Array<Vec3> raised;
			for (const auto& point : line.points) { raised << lift(point, 0); }
			const auto curves = RoadAlignment::fit(raised);
			if (curves.size() + 1 != raised.size()) { return none; }
			bool tight = false;
			for (const auto& curve : curves) { tight |= curve.minimumHorizontalRadius() < c.expressway_minimumRadius * 1.02; }
			if (!tight) { break; }
			auto softened = line.points; double along = 0;
			for (size_t i = 1; i + 1 < line.points.size(); ++i)
			{
				along += line.points[i].distanceFrom(line.points[i - 1]);
				if (along <= fixedLength + 1) { continue; }
				softened[i] = line.points[i].lerp((line.points[i - 1] + line.points[i + 1]) * .5, .45);
			}
			line.points = std::move(softened);
		}
		line.measure();
		return line;
	}

	/// @brief 地図端の端点を置く内側距離。外部接続の判定（RoadNetwork::isBorderNode 相当）に収まる値。
	double borderInset() { return settings().network_borderInset + 20; }

	/// @brief 地図端から内側へ入ってすぐ急斜面・深い谷に当たらないか。当たる端点は縦断（最急勾配＋トンネルの土被り・高架の高さ）が成立しにくい。
	bool gentleBorderApproach(const World& world, Vec2 candidate, Vec2 inward)
	{
		const double edge = groundAt(world, candidate);
		const double cover = settings().expressway_maximumTunnelDepth * .6, viaduct = settings().expressway_maximumViaductHeight;
		for (double distance = 200; distance <= 3000; distance += 200)
		{
			const double rise = groundAt(world, candidate + inward * distance) - edge, allowance = settings().expressway_maximumGrade * distance;
			if (rise > allowance + cover || -rise > allowance + viaduct * 2) { return false; }
		}
		return true;
	}

	/// @brief 地図の外周から、from より direction 側にある低い陸地の端点を探す。延長線の近くに使える端点がないときに使う。
	Optional<Vec2> lowlandBorder(const World& world, Vec2 from, Vec2 direction)
	{
		const double inset = borderInset(), origin = groundAt(world, from);
		Optional<Vec2> best; double bestScore = Math::Inf;
		for (double along = inset; along <= WORLD_SIZE - inset; along += 250)
		{
			for (const auto& [point, inward] : { std::pair{ Vec2{ inset, along }, Vec2{ 1, 0 } }, std::pair{ Vec2{ WORLD_SIZE - inset, along }, Vec2{ -1, 0 } },
				std::pair{ Vec2{ along, inset }, Vec2{ 0, 1 } }, std::pair{ Vec2{ along, WORLD_SIZE - inset }, Vec2{ 0, -1 } } })
			{
				const Vec2 offset = point - from;
				const double distance = offset.length();
				// 進行方向から ±75° 以内の地図端だけを使い、線形が折り返さないようにする。
				if (distance < 2000 || offset.dot(direction) < distance * .26) { continue; }
				if (!landAt(world, point) || !landAt(world, point + inward * 200) || !gentleBorderApproach(world, point, inward)) { continue; }
				const double rise = Abs(groundAt(world, point) - origin);
				const double score = distance + rise * 60;
				if (score < bestScore) { bestScore = score; best = point; }
			}
		}
		return best;
	}

	/// @brief 地図端の陸地を、方向の延長線から近い順に探す。
	/// @param relax 0: 設定どおり。1: 地図端の全長を探し、高低差の上限を 4 倍に緩める。2: 高低差を問わない。
	Optional<Vec2> borderLanding(const World& world, Vec2 from, Vec2 direction, int relax = 0)
	{
		const double inset = borderInset();
		const double searchDistance = relax == 0 ? settings().expressway_borderSearchDistance : static_cast<double>(WORLD_SIZE);
		const double maximumRise = relax == 0 ? settings().expressway_borderMaximumRise : relax == 1 ? settings().expressway_borderMaximumRise * 4 : Math::Inf;
		double reach = Math::Inf;
		for (const auto& [axis, bound] : { std::pair{ direction.x, direction.x > 0 ? WORLD_SIZE - inset - from.x : inset - from.x },
			std::pair{ direction.y, direction.y > 0 ? WORLD_SIZE - inset - from.y : inset - from.y } })
		{
			if (Abs(axis) > 1e-6) { reach = Min(reach, bound / axis); }
		}
		if (!std::isfinite(reach) || reach <= 0) { return none; }
		const Vec2 hit = from + direction * reach;
		// 境界線に沿って左右へ探し、海に落ちる端点を避ける。
		const bool vertical = Abs(hit.x - inset) < 1 || Abs(hit.x - (WORLD_SIZE - inset)) < 1;
		const Vec2 along = vertical ? Vec2{ 0, 1 } : Vec2{ 1, 0 };
		// 山の上で地図端に抜けると縦断が成立しないため、出発地との高低差が小さい陸地を選ぶ。
		const double origin = groundAt(world, from);
		Optional<Vec2> best; double bestScore = Math::Inf;
		const auto gentleApproach = [&](Vec2 candidate) { return relax >= 2 || gentleBorderApproach(world, candidate, -direction); };
		for (double offset = 0; offset <= searchDistance; offset += 250)
		{
			for (const double sign : { 1.0, -1.0 })
			{
				const Vec2 candidate = hit + along * offset * sign;
				if (insideWorld(candidate, inset - 1) && landAt(world, candidate) && (relax >= 2 || landAt(world, candidate - direction * 200)) && gentleApproach(candidate))
				{
					const double rise = Abs(groundAt(world, candidate) - origin);
					const double score = offset + rise * 40;
					if (rise < maximumRise && score < bestScore) { bestScore = score; best = candidate; }
				}
				if (offset == 0) { break; }
			}
		}
		return best;
	}

	/// @brief 本線の端点を必ず地図端に置く。向きを ±30°刻みで振り、それでもなければ条件を段階的に緩める。
	/// @details どの陸地も見つからなければ延長線と地図端の交点（海上なら橋）を返す。
	Vec2 borderEndpoint(const World& world, Vec2 from, Vec2 direction, int firstRelax)
	{
		for (int relax = Clamp(firstRelax, 0, 2); relax <= 2; ++relax)
		{
			// 高低差の小さい地図端を優先する（山の上の地図端に抜けると縦断が成立しにくい）。
			for (const double degrees : { 0.0, 30.0, -30.0, 60.0, -60.0, 90.0, -90.0 })
			{
				const double angle = Math::ToRadians(degrees);
				const Vec2 turned{ direction.x * Cos(angle) - direction.y * Sin(angle), direction.x * Sin(angle) + direction.y * Cos(angle) };
				if (const auto border = borderLanding(world, from, turned.normalized(), relax))
				{
					if (relax > 0 || degrees != 0) { DBG_LOG(U"[ExpresswayBorder] relaxed level={} turnDegrees={:.0f} point=({:.0f},{:.0f})"_fmt(relax, degrees, border->x, border->y)); }
					return *border;
				}
			}
			if (relax == 0)
			{
				if (const auto border = lowlandBorder(world, from, direction))
				{
					DBG_LOG(U"[ExpresswayBorder] lowland point=({:.0f},{:.0f})"_fmt(border->x, border->y));
					return *border;
				}
			}
		}
		const double inset = borderInset();
		double reach = Math::Inf;
		if (Abs(direction.x) > 1e-6) { reach = Min(reach, ((direction.x > 0 ? WORLD_SIZE - inset : inset) - from.x) / direction.x); }
		if (Abs(direction.y) > 1e-6) { reach = Min(reach, ((direction.y > 0 ? WORLD_SIZE - inset : inset) - from.y) / direction.y); }
		const Vec2 hit = from + direction * (std::isfinite(reach) ? Max(0.0, reach) : 0.0);
		DBG_LOG(U"[ExpresswayBorder] fallback=extension point=({:.0f},{:.0f})"_fmt(hit.x, hit.y));
		return Vec2{ Clamp(hit.x, inset, WORLD_SIZE - inset), Clamp(hit.y, inset, WORLD_SIZE - inset) };
	}

	/// @brief 縦断の局所拘束。station 区間内で地盤からの高さを制限する。
	struct Constraint { double from = 0, to = 0, minimumAbove = -1e9, maximumAbove = 1e9; };

	bool solveProfile(Line& line, const World& world, const TransportClearance& crossings, double start, double end,
		const Array<Constraint>& constraints, double grade)
	{
		const auto& c = settings();
		const double margin = ExpresswayCrossSection::carriagewayOffset() + c.expressway_laneWidth * c.expressway_lanesPerDirection * .5 + c.expressway_outerShoulder;
		Array<RailCostProfile::Sample> samples;
		for (size_t i = 0; i < line.points.size(); ++i)
		{
			const Vec2 position = line.points[i];
			const double ground = groundAt(world, position), station = line.stations[i];
			double minimum = Max(c.railway_minimumAltitude, ground - c.expressway_maximumTunnelDepth * line.structureScale), maximum = ground + c.expressway_maximumViaductHeight * line.viaductScale;
			double roadClearance = -1e9, underpass = 1e9;
			const Vec2 side = rightOf(line.tangent(station));
			for (const double lateral : { -margin, 0.0, margin })
			{
				const Vec2 around = position + side * lateral;
				const double terrain = groundAt(world, around), water = world.waterPlanningHeight(around.x, around.y);
				const auto corridor = crossings.interval(around, c.expressway_roadClearanceMargin + 4);
				roadClearance = Max(roadClearance, corridor.second + c.expressway_roadClearanceMargin);
				underpass = Min(underpass, corridor.first - c.expressway_roadClearanceMargin);
				if (terrain < water + 1) { roadClearance = Max(roadClearance, water + c.expressway_waterClearance); underpass = Min(underpass, terrain - c.expressway_waterTunnelCover); }
			}
			samples << RailCostProfile::Sample{ position, ground, minimum, maximum, roadClearance, underpass };
		}
		// 局所拘束は既存道路の立体交差の取付区間（前後 400m）では上限を外し、道路を越える縦断を残す。
		constexpr double kCrossingApproach = 400;
		Array<double> crossingStations;
		for (size_t i = 0; i < samples.size(); ++i) { if (samples[i].roadClearance > -1e8) { crossingStations << line.stations[i]; } }
		for (size_t i = 0; i < samples.size(); ++i)
		{
			auto& sample = samples[i];
			const double station = line.stations[i];
			const bool approach = crossingStations.any([&](double crossing) { return Abs(crossing - station) < kCrossingApproach; });
			for (const auto& rule : constraints)
			{
				if (station < rule.from || station > rule.to) { continue; }
				sample.minimum = Max(sample.minimum, sample.ground + rule.minimumAbove);
				if (!approach) { sample.maximum = Min(sample.maximum, sample.ground + rule.maximumAbove); }
			}
			if (sample.minimum > sample.maximum) { sample.maximum = sample.minimum; }
		}
		// 地図端・JCT の端点が既存道路の上下離隔帯に入るときは、道路を跨ぐ高さへ上げる。
		const auto clearEndpoint = [](const RailCostProfile::Sample& sample, double height)
		{
			return height > sample.underpass && height < sample.roadClearance ? std::ceil(sample.roadClearance) + .5 : height;
		};
		start = clearEndpoint(samples.front(), start);
		end = clearEndpoint(samples.back(), end);
		// 端点のすぐ先で川や道路を跨ぐ必要があり、最急勾配でも間に合わないときは端点を持ち上げる。
		const auto approachEndpoint = [&](bool front, double height)
		{
			const double origin = front ? line.stations.front() : line.stations.back();
			for (size_t k = 0; k < samples.size(); ++k)
			{
				const size_t i = front ? k : samples.size() - 1 - k;
				const double distance = Abs(line.stations[i] - origin);
				if (distance > 600) { break; }
				const auto& sample = samples[i];
				// 谷の深い地図端では、最大土被り・最大橋脚高の範囲へ最急勾配で届く高さまで端点を寄せる。
				height = Clamp(height, sample.minimum - grade * distance, sample.maximum + grade * distance);
				if (sample.roadClearance < -1e8 || sample.underpass >= height - grade * distance) { continue; }
				height = Max(height, std::ceil(sample.roadClearance - grade * distance) + .5);
			}
			return height;
		};
		start = approachEndpoint(true, start);
		end = approachEndpoint(false, end);
		auto profile = RailCostProfile::solve(samples, start, end, grade, {}, tunnelCostScale());
		// 終点近くの立体交差を量子化した縦断で越えきれないときは、地図端側の終点を少しずつ上げて解き直す（起点は JCT の高さに固定されることがある）。
		for (const double raise : { 1.0, 2.0, 4.0, 8.0 })
		{
			if (profile.feasible) { break; }
			profile = RailCostProfile::solve(samples, start, Min(end + raise, samples.back().maximum), grade, {}, tunnelCostScale());
		}
		// 最後の計画（土被りの上限なし）では、地図端の端点の高さも固定しない。地図外へは高架・トンネルのまま抜けてよい。
		if (!profile.feasible && line.structureScale >= 100)
		{
			// 元の高さ（地盤）に近い順に試し、地図端で地表に近い高さを優先する。
			const auto heightsOf = [](const RailCostProfile::Sample& sample, double preferred)
			{
				Array<double> heights;
				for (int k = 0; k <= 8; ++k) { heights << Math::Lerp(sample.minimum, sample.maximum, k / 8.0); }
				heights.sort_by([&](double a, double b) { return Abs(a - preferred) < Abs(b - preferred); });
				return heights;
			};
			for (const double height : heightsOf(samples.back(), end))
			{
				if (profile.feasible) { break; }
				profile = RailCostProfile::solve(samples, start, height, grade, {}, tunnelCostScale());
			}
			for (const double height : heightsOf(samples.front(), start))
			{
				if (profile.feasible) { break; }
				profile = RailCostProfile::solve(samples, height, end, grade, {}, tunnelCostScale());
			}
			if (profile.feasible) { DBG_LOG(U"[ExpresswayProfile] solved with a free map-edge height start={:.1f} end={:.1f}"_fmt(profile.heights.front(), profile.heights.back())); }
		}
		if (!profile.feasible)
		{
			// 診断: 離隔帯を無視しても、始点から最急勾配で届く高さが地盤の上下限（トンネル土被り・高架高さ）と交わらなくなる標本。
			double low = Min(start, end), high = Max(start, end);
			for (const auto& sample : samples) { low = Min(low, sample.minimum); high = Max(high, sample.maximum); }
			String limit = U"none";
			double lo = start, hi = start;
			for (size_t i = 1; i < samples.size(); ++i)
			{
				const double run = samples[i].position.distanceFrom(samples[i - 1].position);
				lo = Max(lo - run * grade, samples[i].minimum); hi = Min(hi + run * grade, samples[i].maximum);
				if (lo <= hi) { continue; }
				limit = U"station={:.0f} at=({:.0f},{:.0f}) ground={:.1f} band=({:.1f},{:.1f})"_fmt(
					line.stations[i], samples[i].position.x, samples[i].position.y, samples[i].ground, samples[i].minimum, samples[i].maximum);
				break;
			}
			if (limit == U"none" && (end < lo - 1 || end > hi + 1)) { limit = U"end={:.1f} reachable=({:.1f},{:.1f})"_fmt(end, lo, hi); }
			DBG_LOG(U"[ExpresswayProfile] infeasible start={:.1f} end={:.1f} samples={} heightRange=({:.0f},{:.0f}) gradeLimit={}"_fmt(start, end, samples.size(), low, high, limit));
			return false;
		}
		line.heights = profile.heights;
		return true;
	}

	/// @brief 片側車道。station は中心線上の値で、配列は走行順。
	struct Carriageway
	{
		Array<double> stations;
		Array<int> nodes;
		Array<int> edges;
		Array<double> arcs; ///< 各ノードまでの走行距離
		Array<int> phones;  ///< 非常電話を置くエッジ
		int side = 1;       ///< +1: station 増加方向（中心線の左側）、-1: 逆方向（右側）

		[[nodiscard]] int indexAt(double station) const
		{
			for (size_t i = 0; i < stations.size(); ++i) { if (Abs(stations[i] - station) < .5) { return static_cast<int>(i); } }
			return -1;
		}
		[[nodiscard]] int nodeAt(double station) const { const int i = indexAt(station); return i < 0 ? -1 : nodes[i]; }
	};

	using HeightOffset = std::function<double(double)>;

	Vec3 carriagewayPoint(const Line& line, double station, int side, const HeightOffset& offset)
	{
		const Vec2 center = line.at(station), right = rightOf(line.tangent(station));
		const double lateral = -side * ExpresswayCrossSection::carriagewayOffset();
		return lift(center + right * lateral, line.height(station) + (offset ? offset(station) : 0.0));
	}

	/// @brief ノードを置く station。IC・JCT の分合流点は必ずノードにする。
	/// @param optional 置けるなら置く節点（変速車線の始終点）。他の節点から kMinimumGap 未満なら省き、短い区間で曲率が跳ねるのを防ぐ。
	Array<double> nodeStations(const Line& line, Array<double> forced, Array<double> optional = {})
	{
		constexpr double kMinimumGap = 40;
		const double spacing = settings().expressway_nodeSpacing, total = line.length();
		forced.remove_if([&](double s) { return s <= 1 || s >= total - 1; });
		Array<double> result{ 0.0, total };
		result.append(forced);
		optional.sort();
		for (const double s : optional)
		{
			if (s <= kMinimumGap || s >= total - kMinimumGap || result.any([&](double f) { return Abs(f - s) < kMinimumGap; })) { continue; }
			result << s;
		}
		forced = result;
		for (double s = spacing; s < total - spacing * .4; s += spacing)
		{
			if (forced.any([&](double f) { return Abs(f - s) < spacing * .3; })) { continue; }
			result << s;
		}
		result.sort();
		Array<double> unique;
		for (const double s : result) { if (unique.isEmpty() || s - unique.back() > 5) { unique << s; } }
		return unique;
	}

	/// @brief 道路網へ追加したエッジを供用状態の自動車専用道として整える。
	void finishEdge(RoadNetwork& roads, const World& world, int id, const std::function<void(RoadEdge&)>& section)
	{
		auto* edge = roads.getEdge(id);
		edge->designGrade = true;
		edge->edgeState = EdgeState::Open;
		edge->length = roads.getBezier(id)->totalLength;
		roads.updateEdgeElevation(id, world);
		section(*roads.getEdge(id));
		roads.rebuildNodeConnectivity(edge->nodeA, edge->nodeB);
	}

	Optional<Carriageway> buildCarriageway(RoadNetwork& roads, const World& world, const Line& line, const Array<double>& stations,
		int side, const HeightOffset& offset, Array<int>& created)
	{
		Carriageway result;
		result.side = side;
		result.stations = stations;
		if (side < 0) { result.stations.reverse(); }
		Array<Vec3> points;
		for (const double s : result.stations) { points << carriagewayPoint(line, s, side, offset); }
		// 接線は隣の節点ではなく中心線の向きから取る。IC の分合流点で節点の間隔が不揃いでも曲率が跳ねない。
		Array<CubicBezier> curves;
		for (size_t i = 0; i + 1 < points.size(); ++i)
		{
			const Vec3 a = points[i], b = points[i + 1];
			const Vec2 span = flat(b - a);
			if (span.lengthSq() < 1e-6) { return none; }
			const Vec2 ta = line.tangent(result.stations[i]) * side, tb = line.tangent(result.stations[i + 1]) * side;
			const double arm = span.length() / 3;
			const auto height = [&](Vec2 p) { return a.y + (b.y - a.y) * (p - flat(a)).dot(span) / span.lengthSq(); };
			const Vec2 first = flat(a) + ta * arm, second = flat(b) - tb * arm;
			curves << CubicBezier{ a, lift(first, height(first)), lift(second, height(second)), b };
		}
		for (const auto& point : points) { result.nodes << roads.addNode(point, NodeType::Joint); }
		double arc = 0;
		result.arcs << 0.0;
		for (size_t i = 0; i < curves.size(); ++i)
		{
			const auto id = roads.addEdge(result.nodes[i], result.nodes[i + 1], curves[i].p1, curves[i].p2, RoadType::Expressway, 2);
			if (!id) { return none; }
			// 走行距離 1km の境界を含む区間に非常電話を置く。
			const double spacing = settings().expressway_emergencyPhoneSpacing;
			const bool phone = Floor((arc + curves[i].totalLength) / spacing) > Floor(arc / spacing);
			finishEdge(roads, world, *id, [phone](RoadEdge& edge) { ExpresswayCrossSection::applyCarriageway(edge, { .emergencyPhone = phone }); });
			if (phone) { result.phones << *id; }
			result.edges << *id; created << *id;
			arc += curves[i].totalLength; result.arcs << arc;
		}
		return result;
	}

	/// @brief 水平方向の接線から制御点を作る 1 本のランプ曲線。高さは制御点ごとに指定する。
	CubicBezier rampCurve(Vec3 from, Vec2 fromDirection, Vec3 to, Vec2 toDirection, double firstControlHeight, double secondControlHeight)
	{
		const double handle = flat(to - from).length() * .38;
		const Vec2 a = flat(from) + fromDirection.normalized() * handle, b = flat(to) - toDirection.normalized() * handle;
		return CubicBezier{ from, lift(a, firstControlHeight), lift(b, secondControlHeight), to };
	}

	double maximumGrade(const CubicBezier& curve)
	{
		double grade = 0;
		for (int i = 0; i <= 32; ++i)
		{
			const Vec3 t = curve.tangent(i / 32.0f);
			grade = Max(grade, Abs(t.y) / Max(1e-6, Vec2{ t.x, t.z }.length()));
		}
		return grade;
	}

	Optional<int> addRamp(RoadNetwork& roads, const World& world, int fromNode, int toNode, const CubicBezier& curve, float speed, Array<int>& created, int lanes = 1)
	{
		const auto id = roads.addEdge(fromNode, toNode, curve.p1, curve.p2, RoadType::Expressway, lanes);
		if (!id) { return none; }
		finishEdge(roads, world, *id, [speed, lanes](RoadEdge& edge) { ExpresswayCrossSection::applyRamp(edge, speed, lanes); });
		created << *id;
		return id;
	}

	/// @brief 文字だけの板（車間距離確認・道路情報板）。rows は文字と、板の高さに対する文字の高さ。
	int addTextPlate(RoadNetwork& roads, int edgeId, double arc, const Array<std::pair<String, double>>& rows, const ColorF& background, double width, double height)
	{
		const auto* edge = roads.getEdge(edgeId); if (!edge || rows.isEmpty()) { return -1; }
		const auto& c = settings();
		GuideSignPlacement sign;
		sign.parentEdgeId = edgeId;
		sign.nodeEndId = edge->nodeA;
		sign.arcOffset = static_cast<float>(arc);
		sign.lateralOffset = -(c.expressway_lanesPerDirection * c.expressway_laneWidth * .5f + c.expressway_outerShoulder + c.expressway_guardrailWidth + c.expressway_signLateralMargin);
		sign.autoGenerated = false;
		sign.bgColor = background;
		sign.showReading = false;
		sign.widthOverride = static_cast<float>(width);
		sign.heightOverride = static_cast<float>(height);
		double total = 0;
		for (const auto& row : rows) { total += row.second * 1.25; }
		double y = (1 - total) * .5;
		for (const auto& [text, size] : rows)
		{
			SignElement element;
			element.kind = SignElementKind::Text;
			element.text = text;
			element.posY = static_cast<float>(y + size * .625);
			element.scale = static_cast<float>(size / GuideSignLayout::DirectionArrow::UpTextH);
			sign.elements << element;
			y += size * 1.25;
		}
		return roads.addGuideSign(sign);
	}

	/// @brief 走行距離 arc の位置を含むエッジと、その中の距離。
	std::pair<int, double> locate(const Carriageway& road, double arc)
	{
		size_t edge = 0;
		while (edge + 1 < road.edges.size() && road.arcs[edge + 1] < arc) { ++edge; }
		return { road.edges[edge], arc - road.arcs[edge] };
	}

	/// @brief 車間距離確認区間: 予告板と、0m（確認基点）・50m・100m の距離板（緑地に白文字）。
	int addSpacingCheck(RoadNetwork& roads, const Carriageway& road, double start)
	{
		int count = 0;
		const ColorF green{ 0.0, 110.0 / 255.0, 74.0 / 255.0, 1.0 };
		const auto [edge, local] = locate(road, start - 200);
		count += addTextPlate(roads, edge, local, { { U"車間確認", .3 }, { U"200m先", .3 } }, green, 2.6, 1.9) >= 0;
		for (const int metres : { 0, 50, 100 })
		{
			const auto [at, offset] = locate(road, start + metres);
			Array<std::pair<String, double>> rows{ { U"{}m"_fmt(metres), .46 } };
			if (metres == 0) { rows << std::pair<String, double>{ U"確認基点", .2 }; }
			count += addTextPlate(roads, at, offset, rows, green, 1.5, 1.2) >= 0;
		}
		return count;
	}

	/// @brief 道路情報板（黒地の文字表示板）。表示文は分岐名と位置から決める。
	int addInformationBoard(RoadNetwork& roads, const Carriageway& road, double arc, const String& name, const String& nextName, uint64 seed)
	{
		if (arc < 20) { return 0; }
		const auto [edge, local] = locate(road, arc);
		const uint64 hash = seed ^ (static_cast<uint64>(arc) * 0x9e3779b97f4a7c15ULL);
		const int kind = static_cast<int>(hash % 4);
		const String head = nextName.isEmpty() ? U"{}出口"_fmt(name) : U"{}－{}"_fmt(name, nextName);
		const String body = kind == 0 ? U"渋滞 {}km {}分"_fmt(2 + hash % 5, 8 + hash % 13)
			: kind == 1 ? U"事故 車線規制" : kind == 2 ? U"工事 車線規制" : U"雨 速度落とせ";
		const ColorF black{ .06, .06, .06, 1.0 };
		return addTextPlate(roads, edge, local, { { head, .26 }, { body, .26 } }, black, 4.6, 2.2) >= 0;
	}

	// ===== 高速道路の案内標識（NEXCO の様式） =====
	// 地は緑、文字・矢印・縁は白。出口番号と路線名は白地に緑文字の枠、路線番号（E○○）は緑地に白縁・白文字。
	// 出口の予告は 2km（番号・IC 名・距離）→ 1km（出口・番号・IC 名・距離と行き先）→ 500m（門型。出口と本線の方面）
	// → 出口（門型。行き先と左上への矢印、本線の方面）の順に置く。寸法は漢字 60cm・ローマ字はその約 4 割。

	constexpr ColorF kSignWhite{ 1, 1, 1, 1 };
	/// @brief 板面の緑。板面テクスチャは描いた色を線形値として sRGB へ符号化するため、
	///   sRGB の緑（0, 110, 74）を線形へ戻して渡し、実物の濃い緑で表示させる。
	const ColorF kSignGreen = kExpresswayGreen.removeSRGBCurve();

	/// @brief 地名と読み。
	struct SignPlace { String name; String reading; };

	/// @brief 路線（路線番号と路線名）。
	struct SignRoute { String number; String name; String reading; };

	/// @brief 板面をメートルで組み立て、最後に盤面に対する比へ直す。
	class BoardBuilder
	{
	public:
		explicit BoardBuilder(double height) : m_height(height) {}

		/// @brief 文字列の幅の見積り [m]（漢字は字高と同じ、英数字はその 6 割）。
		static double textWidth(StringView text, double h)
		{
			double width = 0;
			for (const char32 ch : text) { width += ch < 0x80 ? h * (ch == U' ' ? .3 : .6) : h; }
			return width;
		}

		/// @param align 0: x が左端、0.5: 中央、1: 右端。reading は字高の 4 割で下に添える。
		void text(double x, double y, double h, const String& text, const String& reading = U"", float align = 0)
		{
			SignElement element;
			element.kind = SignElementKind::Text; element.text = text; element.reading = reading;
			element.posY = static_cast<float>(y / m_height);
			element.scale = static_cast<float>(h / (GuideSignLayout::DirectionArrow::UpTextH * m_height));
			element.align = align;
			m_items << Item{ element, x, 0 };
		}

		/// @brief 角丸の枠。fill の alpha が 0 なら塗らない。
		void panel(double cx, double cy, double w, double h, const String& text, const String& reading, double textHeight, const ColorF& fill, const ColorF& ink)
		{
			SignElement element;
			element.kind = SignElementKind::Panel; element.text = text; element.reading = reading;
			element.posY = static_cast<float>(cy / m_height); element.boxH = static_cast<float>(h / m_height);
			element.scale = static_cast<float>(textHeight / m_height); element.fill = fill; element.ink = ink;
			m_items << Item{ element, cx, w };
		}

		/// @brief 区分線（白の細い帯）。
		void rule(double x0, double x1, double y) { panel((x0 + x1) * .5, y, x1 - x0, .07, U"", U"", 0, kSignWhite, kSignWhite); }

		/// @brief 出口番号の枠（白地に緑文字）。幅を返す。
		double number(double x, double cy, int value)
		{
			const String text = Format(value);
			const double w = Max(.85, textWidth(text, .5) + .35);
			panel(x + w * .5, cy, w, .72, text, U"", .5, kSignWhite, kSignGreen);
			return w;
		}

		/// @brief 路線番号（緑地に白縁・白文字）と路線名（白地に緑文字、下に英字）。幅を返す。
		double route(double x, double cy, const SignRoute& route)
		{
			const double badge = textWidth(route.number, .5) + .4;
			panel(x + badge * .5, cy, badge, .78, U"", U"", 0, kSignWhite, kSignWhite);
			panel(x + badge * .5, cy, badge - .12, .66, route.number, U"", .5, kSignGreen, kSignWhite);
			const double nameWidth = Max(textWidth(route.name, .5), textWidth(route.reading, .21)) + .4;
			panel(x + badge + .12 + nameWidth * .5, cy, nameWidth, .78, route.name, route.reading, .46, kSignWhite, kSignGreen);
			return badge + .12 + nameWidth;
		}

		/// @brief 距離（数字を大きく、単位を小さく）。x は右端。幅を返す。
		double distance(double right, double cy, double metres)
		{
			const bool kilometres = metres >= 1000;
			const String value = kilometres ? Format(static_cast<int>(Math::Round(metres / 1000))) : Format(static_cast<int>(metres));
			const String unit = kilometres ? U"km" : U"m";
			const double unitWidth = textWidth(unit, .42), valueWidth = textWidth(value, .66);
			text(right, cy + .08, .42, unit, U"", 1);
			text(right - unitWidth - .05, cy, .66, value, U"", 1);
			return unitWidth + .05 + valueWidth;
		}

		/// @param angle 0 = 上、正で時計回り [度]。x, y は矢の根元。
		void arrow(double x, double y, double length, double angle, double width = 1.7)
		{
			SignElement element;
			element.kind = SignElementKind::Arrow;
			element.posY = static_cast<float>(y / m_height);
			element.arrowLength = static_cast<float>(length * GuideSign::kTexturePxPerM);
			element.arrowAngle = static_cast<float>(angle); element.scale = static_cast<float>(width);
			m_items << Item{ element, x, 0 };
		}

		/// @brief 地名を横に並べる（各地名の下に読み）。幅を返す。
		double places(double x, double cy, const Array<SignPlace>& places, double h = .6)
		{
			double cursor = x;
			for (const auto& place : places)
			{
				const double w = Max(textWidth(place.name, h), textWidth(place.reading, h * .42));
				text(cursor + w * .5, cy - h * .25, h, place.name, place.reading, .5f);
				cursor += w + h * .8;
			}
			return places.isEmpty() ? 0.0 : cursor - x - h * .8;
		}

		[[nodiscard]] double height() const { return m_height; }

		[[nodiscard]] Array<SignElement> finish(double width) const
		{
			Array<SignElement> elements;
			for (const auto& item : m_items)
			{
				SignElement element = item.element;
				element.posX = static_cast<float>(item.x / width);
				element.boxW = static_cast<float>(item.w / width);
				elements << element;
			}
			return elements;
		}

	private:
		struct Item { SignElement element; double x = 0, w = 0; };
		double m_height;
		Array<Item> m_items;
	};

	/// @brief 組み上がった板面（幅・高さ [m] と要素）。
	struct SignFace { double width = 0, height = 0; Array<SignElement> elements; };

	constexpr double kSignMargin = .35;

	/// @brief 2km 予告（109 出口の予告）: [番号] IC 名 ・・・ 2km。
	SignFace exitNotice(int number, const SignPlace& exit, double metres)
	{
		BoardBuilder board{ 1.9 };
		double x = kSignMargin;
		if (number > 0) { x += board.number(x, .78, number) + .3; }
		const double nameWidth = Max(BoardBuilder::textWidth(exit.name, .62), BoardBuilder::textWidth(exit.reading, .26));
		board.text(x, .7, .62, exit.name, exit.reading);
		const double distanceWidth = BoardBuilder::textWidth(U"2km", .6) + .2;
		const double width = x + nameWidth + .8 + distanceWidth + kSignMargin;
		board.distance(width - kSignMargin, .78, metres);
		return { width, board.height(), board.finish(width) };
	}

	/// @brief 1km・500m 予告（110 方面及び出口の予告）: 上段 [番号] IC 名 ・・ 出口 距離、下段 行き先。
	/// @param metres 0 なら距離を出さず、出口の矢印（左上）を添える（112 方面及び出口）。
	SignFace exitDirection(int number, const SignPlace& exit, const String& label, const String& labelReading, const Array<SignPlace>& destinations, double metres)
	{
		BoardBuilder board{ 3.4 };
		double x = kSignMargin;
		if (number > 0) { x += board.number(x, .72, number) + .3; }
		const double nameWidth = Max(BoardBuilder::textWidth(exit.name, .58), BoardBuilder::textWidth(exit.reading, .24));
		board.text(x, .64, .58, exit.name, exit.reading);
		const double labelWidth = BoardBuilder::textWidth(label, .52);
		const double distanceWidth = metres > 0 ? BoardBuilder::textWidth(U"500m", .6) + .2 : 0;
		double width = x + nameWidth + .7 + labelWidth + (metres > 0 ? .35 + distanceWidth : 0) + kSignMargin;
		const double arrowSpace = metres > 0 ? 0.0 : 1.5;
		double placesWidth = 0;
		for (const auto& place : destinations) { placesWidth += Max(BoardBuilder::textWidth(place.name, .6), BoardBuilder::textWidth(place.reading, .25)) + .48; }
		width = Max(width, kSignMargin * 2 + arrowSpace + placesWidth + .2);
		const double right = width - kSignMargin;
		double cursor = right;
		if (metres > 0) { cursor -= board.distance(right, .74, metres) + .35; }
		if (!label.isEmpty()) { board.text(cursor, .66, .52, label, labelReading, 1); }
		board.rule(.18, width - .18, 1.52);
		const double rowY = 2.4;
		if (metres <= 0)
		{
			// 出口は左上へ（本線から左へ分かれる）。
			board.arrow(kSignMargin + .55, rowY + .55, 1.25, -45);
		}
		const double placesStart = kSignMargin + arrowSpace;
		board.places(placesStart + Max(0.0, (right - placesStart - placesWidth + .48) * .5), rowY, destinations);
		return { width, board.height(), board.finish(width) };
	}

	/// @brief 本線の方面（111 方面、車線及び出口の予告の本線側）: 上段 路線番号・路線名、下段 行き先と矢印。
	SignFace routeDirection(const SignRoute& route, const Array<SignPlace>& destinations, double arrowAngle, bool arrowLeft = false)
	{
		BoardBuilder board{ 3.4 };
		const double routeWidth = BoardBuilder::textWidth(route.number, .5) + .4 + .12 + Max(BoardBuilder::textWidth(route.name, .5), BoardBuilder::textWidth(route.reading, .21)) + .4;
		double placesWidth = 0;
		for (const auto& place : destinations) { placesWidth += Max(BoardBuilder::textWidth(place.name, .6), BoardBuilder::textWidth(place.reading, .25)) + .48; }
		const double arrowSpace = 1.4;
		const double width = Max(routeWidth, placesWidth + arrowSpace) + kSignMargin * 2 + .2;
		board.route((width - routeWidth) * .5, .72, route);
		board.rule(.18, width - .18, 1.4);
		const double rowY = 2.35;
		const double arrowX = arrowLeft ? kSignMargin + .55 : width - kSignMargin - .55;
		const double arrowBase = rowY + .6 + (Abs(arrowAngle) > 1 ? -.05 : 0);
		board.arrow(arrowX + (arrowAngle > 1 ? -.35 : arrowAngle < -1 ? .35 : 0), arrowBase, 1.25, arrowAngle);
		const double placesStart = arrowLeft ? kSignMargin + arrowSpace : kSignMargin;
		const double placesEnd = arrowLeft ? width - kSignMargin : width - kSignMargin - arrowSpace;
		board.places(placesStart + Max(0.0, (placesEnd - placesStart - placesWidth + .48) * .5), rowY, destinations);
		return { width, board.height(), board.finish(width) };
	}

	/// @brief 方面及び距離（106 の高速道路版）: 行ごとに [番号] 地名 ・・・ 距離。
	SignFace distanceList(const Array<std::tuple<int, SignPlace, double>>& rows)
	{
		const double rowHeight = 1.05;
		BoardBuilder board{ rows.size() * rowHeight + .35 };
		double nameWidth = 0;
		for (const auto& [number, place, metres] : rows) { nameWidth = Max(nameWidth, Max(BoardBuilder::textWidth(place.name, .55), BoardBuilder::textWidth(place.reading, .23))); }
		const double numberWidth = .85 + .3;
		const double width = kSignMargin + numberWidth + nameWidth + .9 + BoardBuilder::textWidth(U"100km", .6) + kSignMargin;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const auto& [number, place, metres] = rows[i];
			const double cy = .2 + rowHeight * (i + .5);
			if (number > 0) { board.number(kSignMargin, cy, number); }
			board.text(kSignMargin + numberWidth, cy - .12, .55, place.name, place.reading);
			board.distance(width - kSignMargin, cy, Max(1000.0, Math::Round(metres / 1000) * 1000));
		}
		return { width, board.height(), board.finish(width) };
	}

	/// @brief 入口の方向（103）: 上段 白地に路線番号・路線名、下段 緑地に「入口」と IC 名、矢印。
	SignFace entranceDirection(const SignRoute& route, const SignPlace& entrance)
	{
		BoardBuilder board{ 3.0 };
		const double routeWidth = BoardBuilder::textWidth(route.number, .5) + .4 + .12 + Max(BoardBuilder::textWidth(route.name, .5), BoardBuilder::textWidth(route.reading, .21)) + .4;
		const double lowerWidth = 1.4 + BoardBuilder::textWidth(U"入口", .55) + .5 + Max(BoardBuilder::textWidth(entrance.name, .55), BoardBuilder::textWidth(entrance.reading, .23));
		const double width = Max(routeWidth + .8, lowerWidth) + kSignMargin * 2;
		board.panel(width * .5, .7, width - .3, 1.15, U"", U"", 0, kSignWhite, kSignWhite);
		board.route((width - routeWidth) * .5, .7, route);
		board.arrow(kSignMargin + .55, 2.75, 1.2, 0);
		board.text(kSignMargin + 1.4, 1.95, .55, U"入口", U"ENTRANCE");
		board.text(kSignMargin + 1.4 + BoardBuilder::textWidth(U"入口", .55) + .5, 1.95, .55, entrance.name, entrance.reading);
		return { width, board.height(), board.finish(width) };
	}

	/// @brief 案内標識を置く。overhead なら lateral は板の中心、gantryLeft/Right は門型の柱（両方 0 なら隣の板の門型に掛ける）。
	int placeFace(RoadNetwork& roads, int edgeId, int nodeEnd, double arc, const SignFace& face, float lateral,
		bool overhead = false, float gantryLeft = 0, float gantryRight = 0)
	{
		if (!roads.getEdge(edgeId) || face.elements.isEmpty()) { return -1; }
		GuideSignPlacement sign;
		sign.parentEdgeId = edgeId;
		sign.kind = GuideSignKind::DirectionDistance;
		sign.nodeEndId = nodeEnd;
		sign.arcOffset = static_cast<float>(arc);
		sign.lateralOffset = lateral;
		sign.poleHeight = settings().expressway_signPoleHeight;
		sign.autoGenerated = false;
		sign.bgColor = kSignGreen;
		sign.showReading = true;
		sign.widthOverride = static_cast<float>(face.width);
		sign.heightOverride = static_cast<float>(face.height);
		sign.elements = face.elements;
		sign.overhead = overhead;
		sign.gantryLeft = gantryLeft;
		sign.gantryRight = gantryRight;
		return roads.addGuideSign(sign);
	}

	/// @brief 走行距離 arc の位置へ路側（左）の片持ち標識を置く。
	int placeRoadside(RoadNetwork& roads, const Carriageway& road, double arc, const SignFace& face)
	{
		if (arc < 20 || arc > road.arcs.back() - 20) { return 0; }
		const auto& c = settings();
		const auto [edge, local] = locate(road, arc);
		const float lateral = -(c.expressway_lanesPerDirection * c.expressway_laneWidth * .5f + c.expressway_outerShoulder + c.expressway_guardrailWidth + c.expressway_signLateralMargin);
		return placeFace(roads, edge, roads.getEdge(edge)->nodeA, local, face, lateral) >= 0;
	}

	/// @brief 走行距離 arc の位置へ門型を立て、出口側（左）と本線側（右）の板を掛ける。auxiliary はその位置の変速車線の幅。
	int placeGantry(RoadNetwork& roads, const Carriageway& road, double arc, const SignFace& exitFace, const SignFace& throughFace, float auxiliary)
	{
		if (arc < 20 || arc > road.arcs.back() - 20) { return 0; }
		const auto& c = settings();
		const auto [edge, local] = locate(road, arc);
		const int nodeEnd = roads.getEdge(edge)->nodeA;
		const float half = c.expressway_lanesPerDirection * c.expressway_laneWidth * .5f;
		const float left = -(half + auxiliary + c.expressway_outerShoulder + c.expressway_guardrailWidth + .6f);
		float right = half + c.expressway_innerShoulder + c.expressway_guardrailWidth + .8f;
		// 出口の板は左端の車線（変速車線があればその上）、本線の板はその右隣。
		const float exitWidth = static_cast<float>(exitFace.width), throughWidth = static_cast<float>(throughFace.width);
		float exitCenter = auxiliary > 0 ? -(half + auxiliary * .5f) : -(half - c.expressway_laneWidth * .5f);
		exitCenter = Max(exitCenter, left + .4f + exitWidth * .5f);
		const float throughCenter = Max(exitCenter + exitWidth * .5f + .4f + throughWidth * .5f, (exitCenter + exitWidth * .5f + right) * .5f);
		right = Max(right, throughCenter + throughWidth * .5f + .5f);
		int count = placeFace(roads, edge, nodeEnd, local, exitFace, exitCenter, true, left, right) >= 0;
		count += placeFace(roads, edge, nodeEnd, local, throughFace, throughCenter, true) >= 0;
		return count;
	}

	/// @brief 車道の station を走行距離へ直す（節点の間は線形補間）。
	double arcAtStation(const Carriageway& road, double station)
	{
		for (size_t i = 0; i + 1 < road.stations.size(); ++i)
		{
			const double a = road.stations[i], b = road.stations[i + 1];
			if ((station - a) * (station - b) <= 0 && Abs(b - a) > 1e-6) { return Math::Lerp(road.arcs[i], road.arcs[i + 1], (station - a) / (b - a)); }
		}
		return Abs(station - road.stations.front()) < Abs(station - road.stations.back()) ? road.arcs.front() : road.arcs.back();
	}

	/// @brief 分岐（IC の出口・PA/SA・JCT）の予告に使う情報。
	struct ExitSigns
	{
		size_t carriageway = 0;
		int index = -1;                 ///< 分流点の節点の添字
		int number = 0;                 ///< 出口番号（PA/SA・JCT は 0）
		SignPlace exit;                 ///< IC・PA/SA・JCT の名称
		String label, labelReading;     ///< 上段右の表示（IC は「出口 EXIT」）
		Array<SignPlace> destinations;  ///< 出口の先の行き先
		Optional<SignRoute> branch;     ///< JCT の分岐先の路線
	};

	/// @brief 分岐点の手前 2km・1km（路側）、500m・出口（門型、本線の方面を併記）の予告標識。
	/// @details 手前の別の分岐を越える位置には置かない（CSL: junction を跨がない）。
	int addExitSigns(RoadNetwork& roads, const Carriageway& road, const ExitSigns& exit, const HashSet<int>& otherDiverges,
		const SignRoute& route, const Array<SignPlace>& through)
	{
		const auto& c = settings();
		int count = 0;
		const double target = road.arcs[exit.index];
		const auto crosses = [&](double arc)
		{
			return std::any_of(otherDiverges.begin(), otherDiverges.end(), [&](int other) { return other != exit.index && road.arcs[other] > arc && road.arcs[other] < target; });
		};
		const auto exitFace = [&](double metres)
		{
			if (exit.branch) { return routeDirection(*exit.branch, exit.destinations, metres > 0 ? 0.0 : -45.0, true); }
			return exitDirection(exit.number, exit.exit, exit.label, exit.labelReading, exit.destinations, metres);
		};
		if (!crosses(target - 2000)) { count += placeRoadside(roads, road, target - 2000, exitNotice(exit.number, exit.exit, 2000)); }
		if (!crosses(target - 1000)) { count += placeRoadside(roads, road, target - 1000, exit.branch ? exitFace(1000) : exitDirection(exit.number, exit.exit, exit.label, exit.labelReading, exit.destinations, 1000)); }
		if (!crosses(target - 500)) { count += placeGantry(roads, road, target - 500, exit.branch ? exitFace(500) : exitDirection(exit.number, exit.exit, exit.label, exit.labelReading, exit.destinations, 500), routeDirection(route, through, 0), 0); }
		// 出口（分岐点の案内）は減速車線の上。減速車線が全幅になった区間に門型を立てる。
		const double lane = c.expressway_decelerationLaneLength;
		const double action = target - (lane > 0 ? Min(lane * .8, 100.0) : 120.0);
		if (!crosses(action)) { count += placeGantry(roads, road, action, exitFace(0), routeDirection(route, through, 0), lane > 0 ? c.expressway_laneWidth : 0.0f); }
		return count;
	}

	/// @brief 分流鼻に置く 2 眼点滅灯。ランプと本線の外縁が離れた位置を探す。
	void addDivergeBlinker(RoadNetwork& roads, int rampEdge, Vec2 mainlineDirection, Vec2 mainlinePoint)
	{
		const auto curve = roads.getBezier(rampEdge); if (!curve) { return; }
		const auto& c = settings();
		const double needed = c.expressway_rampLaneWidth * .5 + c.expressway_rampInnerShoulder + c.expressway_lanesPerDirection * c.expressway_laneWidth * .5 + c.expressway_outerShoulder + 1.2;
		for (float arc = 20; arc < curve->totalLength * .6f; arc += 4)
		{
			const Vec2 point = flat(curve->positionAt(arc));
			const Vec2 offset = point - mainlinePoint;
			const double lateral = Abs(offset.x * mainlineDirection.y - offset.y * mainlineDirection.x);
			if (lateral < needed) { continue; }
			RoadObject blinker;
			blinker.parentEdgeId = rampEdge; blinker.arcPos = arc;
			blinker.lateralOffset = static_cast<float>(c.expressway_rampLaneWidth * .5 + c.expressway_rampInnerShoulder + .9);
			blinker.type = RoadObjectType::DivergeBlinker;
			roads.addObject(blinker);
			return;
		}
	}

	/// @brief 取付道路（JP Bypass 24m）の 1 区間。A→B の向きで 2+1 の追加車線が B 側へ向く。
	Optional<int> addBypass(RoadNetwork& roads, int from, int to, ExpresswayCrossSection::BypassVariant variant, Array<int>& created)
	{
		const Vec3 a = roads.getNode(from)->position, b = roads.getNode(to)->position;
		const auto id = roads.addEdge(from, to, a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::Arterial, 2);
		if (!id) { return none; }
		auto* edge = roads.getEdge(*id);
		edge->edgeState = EdgeState::Open;
		ExpresswayCrossSection::applyBypass(*edge, variant);
		roads.rebuildNodeConnectivity(from, to);
		created << *id;
		return id;
	}

	/// @brief 線形と自動車専用道の 2D 交差（取付道路が本線を平面で横切らないかの確認）。
	bool crossesSegments(const Array<CubicBezier>& curves, const Array<std::pair<Vec2, Vec2>>& segments)
	{
		for (const auto& curve : curves)
		{
			for (float arc = 0; arc < curve.totalLength; arc += 10)
			{
				const Vec2 a = flat(curve.positionAt(arc)), b = flat(curve.positionAt(Min(curve.totalLength, arc + 10)));
				for (const auto& [c, d] : segments)
				{
					const Vec2 r = b - a, s = d - c; const double denominator = r.x * s.y - r.y * s.x;
					if (Abs(denominator) < 1e-9) { continue; }
					const Vec2 q = c - a;
					const double t = (q.x * s.y - q.y * s.x) / denominator, u = (q.x * r.y - q.y * r.x) / denominator;
					if (t >= 0 && t <= 1 && u >= 0 && u <= 1) { return true; }
				}
			}
		}
		return false;
	}

	struct Interchange { double station = 0; int town = -1; String name; String reading; };

	/// @brief トランペット型 IC の寸法（局所座標: x は遠い側の車道の進行方向、y は町から離れる向き）。
	struct TrumpetLayout
	{
		double radius = 60, loopLift = 5, southboundX = 5, stemSouth = 250, bridgeApex = 9.2, stemGrade = .07;
		double onRadius = 110; ///< 遠い側の流入ランプがループの外を回る円弧の半径（ランプの設計速度の最小曲線半径以上）
		double loopLead = -170, farMerge = 430, nearRamp = 380;
		Vec2 loopCenter{ 0, 0 };
		/// @brief 本線の station 方向に IC が占める範囲（中心からの最大距離）。
		[[nodiscard]] double reach() const { return Max({ -loopLead, farMerge, nearRamp }); }
		/// @brief 変速車線（減速・加速車線とすり付け）まで含めた範囲。
		[[nodiscard]] double auxiliaryReach() const
		{
			const auto& c = GenerationSettings::get();
			const double taper = c.expressway_auxiliaryTaperLength, decelerate = c.expressway_decelerationLaneLength, accelerate = c.expressway_accelerationLaneLength;
			return Max({ -loopLead + decelerate + taper, farMerge + accelerate + taper, nearRamp + Max(decelerate, accelerate) + taper });
		}
		static TrumpetLayout make()
		{
			const auto& c = GenerationSettings::get();
			TrumpetLayout layout;
			layout.radius = Max(c.expressway_trumpetLoopRadius, ExpresswayNetwork::minimumCurveRadius(c.expressway_trumpetLoopSpeed));
			layout.onRadius = ExpresswayNetwork::minimumCurveRadius(c.expressway_rampSpeed) * 1.1;
			layout.loopLift = c.expressway_trumpetLoopLift;
			layout.stemSouth = c.expressway_trumpetStemLength; layout.bridgeApex = c.expressway_trumpetBridgeApex;
			layout.stemGrade = c.expressway_rampMaximumGrade * 1.12;
			layout.loopLead = -c.expressway_trumpetLoopLead; layout.farMerge = c.expressway_trumpetFarMerge; layout.nearRamp = c.expressway_trumpetNearRamp;
			layout.loopCenter = Vec2{ layout.southboundX + layout.radius, ExpresswayCrossSection::carriagewayOffset() + 12 + layout.radius };
			return layout;
		}
	};

	struct Junction
	{
		double station = 0;
		int side = 1;     ///< 支線へ分流する近い側の車道（+1 = 中心線左の station 増加方向）
		Vec2 origin, direction;
		int target = -1;
		String name, reading;
	};

	/// @brief トランペット型 IC の各ランプ曲線と、勾配・交差・本線建築限界の判定結果。
	struct TrumpetGeometry
	{
		struct Piece { CubicBezier curve; String key; };
		TrumpetLayout plan;
		double station = 0, base = 0;
		int townSide = 1;  ///< 町が中心線の右（+1）か左（-1）か
		Vec2 center, ey, farTravel, nearTravel, plazaPoint;
		Vec3 loopEntry, loopRight, loopTop, southboundTop, northboundTop, overLoop, southboundFoot, northboundFoot, plazaStart;
		Array<Piece> pieces;
		String failure;    ///< 空なら建設できる
	};

	/// @brief 車道上の分合流点（station と車道の向き）を返す。節点がなければ none。
	using WayPoint = std::function<Optional<Vec3>(double station, int side)>;

	/// @brief station s の町側に置くトランペット型 IC の形を作り、建設できるか判定する。縦断を解いた後に使う。
	/// @details IC 選定時は車道の計画位置、建設時は実際の節点位置で同じ判定をする。
	TrumpetGeometry trumpetGeometry(const Line& line, double s, Vec2 townCenter, const WayPoint& wayPoint)
	{
		const auto& c = settings();
		TrumpetGeometry g;
		const auto& plan = g.plan = TrumpetLayout::make();
		g.station = s;
		const Vec2 center = g.center = line.at(s), t = line.tangent(s), right = rightOf(t);
		const int townSide = g.townSide = (townCenter - center).dot(right) >= 0 ? 1 : -1;
		// 局所座標: x は遠い側の車道の進行方向、y は町から離れる向き。遠い側の車道は y=+offset にある。
		const Vec2 ex = t * townSide, ey = g.ey = -right * townSide;
		const double base = g.base = line.height(s);
		const auto local = [&](double x, double y, double h) { return lift(center + ex * x + ey * y, base + h); };
		const auto heading = [&](double x, double y) { return ex * x + ey * y; };
		const auto stationOf = [&](double x) { return s + townSide * x; };
		// 遠い側の車道は townSide の向き（+1 = station 増加方向の車道）、近い側はその逆。
		const auto farDiverge = wayPoint(stationOf(plan.loopLead), townSide), farMerge = wayPoint(stationOf(plan.farMerge), townSide);
		const auto nearDiverge = wayPoint(stationOf(plan.nearRamp), -townSide), nearMerge = wayPoint(stationOf(-plan.nearRamp), -townSide);
		if (!farDiverge || !farMerge || !nearDiverge || !nearMerge) { g.failure = U"missing-node"; return g; }
		const auto position = [](const Optional<Vec3>& point) { return *point; };
		const Vec2 farTravel = g.farTravel = line.tangent(stationOf(plan.loopLead)) * townSide, farTravelMerge = line.tangent(stationOf(plan.farMerge)) * townSide;
		const Vec2 nearTravel = g.nearTravel = -line.tangent(stationOf(plan.nearRamp)) * townSide, nearTravelMerge = -line.tangent(stationOf(-plan.nearRamp)) * townSide;
		const double angle = c.expressway_rampDivergeAngle;
		// 各点（局所座標）。
		const Vec3 loopEntry = g.loopEntry = local(plan.loopCenter.x, plan.loopCenter.y - plan.radius, 0);
		const Vec3 loopRight = g.loopRight = local(plan.loopCenter.x + plan.radius, plan.loopCenter.y, plan.loopLift * .33);
		const Vec3 loopTop = g.loopTop = local(plan.loopCenter.x, plan.loopCenter.y + plan.radius, plan.loopLift * .66);
		const Vec3 southboundTop = g.southboundTop = local(plan.southboundX, plan.loopCenter.y, plan.loopLift);
		const Vec3 northboundTop = g.northboundTop = local(-plan.southboundX, plan.loopCenter.y, plan.loopLift);
		// 流入ランプはループの外側を半径 onRadius の 1/4 円で回る。
		const Vec3 overLoop = g.overLoop = local(-plan.southboundX + plan.onRadius, plan.loopCenter.y + plan.onRadius, plan.loopLift);
		const Vec3 southboundFoot = g.southboundFoot = local(plan.southboundX, -plan.stemSouth, 0);
		const Vec3 northboundFoot = g.northboundFoot = local(-plan.southboundX, -plan.stemSouth, 0);
		g.plazaPoint = center + ey * -(plan.stemSouth + 30);
		g.plazaStart = lift(g.plazaPoint, base);
		const auto arcCurve = [&](Vec3 from, Vec2 fromDirection, Vec3 to, Vec2 toDirection, double radius = 0)
		{
			const double arcHandle = .5523 * (radius > 0 ? radius : plan.radius);
			return CubicBezier{ from, lift(flat(from) + fromDirection * arcHandle, Math::Lerp(from.y, to.y, 1.0 / 3)), lift(flat(to) - toDirection * arcHandle, Math::Lerp(from.y, to.y, 2.0 / 3)), to };
		};
		const auto handled = [&](Vec3 from, Vec2 a, Vec3 to, Vec2 b, double factor, Optional<double> firstHeight = none, Optional<double> secondHeight = none)
		{
			const double handle = flat(to - from).length() * factor;
			return CubicBezier{ from, lift(flat(from) + a.normalized() * handle, firstHeight.value_or(Math::Lerp(from.y, to.y, 1.0 / 3))),
				lift(flat(to) - b.normalized() * handle, secondHeight.value_or(Math::Lerp(from.y, to.y, 2.0 / 3))), to };
		};
		auto& pieces = g.pieces;
		pieces = Array<TrumpetGeometry::Piece>{
			{ handled(position(farDiverge), farTravel - rightOf(farTravel) * angle, loopEntry, heading(1, 0), .38), U"farLead" },
			{ arcCurve(loopEntry, heading(1, 0), loopRight, heading(0, 1)), U"loop1" },
			{ arcCurve(loopRight, heading(0, 1), loopTop, heading(-1, 0)), U"loop2" },
			{ arcCurve(loopTop, heading(-1, 0), southboundTop, heading(0, -1)), U"loop3" },
			{ handled(southboundTop, heading(0, -1), southboundFoot, heading(0, -1), .38, base + plan.bridgeApex + 1.5, base + plan.bridgeApex - 1), U"southbound" },
			{ handled(northboundFoot, heading(0, 1), northboundTop, heading(0, 1), .38, base + plan.bridgeApex - 1, base + plan.bridgeApex + 1.5), U"northbound" },
			{ arcCurve(northboundTop, heading(0, 1), overLoop, heading(1, 0), plan.onRadius), U"farOn1" },
			{ handled(overLoop, heading(1, 0), position(farMerge), farTravelMerge + rightOf(farTravelMerge) * angle, .38), U"farOn2" },
			{ handled(position(nearDiverge), nearTravel - rightOf(nearTravel) * angle, southboundFoot, heading(0, -1), .5), U"nearOff" },
			{ handled(northboundFoot, heading(0, 1), position(nearMerge), nearTravelMerge + rightOf(nearTravelMerge) * angle, .5), U"nearOn" },
		};
		// 接続する組を除き、平面で重なる区間は 6m 以上の高低差を取る。跨道部は本線の建築限界を守る。
		const HashSet<String> joined{ U"farLead|loop1", U"loop1|loop2", U"loop2|loop3", U"loop3|southbound", U"northbound|farOn1", U"farOn1|farOn2",
			U"southbound|nearOff", U"northbound|nearOn", U"loop3|farOn1", U"southbound|northbound", U"nearOff|nearOn", U"southbound|farOn1", U"loop3|northbound" };
		String& failure = g.failure;
		// 曲線半径は各ランプの設計速度の最小値（ループは trumpetLoopSpeed、他は rampSpeed）。ベジェ近似の誤差 2% は許す。
		for (const auto& piece : pieces)
		{
			const double speed = piece.key.starts_with(U"loop") ? c.expressway_trumpetLoopSpeed : c.expressway_rampSpeed;
			if (piece.curve.minimumHorizontalRadius() < ExpresswayNetwork::minimumCurveRadius(speed) * .98) { failure = U"radius:" + piece.key; break; }
		}
		for (const auto& piece : pieces)
		{
			if (!failure.isEmpty()) { break; }
			const bool stem = piece.key == U"southbound" || piece.key == U"northbound";
			if (maximumGrade(piece.curve) > (stem ? plan.stemGrade : c.expressway_rampMaximumGrade * 1.05)) { failure = U"grade:" + piece.key; break; }
		}
		for (size_t i = 0; failure.isEmpty() && i < pieces.size(); ++i)
		{
			for (size_t j = i + 1; failure.isEmpty() && j < pieces.size(); ++j)
			{
				if (joined.contains(pieces[i].key + U"|" + pieces[j].key) || joined.contains(pieces[j].key + U"|" + pieces[i].key)) { continue; }
				for (float a = 0; failure.isEmpty() && a <= pieces[i].curve.totalLength; a += 3)
				{
					const Vec3 p = pieces[i].curve.positionAt(a);
					for (float b = 0; b <= pieces[j].curve.totalLength; b += 3)
					{
						const Vec3 q = pieces[j].curve.positionAt(b);
						if (flat(p).distanceFrom(flat(q)) < 5.5 && Abs(p.y - q.y) < c.expressway_junctionCrossingClearance) { failure = U"crossing:" + pieces[i].key + U"/" + pieces[j].key; break; }
					}
				}
			}
		}
		for (const auto& piece : pieces)
		{
			if (!failure.isEmpty() || (piece.key != U"southbound" && piece.key != U"northbound")) { continue; }
			for (float a = 0; a <= piece.curve.totalLength; a += 3)
			{
				const Vec3 p = piece.curve.positionAt(a);
				const double station = line.nearest(flat(p)).first;
				const double lateral = Abs((flat(p) - line.at(station)).dot(rightOf(line.tangent(station))));
				if (lateral < ExpresswayCrossSection::carriagewayOffset() + 7 && p.y - line.height(station) < c.expressway_junctionCrossingClearance) { failure = U"mainline:" + piece.key; break; }
			}
		}
		return g;
	}

	/// @brief 縦断を解いた中心線から、車道の計画上の点を返す。
	WayPoint plannedWay(const Line& line)
	{
		return [&line](double station, int side) -> Optional<Vec3> { return carriagewayPoint(line, station, side, {}); };
	}

	/// @brief 1 路線分の生成データ。
	struct Route
	{
		Line line;
		Array<Interchange> interchanges;
		Array<Interchange> services;   ///< PA/SA（station・最寄りの町・名称）
		Optional<Junction> branch;     ///< この路線から分かれる JCT（本線側）
		bool startsAtJunction = false; ///< 支線。始点の上下線は高さを分ける
		Carriageway forward, backward;
		String name;
		int number = 0;
		int primaryTown = -1;
	};

	String interchangeName(const Settlement& town, StringView suffix) { return ExpresswayNetwork::routeBaseName(town.name) + String{ suffix }; }
	String interchangeReading(const Settlement& town, StringView suffix) { return town.reading.isEmpty() ? String{} : town.reading + U" " + String{ suffix }; }

	/// @brief IC の敷地（町側: 連絡路・料金所・取付道路、反対側: ループ）に一般道が入らないか。
	bool interchangeSiteFree(const Line& line, double s, Vec2 townCenter, const TransportClearance& crossings)
	{
		const auto& c = settings();
		const Vec2 right = rightOf(line.tangent(s));
		const double townSide = (townCenter - line.at(s)).dot(right) >= 0 ? 1.0 : -1.0;
		const auto layout = TrumpetLayout::make();
		const double townReach = layout.stemSouth + 30 + c.expressway_tollStemLength + c.expressway_accessLength;
		const double farReach = layout.loopCenter.y + layout.radius + 90;
		for (double along = -layout.reach(); along <= layout.reach(); along += 40)
		{
			const Vec2 point = line.at(Clamp(s + along, 0.0, line.length()));
			for (double lateral = -farReach; lateral <= townReach; lateral += 35)
			{
				// 町側の連絡路は中心付近だけ、本線沿いは全長を調べる。
				if (lateral > 40 && Abs(along) > 140) { continue; }
				if (crossings.interval(point + right * (lateral * townSide), 12).second > -1e8) { return false; }
			}
		}
		return true;
	}

	/// @brief 路線沿いの町へ IC を割り当てる。間隔・端部・JCT からの離隔を守る。
	Array<Interchange> chooseInterchanges(const Line& line, const Array<Settlement>& towns, const Array<double>& avoidStations, bool lineEndsAtBorder,
		const TransportClearance& crossings)
	{
		const auto& c = settings();
		struct Candidate { double station; double distance; int town; int priority; };
		Array<Candidate> candidates;
		for (size_t i = 0; i < towns.size(); ++i)
		{
			const auto& town = towns[i];
			if (town.kind == MapGenerator::SettlementKind::RuralSettlement) { continue; }
			const auto [station, distance] = line.nearest(town.center);
			if (distance > c.expressway_interchangeTownDistance + town.radius) { continue; }
			candidates << Candidate{ station, distance, static_cast<int>(i), town.kind == MapGenerator::SettlementKind::RegionalCity ? 0 : 1 };
		}
		candidates.sort_by([](const Candidate& a, const Candidate& b) { return a.priority != b.priority ? a.priority < b.priority : a.distance < b.distance; });
		Array<Interchange> result;
		const double endMargin = c.expressway_interchangeEndMargin + TrumpetLayout::make().auxiliaryReach();
		for (const auto& candidate : candidates)
		{
			// 地図端で終わらない路線は末端 IC を残すため、終点側の余裕を短くしない。
			const double upper = line.length() - (lineEndsAtBorder ? endMargin : TrumpetLayout::make().auxiliaryReach() + 200);
			// 取付道路と本線の跨道部が既存道路と重ならない位置まで、IC を路線方向へずらす。
			// IC の敷地（町側: 連絡路・料金所・取付道路、反対側: ループ）に一般道が入らない位置を探す。
			const auto free = [&](double s) { return interchangeSiteFree(line, s, towns[candidate.town].center, crossings); };
			double station = Clamp(candidate.station, endMargin, upper);
			bool placed = false;
			for (double shift = 0; shift <= 1200 && !placed; shift += 100)
			{
				for (const double sign : { 1.0, -1.0 })
				{
					const double trial = candidate.station + shift * sign;
					if (trial < endMargin || trial > upper || !free(trial)) { continue; }
					station = trial; placed = true; break;
				}
			}
			if (!placed) { continue; }
			if (station <= endMargin - 1 && line.length() < endMargin * 2) { continue; }
			if (result.any([&](const Interchange& other) { return Abs(other.station - station) < c.expressway_interchangeSpacing; })) { continue; }
			if (avoidStations.any([&](double other) { return Abs(other - station) < c.expressway_junctionInterchangeSpacing; })) { continue; }
			result << Interchange{ station, candidate.town, interchangeName(towns[candidate.town], U"IC"), interchangeReading(towns[candidate.town], U"IC") };
		}
		result.sort_by([](const Interchange& a, const Interchange& b) { return a.station < b.station; });
		return result;
	}

	/// @brief IC 区間の縦断拘束: 本線を地盤付近に保つ。
	void addInterchangeConstraints(Array<Constraint>& rules, const Array<Interchange>& interchanges)
	{
		for (const auto& ic : interchanges)
		{
			// トランペット型は本線を地盤付近に保ち、連絡路の跨道橋とループを地盤から組み立てる。
			const double reach = TrumpetLayout::make().reach() + 120;
			rules << Constraint{ ic.station - reach, ic.station + reach, -2.5, 2.5 };
		}
	}

	/// @brief IC 間の長い区間の中央付近に PA/SA を置く。縦断が地盤付近で、周囲に一般道がない位置に限る。
	Array<Interchange> chooseServices(const Line& line, const World& world, const TransportClearance& crossings, const Array<Settlement>& towns,
		const Array<double>& occupiedStations)
	{
		const auto& c = settings();
		Array<double> stops = occupiedStations;
		stops << 0.0 << line.length();
		stops.sort();
		Array<Interchange> result;
		for (size_t i = 0; i + 1 < stops.size(); ++i)
		{
			const double gap = stops[i + 1] - stops[i];
			if (gap < c.expressway_serviceMinimumGap) { continue; }
			const double half = c.expressway_serviceLength * .5 + 120;
			const auto suitable = [&](double s)
			{
				if (s - stops[i] < c.expressway_serviceClearance || stops[i + 1] - s < c.expressway_serviceClearance) { return false; }
				for (double along = -half; along <= half; along += 30)
				{
					const double station = Clamp(s + along, 0.0, line.length());
					const Vec2 point = line.at(station), right = rightOf(line.tangent(station));
					if (Abs(line.height(station) - groundAt(world, point)) > 2.5) { return false; }
					for (const double lateral : { -c.expressway_serviceOffset - 40, 0.0, c.expressway_serviceOffset + 40 })
					{
						const Vec2 probe = point + right * lateral;
						if (crossings.interval(probe, 10).second > -1e8 || !landAt(world, probe)) { return false; }
					}
				}
				return true;
			};
			const double middle = (stops[i] + stops[i + 1]) * .5;
			for (double shift = 0; shift <= gap * .3; shift += 150)
			{
				Optional<double> found;
				for (const double sign : { 1.0, -1.0 }) { if (suitable(middle + shift * sign)) { found = middle + shift * sign; break; } }
				if (!found) { continue; }
				const Vec2 point = line.at(*found);
				int nearest = 0; double best = Math::Inf;
				for (size_t t = 0; t < towns.size(); ++t) { const double d = towns[t].center.distanceFromSq(point); if (d < best) { best = d; nearest = static_cast<int>(t); } }
				// 長い区間は休憩施設の大きい SA、それ以外は PA（NEXCO の配置間隔の目安に合わせる）。
				const bool large = gap >= c.expressway_serviceAreaGap;
				result << Interchange{ *found, nearest, interchangeName(towns[nearest], large ? U"SA" : U"PA"), interchangeReading(towns[nearest], large ? U"SA" : U"PA") };
				break;
			}
		}
		return result;
	}

	/// @brief 全 IC の拘束で縦断が成立しなければ、成立する IC だけを順に残す。
	bool solveWithInterchanges(Line& line, const World& world, const TransportClearance& crossings, double start, double end,
		const Array<Constraint>& base, Array<Interchange>& interchanges, double grade)
	{
		const auto attempt = [&](const Array<Interchange>& kept)
		{
			Array<Constraint> rules = base;
			addInterchangeConstraints(rules, kept);
			return solveProfile(line, world, crossings, start, end, rules, grade);
		};
		if (attempt(interchanges)) { return true; }
		Array<Interchange> kept;
		for (const auto& ic : interchanges)
		{
			auto trial = kept; trial << ic;
			if (attempt(trial)) { kept = std::move(trial); }
			else { DBG_LOG(U"[ExpresswayInterchange] dropped name={} station={:.0f} reason=profile"_fmt(ic.name, ic.station)); }
		}
		interchanges = std::move(kept);
		return attempt(interchanges);
	}

	/// @brief 縦断を解いた後、ランプの勾配・交差・本線の建築限界を満たさない IC を外す。
	void dropUnbuildableInterchanges(const Line& line, const Array<Settlement>& towns, Array<Interchange>& interchanges)
	{
		interchanges.remove_if([&](const Interchange& ic)
		{
			const auto geometry = trumpetGeometry(line, ic.station, towns[ic.town].center, plannedWay(line));
			if (geometry.failure.isEmpty()) { return false; }
			DBG_LOG(U"[ExpresswayInterchange] dropped name={} station={:.0f} reason={}"_fmt(ic.name, ic.station, geometry.failure));
			return true;
		});
	}

	/// @brief IC が 1 つも残らなかった本線に、建設できる IC を 1 か所探して加える。
	/// @details 町に近い位置から順に、敷地の一般道 → 縦断 → ランプ形状の順で確かめる。
	///   1 巡目は敷地に一般道が入らない位置だけ、2 巡目は一般道との重なりを許す（取付道路は既存道路へ接続する）。
	///   成立した場合は line の縦断をその IC の拘束で解き直した状態で返す。
	bool addFallbackInterchange(Line& line, const World& world, const TransportClearance& crossings, const Array<Settlement>& towns,
		const Array<double>& avoidStations, double start, double end, const Array<Constraint>& base, double grade, Array<Interchange>& interchanges)
	{
		const auto& c = settings();
		if (towns.isEmpty()) { return false; }
		const double margin = TrumpetLayout::make().reach() + 300;
		if (line.length() < margin * 2) { return false; }
		const auto nearestTown = [&](Vec2 point)
		{
			int nearest = 0; double best = Math::Inf;
			for (size_t i = 0; i < towns.size(); ++i)
			{
				// 農村集落より町を優先して名前と取付先にする。
				const double weight = towns[i].kind == MapGenerator::SettlementKind::RuralSettlement ? 2.0 : 1.0;
				const double d = towns[i].center.distanceFrom(point) * weight;
				if (d < best) { best = d; nearest = static_cast<int>(i); }
			}
			return nearest;
		};
		struct Candidate { double station; int town; };
		Array<Candidate> candidates;
		const auto push = [&](double station, int town)
		{
			if (station < margin || station > line.length() - margin) { return; }
			if (avoidStations.any([&](double other) { return Abs(other - station) < c.expressway_junctionInterchangeSpacing; })) { return; }
			if (candidates.any([&](const Candidate& other) { return Abs(other.station - station) < 60; })) { return; }
			candidates << Candidate{ station, town };
		};
		// 町に近い順（農村集落は後回し）に、最寄りの station から前後へずらした位置。
		Array<int> order;
		for (size_t i = 0; i < towns.size(); ++i) { order << static_cast<int>(i); }
		const auto rank = [&](int i)
		{
			const double distance = Max(0.0, line.nearest(towns[i].center).second - towns[i].radius);
			return distance * (towns[i].kind == MapGenerator::SettlementKind::RuralSettlement ? 3.0 : 1.0);
		};
		order.sort_by([&](int a, int b) { return rank(a) < rank(b); });
		for (const int town : order)
		{
			const double station = line.nearest(towns[town].center).first;
			for (double shift = 0; shift <= 3000; shift += 150)
			{
				push(station + shift, town);
				push(station - shift, town);
			}
		}
		// どの町からも遠い路線でも、本線上を等間隔に調べる。
		for (double station = margin; station <= line.length() - margin; station += 300) { push(station, nearestTown(line.at(station))); }
		const Array<double> solvedHeights = line.heights;
		int solves = 0;
		constexpr int kMaximumSolves = 24;
		for (const bool strict : { true, false })
		{
			for (const auto& candidate : candidates)
			{
				const Vec2 townCenter = towns[candidate.town].center;
				if (strict && !interchangeSiteFree(line, candidate.station, townCenter, crossings)) { continue; }
				// 現在の縦断で形が成立しない位置は、解き直しても成立しにくいので先に除く。
				line.heights = solvedHeights;
				if (!trumpetGeometry(line, candidate.station, townCenter, plannedWay(line)).failure.isEmpty()) { continue; }
				if (++solves > kMaximumSolves) { break; }
				const Interchange ic{ candidate.station, candidate.town, interchangeName(towns[candidate.town], U"IC"), interchangeReading(towns[candidate.town], U"IC") };
				Array<Constraint> rules = base;
				addInterchangeConstraints(rules, { ic });
				if (!solveProfile(line, world, crossings, start, end, rules, grade)) { continue; }
				if (const auto geometry = trumpetGeometry(line, ic.station, townCenter, plannedWay(line)); !geometry.failure.isEmpty())
				{
					DBG_LOG(U"[ExpresswayInterchange] fallback rejected station={:.0f} reason={}"_fmt(ic.station, geometry.failure));
					continue;
				}
				DBG_LOG(U"[ExpresswayInterchange] fallback name={} station={:.0f} strict={} solves={}"_fmt(ic.name, ic.station, strict, solves));
				interchanges << ic;
				return true;
			}
		}
		line.heights = solvedHeights;
		DBG_LOG(U"[ExpresswayInterchange] fallback failed candidates={} solves={}"_fmt(candidates.size(), solves));
		return false;
	}

	/// @brief 町の外側へ置いた IC 誘導点。地区半径の外へ出し、路線軸に寄せる。
	Vec2 interchangeSite(const Settlement& town, Vec2 axisPoint)
	{
		Vec2 outward = axisPoint - town.center;
		if (outward.lengthSq() < 1) { outward = Vec2{ 1, 0 }; }
		return town.center + outward.normalized() * (town.radius + settings().expressway_interchangeOffset);
	}

	/// @brief 町の並びから本線の通過点を作る。両端は地図端の陸地、なければ最後の町の先で止める。
	struct Corridor { Array<Vec2> waypoints; bool startBorder = false, endBorder = false; int primaryTown = -1; };

	/// @param attempt 0: 主要都市を順に結ぶ。1: 地図端の着地点を緩めて選び直す。2 以上: 両端の 2 都市だけを通過点にする。
	Optional<Corridor> mainCorridor(const World& world, const Array<Settlement>& towns, uint64 seed, int attempt)
	{
		const auto& c = settings();
		if (towns.isEmpty()) { return none; }
		Array<int> major;
		for (size_t i = 0; i < towns.size(); ++i) { if (towns[i].kind == MapGenerator::SettlementKind::RegionalCity) { major << static_cast<int>(i); } }
		if (major.size() < 2)
		{
			for (size_t i = 0; i < towns.size(); ++i) { if (towns[i].kind == MapGenerator::SettlementKind::LocalTown) { major << static_cast<int>(i); } }
		}
		if (major.size() < 2)
		{
			major.clear();
			for (size_t i = 0; i < towns.size(); ++i) { major << static_cast<int>(i); }
		}
		int first = major[0], second = major[0]; double longest = -1;
		for (const int a : major) for (const int b : major)
		{
			const double distance = towns[a].center.distanceFromSq(towns[b].center);
			if (distance > longest) { longest = distance; first = a; second = b; }
		}
		// 町が 1 つだけなら、地図の中心から町へ向かう軸（中心にあれば種で決めた向き）で地図を横断する。
		Vec2 axis = towns[second].center - towns[first].center;
		if (axis.lengthSq() < 1)
		{
			axis = towns[first].center - Vec2{ WORLD_SIZE * .5, WORLD_SIZE * .5 };
			if (axis.lengthSq() < 1) { axis = (seed >> 9) & 1 ? Vec2{ 1, 0 } : Vec2{ 0, 1 }; }
			axis = Vec2{ -axis.y, axis.x };
		}
		axis = axis.normalized();
		const Vec2 normal{ -axis.y, axis.x };
		struct Stop { double along; Vec2 site; int town; };
		Array<Stop> stops;
		for (const int index : major)
		{
			if (attempt >= 2 && index != first && index != second) { continue; }
			const auto& town = towns[index];
			const Vec2 delta = town.center - towns[first].center;
			const double lateral = delta.dot(normal);
			if (Abs(lateral) > c.expressway_regionalCityAxisDistance) { continue; }
			// 軸上の町は種で決めた側へ外す。
			const Vec2 axisPoint = towns[first].center + axis * delta.dot(axis) + normal * (Abs(lateral) < 1 ? ((seed >> 7) & 1 ? 1.0 : -1.0) : 0.0);
			stops << Stop{ delta.dot(axis), interchangeSite(town, axisPoint), index };
		}
		stops.sort_by([](const Stop& a, const Stop& b) { return a.along < b.along; });
		Corridor corridor;
		int largest = stops.front().town;
		for (const auto& stop : stops) { if (towns[stop.town].radius > towns[largest].radius) { largest = stop.town; } }
		corridor.primaryTown = largest;
		// 本線は必ず地図端から地図端へ抜ける（外部接続）。着地点が見つからなければ条件を緩める。
		const Vec2 head = stops.front().site, tail = stops.back().site;
		const int relax = Min(attempt, 1);
		corridor.waypoints << borderEndpoint(world, head, -axis, relax);
		for (const auto& stop : stops) { corridor.waypoints << stop.site; }
		corridor.waypoints << borderEndpoint(world, tail, axis, relax);
		corridor.startBorder = corridor.endBorder = true;
		const double inset = borderInset();
		for (auto& point : corridor.waypoints) { point = Vec2{ Clamp(point.x, inset, WORLD_SIZE - inset), Clamp(point.y, inset, WORLD_SIZE - inset) }; }
		// 町の誘導点が地図端の着地点より外側（端の近く）にあると線形が折り返すため、近すぎる通過点を外す。
		Array<Vec2> ordered{ corridor.waypoints.front() };
		for (size_t i = 1; i + 1 < corridor.waypoints.size(); ++i)
		{
			if (corridor.waypoints[i].distanceFrom(corridor.waypoints.front()) > 800 && corridor.waypoints[i].distanceFrom(corridor.waypoints.back()) > 800) { ordered << corridor.waypoints[i]; }
		}
		ordered << corridor.waypoints.back();
		corridor.waypoints = std::move(ordered);
		DBG_LOG(U"[ExpresswayCorridorPlan] attempt={} stops={} start=({:.0f},{:.0f}) end=({:.0f},{:.0f})"_fmt(attempt, stops.size(),
			corridor.waypoints.front().x, corridor.waypoints.front().y, corridor.waypoints.back().x, corridor.waypoints.back().y));
		return corridor;
	}

	Optional<Line> routeThrough(const World& world, const AvoidanceGrid& avoid, const Array<Vec2>& waypoints, size_t fixedPrefix)
	{
		Array<Vec2> coarse;
		for (size_t i = 0; i < fixedPrefix && i < waypoints.size(); ++i) { coarse << waypoints[i]; }
		for (size_t i = Max<size_t>(1, fixedPrefix); i < waypoints.size(); ++i)
		{
			auto part = searchCorridor(world, avoid, waypoints[i - 1], waypoints[i]);
			if (part.size() < 2) { DBG_LOG(U"[Expressway] corridor failed from={} to={}"_fmt(waypoints[i - 1], waypoints[i])); return none; }
			part.pop_front();
			coarse.append(part);
		}
		auto line = shapeLine(coarse, fixedPrefix);
		if (line) { DBG_LOG(U"[ExpresswayCorridor] waypoints={} lengthM={:.0f} straightM={:.0f}"_fmt(waypoints.size(), line->length(), waypoints.front().distanceFrom(waypoints.back()))); }
		return line;
	}

	/// @brief 既存の国道番号のうち、線形が平行で近いものを路線番号に使う（CSL MotorwayNamingPlans）。
	int routeNumber(const RoadNetwork& roads, const Line& line, uint64 seed, const HashSet<int>& used)
	{
		int best = 0; double nearest = 1500;
		for (const auto& route : roads.routes())
		{
			if (route.kind != RoadRouteKind::NationalRoute || route.number <= 0 || used.contains(route.number)) { continue; }
			for (const int id : route.edgeIds)
			{
				const auto curve = roads.getBezier(id); if (!curve) { continue; }
				const Vec2 middle = flat(curve->positionAt(curve->totalLength * .5f));
				const auto [station, distance] = line.nearest(middle);
				const Vec3 t = curve->tangentAt(curve->totalLength * .5f);
				if (distance < nearest && Abs(Vec2{ t.x, t.z }.normalized().dot(line.tangent(station))) > .85) { nearest = distance; best = route.number; }
			}
		}
		if (best > 0) { return best; }
		int number = 1 + static_cast<int>(seed % 99);
		for (int i = 0; i < 99 && used.contains(number); ++i) { number = number % 99 + 1; }
		return number;
	}

	/// @brief 既存道路の近くでは土工で地盤を動かさない（一般道の路面と擁壁の整合を保つ）。
	class OccupiedGround
	{
	public:
		void addCurve(const CubicBezier& curve)
		{
			for (float arc = 0; arc <= curve.totalLength; arc += 12) { const Vec3 p = curve.positionAt(arc); m_cells.insert(key(static_cast<int>(p.x / kCell), static_cast<int>(p.z / kCell))); }
		}
		[[nodiscard]] bool near(Vec2 point, double distance) const
		{
			const int x = static_cast<int>(point.x / kCell), z = static_cast<int>(point.y / kCell), range = static_cast<int>(Ceil(distance / kCell));
			for (int dz = -range; dz <= range; ++dz) for (int dx = -range; dx <= range; ++dx) { if (m_cells.contains(key(x + dx, z + dz))) { return true; } }
			return false;
		}
	private:
		static constexpr double kCell = 32;
		static int64 key(int x, int z) { return (static_cast<int64>(x) << 32) | static_cast<uint32>(z); }
		HashSet<int64> m_cells;
	};
}

namespace ExpresswayNetwork
{
	double minimumCurveRadius(double speed)
	{
		// 道路構造令 第15条: 120→710m, 100→460m, 80→280m, 60→150m, 50→100m, 40→60m, 30→30m。
		constexpr std::array<std::pair<double, double>, 7> table{ { { 30, 30 }, { 40, 60 }, { 50, 100 }, { 60, 150 }, { 80, 280 }, { 100, 460 }, { 120, 710 } } };
		if (speed <= table.front().first) { return table.front().second; }
		for (size_t i = 1; i < table.size(); ++i)
		{
			if (speed <= table[i].first)
			{
				const double t = (speed - table[i - 1].first) / (table[i].first - table[i - 1].first);
				return Math::Lerp(table[i - 1].second, table[i].second, t);
			}
		}
		return table.back().second;
	}

	String displayName(StringView name)
	{
		String result{ name };
		result.replace(U"自動車道", U"道");
		result.replace(U"高速", U"");
		return result.trimmed();
	}

	String routeBaseName(StringView place)
	{
		String result = String{ place }.trimmed();
		for (const StringView suffix : { U"自動車道", U"高速道路", U"高速" }) { if (result.ends_with(suffix)) { result = result.substr(0, result.size() - suffix.size()); } }
		if (result.size() > 1 && result.ends_with(U"市")) { result.pop_back(); }
		return result.isEmpty() ? String{ U"中央" } : result;
	}

	Summary generate(uint64 seed, World& world, RoadNetwork& roads, const Array<MapGenerator::Settlement>& settlements)
	{
		const Stopwatch timer{ StartImmediately::Yes };
		const auto& c = settings();
		Summary summary;
		if (!c.expressway_enabled || c.expressway_maximumLines <= 0 || settlements.isEmpty()) { return summary; }

		// 既存道路の立体交差判定と土工の除外範囲は、高速道路を足す前の道路網で作る。
		const TransportClearance crossings{ &roads, world };
		const AvoidanceGrid avoid{ settlements, roads };
		OccupiedGround occupied;
		for (const auto& edge : roads.edges()) { if (edge.id >= 0 && edge.hasRoadLanes()) { if (const auto curve = roads.getBezier(edge.id)) { occupied.addCurve(*curve); } } }

		// 本線は必ず地図端から地図端へ抜け、IC を 1 つ以上持つ。成立しなければ通過点・着地点・勾配を緩めて計画し直す。
		Array<Route> routes;
		Optional<Corridor> corridor;
		constexpr int kPlanAttempts = 4;
		bool planned = false;
		for (int attempt = 0; attempt < kPlanAttempts && !planned; ++attempt)
		{
			const bool lastAttempt = attempt + 1 == kPlanAttempts;
			// 最後の試行だけ最急縦断勾配を 1.5 倍（4% → 6%: 山地部の設計速度相当）まで許す。
			const double mainGrade = Min(.08, c.expressway_maximumGrade * (lastAttempt ? 1.5 : 1.0));
			routes.clear();
			corridor = mainCorridor(world, settlements, seed, attempt);
			if (!corridor) { DBG_LOG(U"[Expressway] skipped reason=no-towns"); return summary; }
			{
				auto line = routeThrough(world, avoid, corridor->waypoints, 1);
				if (!line) { DBG_LOG(U"[Expressway] retry attempt={} reason=no-mainline-corridor"_fmt(attempt)); continue; }
				Route first;
				first.line = std::move(*line);
				// 3 回目は山地を長大トンネル（土被り 4 倍まで）・高橋脚（2 倍まで）で抜ける。
				// 最後の試行は土被りの上限を外し（下限は railway_minimumAltitude）、高架も 3 倍まで許して本線を必ず通す。
				first.line.structureScale = lastAttempt ? 1000.0 : attempt >= 2 ? 4.0 : 1.0;
				first.line.viaductScale = lastAttempt ? 3.0 : attempt >= 2 ? 2.0 : 1.0;
				first.primaryTown = corridor->primaryTown;
				routes << std::move(first);
			}

			// ---- JCT: 本線から離れた町へ支線を出す（Y 型の片方向 JCT） ----
			if (c.expressway_maximumLines >= 2 && !lastAttempt)
			{
				int target = -1; double farthest = c.expressway_junctionTargetDistance;
				for (size_t i = 0; i < settlements.size(); ++i)
				{
					const auto& town = settlements[i];
					if (town.kind == MapGenerator::SettlementKind::RuralSettlement) { continue; }
					const double distance = routes.front().line.nearest(town.center).second - town.radius;
					const double score = distance * (town.kind == MapGenerator::SettlementKind::RegionalCity ? 1.6 : 1.0);
					if (distance > c.expressway_junctionTargetDistance && score > farthest) { farthest = score; target = static_cast<int>(i); }
				}
				const double guard = c.expressway_junctionMergeLength + 900;
				if (target >= 0 && routes.front().line.length() > guard * 2)
				{
					Junction junction;
					junction.target = target;
					const auto& baseLine = routes.front().line;
					junction.station = Clamp(baseLine.nearest(settlements[target].center).first, guard, baseLine.length() - guard);
					{
						// 連絡路の区間に一般道が入らない位置へ JCT をずらす（縦断を地盤付近に保つため）。
						const int side = (settlements[target].center - baseLine.at(junction.station)).dot(rightOf(baseLine.tangent(junction.station))) < 0 ? 1 : -1;
						const auto free = [&](double s)
						{
							const double from = Min(s - side * (c.expressway_junctionMergeLength + 400), s + side * 400.0), to = Max(s - side * (c.expressway_junctionMergeLength + 400), s + side * 400.0);
							for (double station = from; station <= to; station += 30)
							{
								const Vec2 right = rightOf(baseLine.tangent(station));
								for (const double lateral : { -40.0, 0.0, 40.0, c.expressway_junctionBranchOffset * side * -1.0 - 20 * side })
								{
									if (crossings.interval(baseLine.at(Clamp(station, 0.0, baseLine.length())) + right * lateral, 12).second > -1e8) { return false; }
								}
							}
							return true;
						};
						const double preferred = junction.station;
						for (double shift = 0; shift <= 4000; shift += 150)
						{
							bool found = false;
							for (const double sign : { 1.0, -1.0 })
							{
								const double trial = preferred + shift * sign;
								if (trial < guard || trial > baseLine.length() - guard || !free(trial)) { continue; }
								junction.station = trial; found = true; break;
							}
							if (found) { break; }
						}
					}
					const Vec2 center = routes.front().line.at(junction.station), t = routes.front().line.tangent(junction.station);
					// 目標が中心線の左なら station 増加側の車道（左側）から分岐する。
					junction.side = (settlements[target].center - center).dot(rightOf(t)) < 0 ? 1 : -1;
					const Vec2 u = t * junction.side, left = -rightOf(u);
					const double angle = Math::ToRadians(c.expressway_junctionBranchAngle);
					junction.direction = (u * Cos(angle) + left * Sin(angle)).normalized();
					junction.origin = center + left * c.expressway_junctionBranchOffset;
					// JCT 名は最寄りの町名（CSL: 地区名を JCT 名に使う）。
					int nearestTown = target; double best = Math::Inf;
					for (size_t i = 0; i < settlements.size(); ++i)
					{
						if (settlements[i].kind == MapGenerator::SettlementKind::RuralSettlement) { continue; }
						const double d = settlements[i].center.distanceFromSq(center);
						if (d < best) { best = d; nearestTown = static_cast<int>(i); }
					}
					junction.name = interchangeName(settlements[nearestTown], U"JCT");
					junction.reading = interchangeReading(settlements[nearestTown], U"JCT");
					const auto& town = settlements[target];
					const Vec2 site = interchangeSite(town, junction.origin);
					Array<Vec2> waypoints{ junction.origin, junction.origin + junction.direction * c.expressway_junctionBranchLead, site };
					const Vec2 onward = (town.center - center).normalized();
					if (const auto border = borderLanding(world, site, onward)) { waypoints << *border; }
					else { waypoints << site + onward * c.expressway_lineEndLead; }
					if (auto line = routeThrough(world, avoid, waypoints, 2))
					{
						Route branch;
						branch.line = std::move(*line);
						branch.startsAtJunction = true;
						branch.primaryTown = target;
						routes.front().branch = junction;
						routes << std::move(branch);
					}
					else { DBG_LOG(U"[Expressway] junction skipped reason=no-branch-corridor target={}"_fmt(town.name)); }
				}
			}
			Route& mainRoute = routes.front();

			// ---- IC と縦断 ----
			Array<double> mainAvoid;
			if (mainRoute.branch) { mainAvoid << mainRoute.branch->station; }
			mainRoute.interchanges = chooseInterchanges(mainRoute.line, settlements, mainAvoid, corridor->endBorder, crossings);
			{
				Array<Constraint> rules;
				if (mainRoute.branch)
				{
					const double s = mainRoute.branch->station, side = mainRoute.branch->side;
					const double from = Min(s - side * (c.expressway_junctionMergeLength + 120), s + side * 200), to = Max(s - side * (c.expressway_junctionMergeLength + 120), s + side * 200);
					rules << Constraint{ from, to, -3, 6 };
				}
				const double start = groundAt(world, mainRoute.line.points.front()) + 1, end = groundAt(world, mainRoute.line.points.back()) + 1;
				bool solved = solveWithInterchanges(mainRoute.line, world, crossings, start, end, rules, mainRoute.interchanges, mainGrade);
				if (!solved && !rules.isEmpty())
				{
					DBG_LOG(U"[Expressway] junction dropped reason=mainline-profile");
					rules.clear(); mainRoute.branch.reset(); routes.resize(1); mainAvoid.clear();
					solved = solveWithInterchanges(mainRoute.line, world, crossings, start, end, rules, mainRoute.interchanges, mainGrade);
				}
				if (!solved) { DBG_LOG(U"[Expressway] retry attempt={} reason=mainline-profile-infeasible lengthM={:.0f}"_fmt(attempt, mainRoute.line.length())); continue; }
				// 本線には必ず IC を置く。ランプが成立しない IC を外し、残らなければ建設できる位置を探す。
				dropUnbuildableInterchanges(mainRoute.line, settlements, mainRoute.interchanges);
				if (mainRoute.interchanges.isEmpty()
					&& !addFallbackInterchange(mainRoute.line, world, crossings, settlements, mainAvoid, start, end, rules, mainGrade, mainRoute.interchanges)
					&& mainRoute.branch)
				{
					// JCT との離隔で置けないときは JCT をあきらめ、IC を優先する。
					DBG_LOG(U"[Expressway] junction dropped reason=interchange-required");
					rules.clear(); mainRoute.branch.reset(); routes.resize(1); mainAvoid.clear();
					if (solveProfile(mainRoute.line, world, crossings, start, end, {}, mainGrade))
					{
						addFallbackInterchange(mainRoute.line, world, crossings, settlements, mainAvoid, start, end, rules, mainGrade, mainRoute.interchanges);
					}
				}
				if (mainRoute.interchanges.isEmpty() && !lastAttempt) { DBG_LOG(U"[Expressway] retry attempt={} reason=no-buildable-interchange"_fmt(attempt)); continue; }
				mainRoute.interchanges.sort_by([](const Interchange& a, const Interchange& b) { return a.station < b.station; });
			}
			DBG_LOG(U"[ExpresswayPlan] attempt={} lengthKm={:.1f} interchanges={} junction={} grade={:.3f}"_fmt(
				attempt, routes.front().line.length() / 1000.0, routes.front().interchanges.size(), routes.front().branch.has_value(), mainGrade));
			planned = true;
		}
		if (!planned) { DBG_LOG(U"[Expressway] skipped reason=no-feasible-mainline attempts={}"_fmt(kPlanAttempts)); return summary; }
		Route& mainRoute = routes.front();
		if (routes.size() > 1)
		{
			auto& branch = routes[1];
			const auto& junction = *mainRoute.branch;
			branch.interchanges = chooseInterchanges(branch.line, settlements, { 0.0 }, true, crossings);
			const double split = c.expressway_junctionFlyoverClearance * .5 / Max(100.0, c.expressway_junctionSplitLength);
			const double start = mainRoute.line.height(junction.station) + c.expressway_junctionFlyoverClearance * .5;
			const double end = groundAt(world, branch.line.points.back()) + 1;
			if (!solveWithInterchanges(branch.line, world, crossings, start, end, {}, branch.interchanges, Max(.01, c.expressway_maximumGrade - split)))
			{
				DBG_LOG(U"[Expressway] junction skipped reason=branch-profile-infeasible");
				routes.pop_back(); mainRoute.branch.reset();
			}
		}

		// ---- JCT ランプの事前検証（交差する連絡路どうし・本線との離隔） ----
		const double flyover = c.expressway_junctionFlyoverClearance;
		const double splitLength = Max(100.0, c.expressway_junctionSplitLength);
		const HeightOffset branchOut = [=](double s) { return s < splitLength ? -flyover * .5 * (1 - s / splitLength) : 0.0; };
		const HeightOffset branchIn = [=](double s) { return s < splitLength ? flyover * .5 * (1 - s / splitLength) : 0.0; };
		struct JunctionRamps { CubicBezier diverge, merge; double divergeStation = 0, mergeStation = 0; };
		Optional<JunctionRamps> junctionRamps;
		if (routes.size() > 1)
		{
			const auto& junction = *mainRoute.branch;
			const auto& branch = routes[1].line;
			const int near = junction.side, far = -junction.side;
			const double divergeStation = junction.station - near * c.expressway_junctionDivergeLength;
			const double mergeStation = junction.station - near * c.expressway_junctionMergeLength;
			const Vec3 divergePoint = carriagewayPoint(mainRoute.line, divergeStation, near, {});
			const Vec3 mergePoint = carriagewayPoint(mainRoute.line, mergeStation, far, {});
			const Vec3 branchStart = carriagewayPoint(branch, 0, 1, branchOut), branchEnd = carriagewayPoint(branch, 0, -1, branchIn);
			const Vec2 nearTravel = mainRoute.line.tangent(divergeStation) * near, farTravel = mainRoute.line.tangent(mergeStation) * far;
			const double angle = c.expressway_rampDivergeAngle;
			const Vec2 v = branch.tangent(0);
			JunctionRamps ramps;
			ramps.divergeStation = divergeStation; ramps.mergeStation = mergeStation;
			ramps.diverge = rampCurve(divergePoint, nearTravel - rightOf(nearTravel) * angle, branchStart, v,
				Math::Lerp(divergePoint.y, branchStart.y, .2), Math::Lerp(divergePoint.y, branchStart.y, .8));
			ramps.merge = rampCurve(branchEnd, -v, mergePoint, farTravel + rightOf(farTravel) * angle, branchEnd.y, mergePoint.y);
			bool valid = maximumGrade(ramps.diverge) <= c.expressway_rampMaximumGrade * 1.05 && maximumGrade(ramps.merge) <= c.expressway_rampMaximumGrade * 1.05;
			// 本線車道の上を横切る区間だけ離隔を測る。合流直前に並走する区間は横断ではない。
			const double halfWidth = c.expressway_lanesPerDirection * c.expressway_laneWidth * .5 + c.expressway_outerShoulder * .5 + c.expressway_rampLaneWidth * .5;
			int crossingSamples = 0; double lowest = Math::Inf;
			for (float arc = 0; valid && arc <= ramps.merge.totalLength; arc += 4)
			{
				const Vec3 p = ramps.merge.positionAt(arc);
				const double station = mainRoute.line.nearest(flat(p)).first;
				const Vec2 center = mainRoute.line.at(station), along = mainRoute.line.tangent(station);
				const double lateral = Abs((flat(p) - center).dot(rightOf(along)));
				const Vec3 heading = ramps.merge.tangentAt(arc);
				const bool parallel = Abs(Vec2{ heading.x, heading.z }.normalized().dot(along)) > .97;
				if (Abs(lateral - ExpresswayCrossSection::carriagewayOffset()) < halfWidth && !parallel)
				{
					++crossingSamples;
					lowest = Min(lowest, p.y - mainRoute.line.height(station));
					valid &= p.y - mainRoute.line.height(station) >= c.expressway_junctionCrossingClearance;
				}
				for (float other = 0; valid && other <= ramps.diverge.totalLength; other += 4)
				{
					const Vec3 q = ramps.diverge.positionAt(other);
					if (flat(p).distanceFrom(flat(q)) < c.expressway_rampLaneWidth * 2 + 2) { valid &= Abs(p.y - q.y) >= c.expressway_junctionCrossingClearance; }
				}
			}
			DBG_LOG(U"[ExpresswayJunction] name={} valid={} crossingSamples={} lowestClearance={:.2f} divergeGrade={:.4f} mergeGrade={:.4f}"_fmt(
				junction.name, valid, crossingSamples, lowest, maximumGrade(ramps.diverge), maximumGrade(ramps.merge)));
			if (valid) { junctionRamps = ramps; }
			else { routes.pop_back(); mainRoute.branch.reset(); }
		}

		// ---- 上下線の構築 ----
		Array<int> mainlineEdges, rampEdges, accessEdges;
		HashSet<int> usedNumbers;
		for (size_t index = 0; index < routes.size(); ++index)
		{
			auto& route = routes[index];
			Array<double> forced, optional;
			for (const auto& ic : route.interchanges)
			{
				// 町の側で分合流の位置が入れ替わるため、両方の向きの節点を置く。
				const auto layout = TrumpetLayout::make();
				for (const double along : { layout.loopLead, layout.farMerge, layout.nearRamp, -layout.nearRamp, -layout.loopLead, -layout.farMerge }) { forced << ic.station + along; }
				// 変速車線の始終点（すり付けの端）にも節点を置く。
				const double taper = c.expressway_auxiliaryTaperLength, decelerate = c.expressway_decelerationLaneLength, accelerate = c.expressway_accelerationLaneLength;
				for (const double along : { layout.loopLead - decelerate, layout.loopLead - decelerate - taper, layout.farMerge + accelerate, layout.farMerge + accelerate + taper,
					layout.nearRamp + decelerate, layout.nearRamp + decelerate + taper, layout.nearRamp + accelerate, layout.nearRamp + accelerate + taper })
				{
					optional << ic.station + along << ic.station - along;
				}
			}
			if (index == 0 && junctionRamps)
			{
				const int near = mainRoute.branch->side;
				forced << junctionRamps->divergeStation << junctionRamps->mergeStation;
				optional << junctionRamps->divergeStation - near * c.expressway_decelerationLaneLength << junctionRamps->divergeStation - near * (c.expressway_decelerationLaneLength + c.expressway_auxiliaryTaperLength);
				optional << junctionRamps->mergeStation - near * c.expressway_accelerationLaneLength << junctionRamps->mergeStation - near * (c.expressway_accelerationLaneLength + c.expressway_auxiliaryTaperLength);
			}
			{
				Array<double> served;
				for (const auto& ic : route.interchanges) { served << ic.station; }
				if (index == 0 && mainRoute.branch) { served << mainRoute.branch->station; }
				if (route.startsAtJunction) { served << 0.0; }
				route.services = chooseServices(route.line, world, crossings, settlements, served);
				for (const auto& service : route.services)
				{
					const double half = c.expressway_serviceLength * .5, taper = c.expressway_auxiliaryTaperLength;
					forced << service.station - half << service.station + half;
					for (const double along : { half + c.expressway_decelerationLaneLength, half + c.expressway_decelerationLaneLength + taper,
						half + c.expressway_accelerationLaneLength, half + c.expressway_accelerationLaneLength + taper })
					{
						optional << service.station - along << service.station + along;
					}
				}
			}
			if (route.startsAtJunction) { forced << splitLength; }
			const auto stations = nodeStations(route.line, forced, optional);
			const auto forwardOffset = route.startsAtJunction ? branchOut : HeightOffset{};
			const auto backwardOffset = route.startsAtJunction ? branchIn : HeightOffset{};
			auto forward = buildCarriageway(roads, world, route.line, stations, 1, forwardOffset, mainlineEdges);
			auto backward = buildCarriageway(roads, world, route.line, stations, -1, backwardOffset, mainlineEdges);
			if (!forward || !backward) { DBG_LOG(U"[Expressway] carriageway build failed route={}"_fmt(index)); return summary; }
			route.forward = std::move(*forward); route.backward = std::move(*backward);
			const auto& town = settlements[Max(0, route.primaryTown)];
			route.number = routeNumber(roads, route.line, seed + index * 31, usedNumbers);
			usedNumbers.insert(route.number);
			route.name = U"E{} {}自動車道"_fmt(route.number, routeBaseName(town.name));
			Array<int> routeEdges = route.forward.edges;
			routeEdges.append(route.backward.edges);
			roads.addRoute(RoadRouteKind::Expressway, route.name, std::move(routeEdges), route.number);
			summary.lengthKm += route.line.length() / 1000.0;
			++summary.lines;
			DBG_LOG(U"[ExpresswayLine] name={} lengthKm={:.1f} nodes={} interchanges={} branch={}"_fmt(
				route.name, route.line.length() / 1000.0, stations.size(), route.interchanges.size(), route.startsAtJunction));
		}

		// ---- IC（トランペット型: 遠い側の流出はループ、連絡路は本線を跨いで料金所へ） ----
		HashSet<int> mainlineSet(mainlineEdges.begin(), mainlineEdges.end());
		// ---- 変速車線: 分流点の手前に減速車線、合流点の先に加速車線（平行式、すり付け付き） ----
		// 本線エッジごとに A 端・B 端の変速車線の幅を集め、路側設備を作り直すときに断面へ反映する。
		HashTable<int, std::pair<float, float>> auxiliary;
		HashSet<int> auxiliaryNodes;
		const auto addAuxiliary = [&](const Carriageway& way, int node, bool merge)
		{
			const auto found = std::find(way.nodes.begin(), way.nodes.end(), node);
			if (found == way.nodes.end()) { return; }
			const size_t k = static_cast<size_t>(found - way.nodes.begin());
			const double full = merge ? c.expressway_accelerationLaneLength : c.expressway_decelerationLaneLength, taper = c.expressway_auxiliaryTaperLength;
			const auto widthAt = [&](double distance) { return static_cast<float>(c.expressway_laneWidth * Clamp(1 - (distance - full) / taper, 0.0, 1.0)); };
			for (size_t j = 0; j < way.edges.size(); ++j)
			{
				// 合流は下流側（走行距離が増える側）、分流は上流側の区間。距離は分合流点から測る。
				const double a = merge ? way.arcs[j] - way.arcs[k] : way.arcs[k] - way.arcs[j];
				const double b = merge ? way.arcs[j + 1] - way.arcs[k] : way.arcs[k] - way.arcs[j + 1];
				const double near = Min(a, b);
				if (near < -1e-6 || near >= full + taper) { continue; }
				auto& widths = auxiliary[way.edges[j]];
				widths.first = Max(widths.first, widthAt(a)); widths.second = Max(widths.second, widthAt(b));
				auxiliaryNodes.insert(way.nodes[j]); auxiliaryNodes.insert(way.nodes[j + 1]);
			}
		};
		// ランプの本線側の端を変速車線の中心へ寄せる（ランプの車線中心 → 変速車線の中心）。
		const float auxiliaryShift = -static_cast<float>(c.expressway_lanesPerDirection * c.expressway_laneWidth * .5 + c.expressway_laneWidth * .5);
		const auto shiftRamp = [&](const Optional<int>& id, bool atB)
		{
			if (!id) { return; }
			if (auto* edge = roads.getEdge(*id)) { ExpresswayCrossSection::shiftRampEnd(*edge, atB, auxiliaryShift); auxiliaryNodes.insert(edge->nodeA); auxiliaryNodes.insert(edge->nodeB); }
		};
		Array<std::pair<Vec2, Vec2>> mainlineSegments;
		for (const int id : mainlineEdges)
		{
			const auto curve = roads.getBezier(id);
			for (float arc = 0; arc < curve->totalLength; arc += 20) { mainlineSegments << std::pair{ flat(curve->positionAt(arc)), flat(curve->positionAt(Min(curve->totalLength, arc + 20))) }; }
		}
		Array<Array<int>> divergeIndices(routes.size() * 2);
		Array<ExitSigns> pendingSigns;
		Array<std::pair<size_t, int>> pendingMerges;  ///< 合流（加速車線）のある車道と節点の添字。先で方面及び距離を出す
		/// @brief 出来た IC（方面・距離の標識の行き先に使う）。
		struct BuiltExit { double station = 0; int number = 0; SignPlace place; };
		Array<Array<BuiltExit>> builtExits(routes.size());
		/// @brief 料金所の先の分岐（方面の門型）と取付道路の入口標識に使う情報。
		struct TollSplit
		{
			size_t route = 0; int stem = -1, access = -1; bool farLeft = true;
			size_t farIndex = 0, nearIndex = 0; double farMerge = 0, nearMerge = 0; SignPlace entrance;
		};
		Array<TollSplit> tollSplits;
		const auto placeOf = [&](size_t town) { return SignPlace{ ExpresswayNetwork::routeBaseName(settlements[town].name), PlaceNameFormat::capitalizeReading(settlements[town].reading) }; };
		// IC の出口の行き先: IC の町と、出口に近いもう 1 つの町（12km 以内）。
		const auto exitPlaces = [&](size_t town, Vec2 point)
		{
			Array<SignPlace> places{ placeOf(town) };
			Optional<size_t> second; double nearest = 12000;
			for (size_t i = 0; i < settlements.size(); ++i)
			{
				if (i == town || placeOf(i).name == places.front().name) { continue; }
				const double distance = settlements[i].center.distanceFrom(point);
				if (distance < nearest) { nearest = distance; second = i; }
			}
			if (second) { places << placeOf(*second); }
			return places;
		};
		RoadEdge connector;
		connector.roadType = RoadType::Arterial;
		GeneratedStreet::apply(connector, GeneratedStreet::describe(GeneratedStreet::Role::Street16));
		for (size_t index = 0; index < routes.size(); ++index)
		{
			auto& route = routes[index];
			for (const auto& ic : route.interchanges)
			{
				const auto geometry = trumpetGeometry(route.line, ic.station, settlements[ic.town].center, [&](double station, int side) -> Optional<Vec3>
				{
					const int node = (side > 0 ? route.forward : route.backward).nodeAt(station);
					return node < 0 ? Optional<Vec3>{} : Optional<Vec3>{ roads.getNode(node)->position };
				});
				if (!geometry.failure.isEmpty()) { DBG_LOG(U"[ExpresswayInterchange] skipped name={} reason={}"_fmt(ic.name, geometry.failure)); continue; }
				const auto& plan = geometry.plan;
				const auto& pieces = geometry.pieces;
				const double s = ic.station;
				const int townSide = geometry.townSide;
				const Vec2 ey = geometry.ey, farTravel = geometry.farTravel, nearTravel = geometry.nearTravel, plazaPoint = geometry.plazaPoint;
				const Vec3 loopEntry = geometry.loopEntry, loopRight = geometry.loopRight, loopTop = geometry.loopTop, southboundTop = geometry.southboundTop;
				const Vec3 northboundTop = geometry.northboundTop, overLoop = geometry.overLoop, southboundFoot = geometry.southboundFoot, northboundFoot = geometry.northboundFoot;
				const Vec3 plazaStart = geometry.plazaStart;
				const auto stationOf = [&](double x) { return s + townSide * x; };
				const auto& nearWay = townSide > 0 ? route.backward : route.forward;
				const auto& farWay = townSide > 0 ? route.forward : route.backward;
				const int farDiverge = farWay.nodeAt(stationOf(plan.loopLead)), farMerge = farWay.nodeAt(stationOf(plan.farMerge));
				const int nearDiverge = nearWay.nodeAt(stationOf(plan.nearRamp)), nearMerge = nearWay.nodeAt(stationOf(-plan.nearRamp));
				const auto position = [&](int node) { return roads.getNode(node)->position; };
				// 節点を作り、曲線ごとに 1 車線のランプを張る。
				const auto node = [&](Vec3 p) { return roads.addNode(p, NodeType::Joint); };
				const int entry = node(loopEntry), loopA = node(loopRight), loopB = node(loopTop), sbTop = node(southboundTop), nbTop = node(northboundTop), above = node(overLoop);
				const int sbFoot = node(southboundFoot), nbFoot = node(northboundFoot), plaza = node(plazaStart);
				const Array<std::pair<int, int>> links{ { farDiverge, entry }, { entry, loopA }, { loopA, loopB }, { loopB, sbTop }, { sbTop, sbFoot }, { nbFoot, nbTop },
					{ nbTop, above }, { above, farMerge }, { nearDiverge, sbFoot }, { nbFoot, nearMerge } };
				Array<Optional<int>> ramps;
				for (size_t i = 0; i < links.size(); ++i)
				{
					const bool loop = pieces[i].key.starts_with(U"loop");
					ramps << addRamp(roads, world, links[i].first, links[i].second, pieces[i].curve, loop ? c.expressway_trumpetLoopSpeed : c.expressway_rampSpeed, rampEdges);
				}
				// 連絡路の上下線を料金所前で 1 本の往復路にまとめる。
				const auto joinCurve = [&](int from, int to)
				{
					const Vec3 a = position(from), b = position(to);
					return CubicBezier{ a, a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), b };
				};
				// 料金所側は往復路の片側（2 車線）をそのまま分けた 2 車線の連絡路。料金所前の端では往復路の左半分へ寄せ、
				// 分離帯側の路肩・防護柵を絞って、2 本が 1 本の路面から分かれる形にする（重なって裂けて見えないように）。
				const auto outbound = addRamp(roads, world, sbFoot, plaza, joinCurve(sbFoot, plaza), c.expressway_rampSpeed, rampEdges, 2);
				const auto inbound = addRamp(roads, world, plaza, nbFoot, joinCurve(plaza, nbFoot), c.expressway_rampSpeed, rampEdges, 2);
				const float stemHalf = -(ExpresswayCrossSection::kTollSeparator * .5f + c.expressway_rampLaneWidth);
				if (outbound) { if (auto* edge = roads.getEdge(*outbound)) { ExpresswayCrossSection::shiftEnd(*edge, true, stemHalf, 1); } }
				if (inbound) { if (auto* edge = roads.getEdge(*inbound)) { ExpresswayCrossSection::shiftEnd(*edge, false, stemHalf, 1); } }
				// 鼻（1 車線のランプ 2 本と 2 車線の連絡路の分合流）: 各ランプの端を連絡路の左右の車線へ寄せ、内側の路側部材を絞る。
				// travel は鼻での連絡路の進行方向。
				const auto splitAtFoot = [&](int foot, Vec2 travel, size_t first, size_t second) -> bool
				{
					if (!ramps[first] || !ramps[second] || travel.lengthSq() < 1e-6) { return true; }
					const Vec3 footPosition = position(foot);
					const Vec2 right = rightOf(travel.normalized());
					const auto lateralOf = [&](int id)
					{
						const auto curve = roads.getBezier(id);
						const auto* edge = roads.getEdge(id);
						const bool atB = edge->nodeB == foot;
						const Vec3 probe = curve->positionAt(atB ? Max(0.0f, curve->totalLength - 30.0f) : Min(curve->totalLength, 30.0f));
						return (flat(probe) - flat(footPosition)).dot(right);
					};
					const int a = *ramps[first], b = *ramps[second];
					const bool aLeft = lateralOf(a) < lateralOf(b);
					const float shift = c.expressway_rampLaneWidth * .5f;
					for (const auto& [id, left] : { std::pair{ a, aLeft }, std::pair{ b, !aLeft } })
					{
						auto* edge = roads.getEdge(id);
						const bool atB = edge->nodeB == foot;
						// 左のランプは右側（相手側）を、右のランプは左側を絞る。
						ExpresswayCrossSection::shiftEnd(*edge, atB, left ? -shift : shift, left ? 1 : -1);
					}
					return aLeft;
				};
				splitAtFoot(sbFoot, flat(position(plaza)) - flat(position(sbFoot)), 4, 8);
				// 料金所から本線へ向かう側: ramps[5] はループを越えて遠い側の車道へ、ramps[9] は近い側の車道へ入る。
				const bool farLeft = splitAtFoot(nbFoot, flat(position(nbFoot)) - flat(position(plaza)), 5, 9);
				Optional<int> stemId;
				const Vec2 plazaEndPoint = plazaPoint - ey * c.expressway_tollStemLength;
				const int stemEnd = roads.addNode(lift(plazaEndPoint, groundAt(world, plazaEndPoint)), NodeType::Joint);
				if (const auto stem = roads.addEdge(plaza, stemEnd, plazaStart.lerp(position(stemEnd), 1.0 / 3), plazaStart.lerp(position(stemEnd), 2.0 / 3), RoadType::Expressway, 4))
				{
					finishEdge(roads, world, *stem, [](RoadEdge& edge) { ExpresswayCrossSection::applyTollStem(edge); });
					rampEdges << *stem;
					stemId = stem;
					RoadObject gate;
					gate.parentEdgeId = *stem; gate.arcPos = static_cast<float>(c.expressway_tollStemLength * .5); gate.type = RoadObjectType::TollGate;
					gate.scale = ExpresswayCrossSection::tollStemWidth() / c.expressway_tollGateWidth + .3f;
					roads.addObject(gate);
					++summary.tollGates;
				}
				if (ramps[0]) { addDivergeBlinker(roads, *ramps[0], farTravel, flat(position(farDiverge))); }
				if (ramps[8]) { addDivergeBlinker(roads, *ramps[8], nearTravel, flat(position(nearDiverge))); }
				addAuxiliary(farWay, farDiverge, false); addAuxiliary(farWay, farMerge, true);
				addAuxiliary(nearWay, nearDiverge, false); addAuxiliary(nearWay, nearMerge, true);
				shiftRamp(ramps[0], false); shiftRamp(ramps[7], true); shiftRamp(ramps[8], false); shiftRamp(ramps[9], true);
				// 町側へ 24m の取付道路（往復 1+1）を延ばし、最寄りの一般道へつなぐ。料金所側だけ片側 2 車線にすると
				// 平地でも登坂車線のように見えるため、全区間を 1+1 にする。
				const int nearTerminal = stemEnd;
				const Vec2 outward = -ey, nearPoint = plazaEndPoint;
				const Vec2 middlePoint = nearPoint + outward * c.expressway_accessAddedLaneLength, outerPoint = nearPoint + outward * c.expressway_accessLength;
				const int middle = roads.addNode(lift(middlePoint, groundAt(world, middlePoint)), NodeType::Joint);
				const int outer = roads.addNode(lift(outerPoint, groundAt(world, outerPoint)), NodeType::Joint);
				const auto accessId = addBypass(roads, middle, nearTerminal, ExpresswayCrossSection::BypassVariant::OneOne, accessEdges);
				addBypass(roads, outer, middle, ExpresswayCrossSection::BypassVariant::OneOne, accessEdges);
				struct Target { int node; double score; };
				const Vec3 outerPosition = roads.getNode(outer)->position;
				// 取付道路は近い一般道へつなぐ。町から離れた IC は探索半径を 4 倍まで広げる。
				bool connected = false;
				for (const double radius : { c.expressway_accessConnectRadius, c.expressway_accessConnectRadius * 4 })
				{
					if (connected) { break; }
					Array<Target> targets;
					for (const auto& candidate : roads.nodes())
					{
						if (candidate.id < 0 || candidate.attachments.isEmpty()) { continue; }
						bool ordinary = false, arterial = false;
						for (const auto& attachment : candidate.attachments)
						{
							const auto* edge = roads.getEdge(attachment.edgeId);
							if (!edge || mainlineSet.contains(edge->id) || edge->roadType == RoadType::Expressway || edge->useElevation || !edge->hasRoadLanes()) { ordinary = false; break; }
							ordinary = true; arterial |= edge->roadType == RoadType::Arterial;
						}
						if (!ordinary || accessEdges.any([&](int id) { const auto* e = roads.getEdge(id); return e && (e->nodeA == candidate.id || e->nodeB == candidate.id); })) { continue; }
						const double distance = flat(candidate.position).distanceFrom(flat(outerPosition));
						if (distance > radius || distance < 30) { continue; }
						const double toward = (flat(candidate.position) - flat(outerPosition)).normalized().dot(outward);
						targets << Target{ candidate.id, distance * (arterial ? .6 : 1.0) * (toward > -.2 ? 1.0 : 2.5) };
					}
					targets.sort_by([](const Target& a, const Target& b) { return a.score < b.score; });
					for (size_t attempt = 0; attempt < Min<size_t>(4, targets.size()) && !connected; ++attempt)
					{
						const Vec3 goal = roads.getNode(targets[attempt].node)->position;
						const auto found = RoadAlignment::find(world, outerPosition, goal, RoadType::Arterial, 30000);
						if (!found || crossesSegments(found->curves, mainlineSegments)) { continue; }
						auto ids = RoadAutoPlace::buildAlignment(roads, world, found->curves, connector, .5f);
						if (ids.isEmpty()) { continue; }
						for (const int id : ids) { roads.getEdge(id)->edgeState = EdgeState::Open; }
						roads.resolveIntersections(*std::min_element(ids.begin(), ids.end()), &ids);
						for (const int id : ids) { if (auto* edge = roads.getEdge(id)) { edge->edgeState = EdgeState::Open; accessEdges << id; } }
						connected = true;
					}
				}
				roads.addNamedDestination(nearTerminal, ic.name, ic.reading, 1);
				++summary.interchanges;
				const size_t farIndex = index * 2 + (townSide > 0 ? 0 : 1), nearIndex = index * 2 + (townSide > 0 ? 1 : 0);
				const int farSignNode = farWay.indexAt(stationOf(plan.loopLead)), nearSignNode = nearWay.indexAt(stationOf(plan.nearRamp));
				// 出口標識には路線ごとの IC 番号（起点側から 1, 2, …）を添える。
				const int number = static_cast<int>(&ic - route.interchanges.data()) + 1;
				const SignPlace exitPlace{ ic.name, PlaceNameFormat::capitalizeReading(ic.reading) };
				const auto destinations = exitPlaces(static_cast<size_t>(ic.town), plazaPoint);
				pendingSigns << ExitSigns{ farIndex, farSignNode, number, exitPlace, U"出口", U"EXIT", destinations, none };
				pendingSigns << ExitSigns{ nearIndex, nearSignNode, number, exitPlace, U"出口", U"EXIT", destinations, none };
				builtExits[index] << BuiltExit{ s, number, placeOf(static_cast<size_t>(ic.town)) };
				pendingMerges << std::pair{ farIndex, farWay.indexAt(stationOf(plan.farMerge)) } << std::pair{ nearIndex, nearWay.indexAt(stationOf(-plan.nearRamp)) };
				if (stemId) { tollSplits << TollSplit{ index, *stemId, accessId ? *accessId : -1, farLeft, farIndex, nearIndex, stationOf(plan.farMerge), stationOf(-plan.nearRamp), exitPlace }; }
				divergeIndices[farIndex] << farSignNode;
				divergeIndices[nearIndex] << nearSignNode;
				DBG_LOG(U"[ExpresswayInterchange] name={} type=trumpet station={:.0f} townSide={} connected={} plaza=({:.0f},{:.0f}) loop=({:.0f},{:.0f})"_fmt(
					ic.name, s, townSide, connected, plazaPoint.x, plazaPoint.y, flat(loopTop).x, flat(loopTop).y));
			}
		}

		// ---- PA/SA: 両側の車道から左へ分流し、駐車場に沿う通路を通って合流する ----
		for (size_t index = 0; index < routes.size(); ++index)
		{
			auto& route = routes[index];
			for (const auto& service : route.services)
			{
				const double s = service.station, half = c.expressway_serviceLength * .5, angle = c.expressway_rampDivergeAngle;
				for (const int side : { 1, -1 })
				{
					const auto& way = side > 0 ? route.forward : route.backward;
					const double from = s - side * half, to = s + side * half;
					const int diverge = way.nodeAt(from), merge = way.nodeAt(to);
					if (diverge < 0 || merge < 0) { continue; }
					const auto outward = [&](double station) { return -rightOf(route.line.tangent(station) * side); };
					const auto aisle = [&](double station)
					{
						const Vec3 carriageway = carriagewayPoint(route.line, station, side, {});
						return lift(flat(carriageway) + outward(station) * c.expressway_serviceOffset, carriageway.y);
					};
					const Vec2 travelIn = route.line.tangent(from) * side, travelOut = route.line.tangent(to) * side;
					const Vec3 a = roads.getNode(diverge)->position, b = roads.getNode(merge)->position;
					// 出入りの曲線がランプの最小曲線半径を守るまで、駐車場沿いの通路を短くして取付区間を延ばす（曲線区間の SA）。
					Vec3 aisleStart, aisleEnd; CubicBezier lead, back; double bestRadius = -1;
					for (const double fraction : { .22, .3, .38, .46 })
					{
						const double inner = c.expressway_serviceLength * fraction;
						const Vec3 p = aisle(s - side * (half - inner)), q = aisle(s + side * (half - inner));
						const Vec2 direction = (flat(q) - flat(p)).normalized();
						const auto in = rampCurve(a, travelIn - rightOf(travelIn) * angle, p, direction, Math::Lerp(a.y, p.y, 1.0 / 3), Math::Lerp(a.y, p.y, 2.0 / 3));
						const auto out = rampCurve(q, direction, b, travelOut + rightOf(travelOut) * angle, Math::Lerp(q.y, b.y, 1.0 / 3), Math::Lerp(q.y, b.y, 2.0 / 3));
						const double radius = Min(in.minimumHorizontalRadius(), out.minimumHorizontalRadius());
						if (radius <= bestRadius) { continue; }
						bestRadius = radius; aisleStart = p; aisleEnd = q; lead = in; back = out;
						if (radius >= ExpresswayNetwork::minimumCurveRadius(c.expressway_rampSpeed)) { break; }
					}
					const int start = roads.addNode(aisleStart, NodeType::Joint), end = roads.addNode(aisleEnd, NodeType::Joint);
					const CubicBezier straight{ aisleStart, aisleStart.lerp(aisleEnd, 1.0 / 3), aisleStart.lerp(aisleEnd, 2.0 / 3), aisleEnd };
					const auto first = addRamp(roads, world, diverge, start, lead, c.expressway_rampSpeed, rampEdges);
					const auto middle = addRamp(roads, world, start, end, straight, 20, rampEdges);
					const auto last = addRamp(roads, world, end, merge, back, c.expressway_rampSpeed, rampEdges);
					addAuxiliary(way, diverge, false); addAuxiliary(way, merge, true);
					shiftRamp(first, false); shiftRamp(last, true);
					if (middle)
					{
						RoadObject area;
						area.parentEdgeId = *middle; area.arcPos = static_cast<float>(straight.totalLength * .5);
						area.type = RoadObjectType::ServiceArea;
						area.scale = service.name.ends_with(U"SA") ? 1.6f : 1.0f;
						roads.addObject(area);
					}
					if (first) { addDivergeBlinker(roads, *first, travelIn, flat(a)); }
					const size_t carriageway = index * 2 + (side > 0 ? 0 : 1);
					const int signIndex = way.indexAt(from);
					pendingSigns << ExitSigns{ carriageway, signIndex, 0, SignPlace{ service.name, PlaceNameFormat::capitalizeReading(service.reading) },
						U"", U"", {}, none };
					pendingMerges << std::pair{ carriageway, way.indexAt(to) };
					divergeIndices[carriageway] << signIndex;
				}
				++summary.services;
				DBG_LOG(U"[ExpresswayService] name={} station={:.0f} position=({:.0f},{:.0f})"_fmt(service.name, s, route.line.at(s).x, route.line.at(s).y));
			}
		}

		// ---- JCT 連絡路 ----
		if (junctionRamps && routes.size() > 1)
		{
			const auto& junction = *mainRoute.branch;
			auto& nearWay = junction.side > 0 ? mainRoute.forward : mainRoute.backward;
			auto& farWay = junction.side > 0 ? mainRoute.backward : mainRoute.forward;
			const int divergeNode = nearWay.nodeAt(junctionRamps->divergeStation), mergeNode = farWay.nodeAt(junctionRamps->mergeStation);
			const int branchStart = routes[1].forward.nodes.front(), branchEnd = routes[1].backward.nodes.back();
			const auto first = addRamp(roads, world, divergeNode, branchStart, junctionRamps->diverge, c.expressway_junctionRampSpeed, rampEdges);
			const auto second = addRamp(roads, world, branchEnd, mergeNode, junctionRamps->merge, c.expressway_junctionRampSpeed, rampEdges);
			if (first && second)
			{
				++summary.junctions;
				addAuxiliary(nearWay, divergeNode, false); addAuxiliary(farWay, mergeNode, true);
				shiftRamp(first, false); shiftRamp(second, true);
				addDivergeBlinker(roads, *first, mainRoute.line.tangent(junctionRamps->divergeStation) * junction.side, flat(roads.getNode(divergeNode)->position));
				const size_t nearIndex = junction.side > 0 ? 0 : 1;
				const int divergeIndex = nearWay.indexAt(junctionRamps->divergeStation);
				const auto& branchRoute = routes[1];
				const SignRoute branch{ U"E{}"_fmt(branchRoute.number), ExpresswayNetwork::displayName(branchRoute.name.substr(branchRoute.name.indexOf(U' ') + 1)),
					PlaceNameFormat::capitalizeReading(settlements[Max(0, branchRoute.primaryTown)].reading).uppercased() + U" EXP" };
				Array<SignPlace> branchPlaces;
				if (branchRoute.primaryTown >= 0) { branchPlaces << placeOf(static_cast<size_t>(branchRoute.primaryTown)); }
				pendingSigns << ExitSigns{ nearIndex, divergeIndex, 0, SignPlace{ junction.name, PlaceNameFormat::capitalizeReading(junction.reading) }, U"", U"", branchPlaces, branch };
				pendingMerges << std::pair{ junction.side > 0 ? size_t{ 1 } : size_t{ 0 }, farWay.indexAt(junctionRamps->mergeStation) };
				divergeIndices[nearIndex] << divergeIndex;
			}
			DBG_LOG(U"[ExpresswayJunction] built name={} diverge={} merge={} branch={} origin=({:.0f},{:.0f})"_fmt(junction.name, first.has_value(), second.has_value(), routes[1].name, junction.origin.x, junction.origin.y));
		}

		// ---- 標識 ----
		// 路線番号と路線名（「E158 波島自動車道」→ E158・波島道・HASHIMA EXP）。
		const auto signRoute = [&](size_t index)
		{
			const auto& route = routes[index];
			const String name = route.name.includes(U' ') ? route.name.substr(route.name.indexOf(U' ') + 1) : route.name;
			const String reading = route.primaryTown >= 0 ? settlements[static_cast<size_t>(route.primaryTown)].reading : String{};
			return SignRoute{ U"E{}"_fmt(route.number), ExpresswayNetwork::displayName(name), reading.uppercased() + U" EXP" };
		};
		// 路線の端の町（地図端に最も近い町）。先に IC がないときの行き先。
		const auto endPlace = [&](size_t index, bool atEnd)
		{
			const auto& line = routes[index].line;
			const Vec2 point = line.at(atEnd ? line.length() : 0.0);
			size_t best = 0; double nearest = Math::Inf;
			for (size_t i = 0; i < settlements.size(); ++i)
			{
				const double distance = settlements[i].center.distanceFrom(point);
				if (distance < nearest) { nearest = distance; best = i; }
			}
			return placeOf(best);
		};
		// 車道の走行距離 arc より先の行き先: 次の IC の町と、路線の先の端の町（同名は 1 つ）。
		const auto throughPlaces = [&](size_t carriageway, double arc)
		{
			const size_t index = carriageway / 2;
			const auto& way = carriageway % 2 == 0 ? routes[index].forward : routes[index].backward;
			Array<std::pair<double, SignPlace>> ahead;
			for (const auto& built : builtExits[index])
			{
				const double at = arcAtStation(way, built.station);
				if (at > arc + 100) { ahead << std::pair{ at, built.place }; }
			}
			ahead.sort_by([](const auto& a, const auto& b) { return a.first < b.first; });
			Array<SignPlace> places;
			if (!ahead.isEmpty()) { places << ahead.front().second; }
			const SignPlace last = endPlace(index, way.side > 0);
			if (places.isEmpty() || places.front().name != last.name) { places << last; }
			return places;
		};
		for (const auto& pending : pendingSigns)
		{
			const auto& route = routes[pending.carriageway / 2];
			const auto& way = pending.carriageway % 2 == 0 ? route.forward : route.backward;
			if (pending.index < 0) { continue; }
			HashSet<int> others(divergeIndices[pending.carriageway].begin(), divergeIndices[pending.carriageway].end());
			summary.guideSigns += addExitSigns(roads, way, pending, others, signRoute(pending.carriageway / 2), throughPlaces(pending.carriageway, way.arcs[pending.index]));
			// 道路情報板は予告標識（2km）の手前。手前の別の分岐を越える位置には置かない。
			const double board = way.arcs[pending.index] - c.expressway_informationBoardLead;
			const bool crosses = std::any_of(others.begin(), others.end(), [&](int other) { return other != pending.index && way.arcs[other] > board && way.arcs[other] < way.arcs[pending.index]; });
			if (!crosses) { summary.guideSigns += addInformationBoard(roads, way, board, pending.exit.name, U"", seed + pending.carriageway); }
		}
		// 合流の先（加速車線の終わりから 250m）に方面及び距離: この先の IC（番号・町・距離）と路線の先の端の町。
		for (const auto& [carriageway, index] : pendingMerges)
		{
			if (index < 0) { continue; }
			const size_t routeIndex = carriageway / 2;
			const auto& way = carriageway % 2 == 0 ? routes[routeIndex].forward : routes[routeIndex].backward;
			const double arc = way.arcs[static_cast<size_t>(index)] + c.expressway_accelerationLaneLength + c.expressway_auxiliaryTaperLength + 250;
			Array<std::pair<double, BuiltExit>> ahead;
			for (const auto& built : builtExits[routeIndex])
			{
				const double at = arcAtStation(way, built.station);
				if (at > arc + 300) { ahead << std::pair{ at, built }; }
			}
			ahead.sort_by([](const auto& a, const auto& b) { return a.first < b.first; });
			Array<std::tuple<int, SignPlace, double>> rows;
			for (const auto& [at, built] : ahead) { if (rows.size() < 2) { rows << std::tuple{ built.number, built.place, at - arc }; } }
			const SignPlace last = endPlace(routeIndex, way.side > 0);
			if (rows.isEmpty() || std::get<1>(rows.back()).name != last.name) { rows << std::tuple{ 0, last, way.arcs.back() - arc }; }
			summary.guideSigns += placeRoadside(roads, way, arc, distanceList(rows));
		}
		// 料金所の先の分岐（方面の門型）と、取付道路の入口の方向。
		for (const auto& split : tollSplits)
		{
			const auto* stem = roads.getEdge(split.stem);
			if (!stem) { continue; }
			const SignRoute route = signRoute(split.route);
			const auto& farWay = split.farIndex % 2 == 0 ? routes[split.route].forward : routes[split.route].backward;
			const auto& nearWay = split.nearIndex % 2 == 0 ? routes[split.route].forward : routes[split.route].backward;
			const auto farPlaces = throughPlaces(split.farIndex, arcAtStation(farWay, split.farMerge));
			const auto nearPlaces = throughPlaces(split.nearIndex, arcAtStation(nearWay, split.nearMerge));
			// 料金所から本線へ向かう車線は往復路の B→A 側（A→B の右）。運転者の左の板ほど横位置が大きい。
			const SignFace farFace = routeDirection(route, farPlaces, split.farLeft ? -45.0 : 45.0, split.farLeft);
			const SignFace nearFace = routeDirection(route, nearPlaces, split.farLeft ? 45.0 : -45.0, !split.farLeft);
			const SignFace& leftFace = split.farLeft ? farFace : nearFace;
			const SignFace& rightFace = split.farLeft ? nearFace : farFace;
			const float post = ExpresswayCrossSection::tollStemWidth() * .5f + .6f;
			const float leftCenter = post - .4f - static_cast<float>(leftFace.width) * .5f;
			const float rightCenter = leftCenter - static_cast<float>(leftFace.width + rightFace.width) * .5f - .4f;
			const float otherPost = Min(-post, rightCenter - static_cast<float>(rightFace.width) * .5f - .5f);
			const double arc = stem->length * .8;  // 料金所（中央）を過ぎて往復路の分かれ目の手前
			summary.guideSigns += placeFace(roads, split.stem, stem->nodeB, arc, leftFace, leftCenter, true, otherPost, post) >= 0;
			summary.guideSigns += placeFace(roads, split.stem, stem->nodeB, arc, rightFace, rightCenter, true) >= 0;
			if (const auto* access = roads.getEdge(split.access))
			{
				const float lateral = -(c.expressway_accessMedian * .5f + c.expressway_accessLaneWidth + c.expressway_accessShoulder + c.expressway_accessSidewalk * .5f);
				summary.guideSigns += placeFace(roads, split.access, access->nodeA, Min(20.0, access->length * .3), entranceDirection(route, split.entrance), lateral) >= 0;
			}
		}
		// 車間距離確認区間: 各車道の起点から一定距離の、分岐から離れた地上区間に 1 か所。
		for (size_t carriageway = 0; carriageway < routes.size() * 2; ++carriageway)
		{
			const auto& route = routes[carriageway / 2];
			const auto& way = carriageway % 2 == 0 ? route.forward : route.backward;
			if (way.edges.isEmpty()) { continue; }
			for (double start = c.expressway_spacingCheckLead; start + 300 < way.arcs.back(); start += 500)
			{
				const bool nearDiverge = std::any_of(divergeIndices[carriageway].begin(), divergeIndices[carriageway].end(), [&](int index) { return index >= 0 && Abs(way.arcs[index] - start) < 1500; });
				bool ground = true;
				for (double arc = start - 250; ground && arc <= start + 120; arc += 40)
				{
					const auto* edge = roads.getEdge(locate(way, arc).first);
					ground = edge && !edge->tunnel && !edge->useElevation;
				}
				if (nearDiverge || !ground) { continue; }
				summary.guideSigns += addSpacingCheck(roads, way, start);
				break;
			}
		}

		// ---- 土工・橋脚 ----
		HashSet<int> earthwork;
		Array<int> generated = mainlineEdges;
		generated.append(rampEdges);
		OccupiedGround access;
		for (const int id : accessEdges) { if (const auto curve = roads.getBezier(id)) { access.addCurve(*curve); } }
		for (const int id : generated)
		{
			const auto* edge = roads.getEdge(id); const auto curve = roads.getBezier(id);
			if (!edge || !curve || edge->tunnel) { continue; }
			bool clear = true;
			for (float arc = 0; clear && arc <= curve->totalLength; arc += 20)
			{
				const Vec2 point = flat(curve->positionAt(arc));
				clear = !occupied.near(point, c.expressway_earthworkRoadDistance) && !access.near(point, c.expressway_earthworkRoadDistance);
			}
			if (clear) { earthwork.insert(id); }
		}
		const int grounded = earthwork.empty() ? 0 : RoadTerrainFit::apply(roads, world, earthwork);
		int piers = 0, elevated = 0, tunnels = 0;
		for (const int id : generated)
		{
			const auto* edge = roads.getEdge(id); if (!edge) { continue; }
			elevated += edge->useElevation && !edge->tunnel; tunnels += edge->tunnel;
			if (edge->tunnel && tunnels <= 3) { const Vec3 at = roads.getBezier(id)->positionAt(edge->length * .5f); DBG_LOG(U"[ExpresswayTunnel] edge={} position=({:.0f},{:.1f},{:.0f})"_fmt(id, at.x, at.y, at.z)); }
			if (edge->useElevation && !edge->tunnel) { roads.generatePiersForEdge(id, world); ++piers; }
		}
		// 分合流部（ランプの本線側の端）の前後は本線も照明区間にする。
		Array<Vec2> rampEnds;
		for (const int id : rampEdges)
		{
			if (const auto* ramp = roads.getEdge(id))
			{
				for (const int node : { ramp->nodeA, ramp->nodeB }) { if (const auto* n = roads.getNode(node)) { rampEnds << flat(n->position); } }
			}
		}
		// 土工で高架・トンネルの区分が変わった区間の路側設備を作り直す。
		for (const auto& route : routes)
		{
			for (const auto* way : { &route.forward, &route.backward })
			{
				for (const int id : way->edges)
				{
					auto* edge = roads.getEdge(id); if (!edge) { continue; }
					// 市街地（地区半径＋遮音壁距離）の中を通る区間に遮音壁を立てる。
					const Vec2 middle = flat(roads.getBezier(id)->positionAt(edge->length * .5f));
					const bool urban = settlements.any([&](const Settlement& town)
					{
						return town.kind != MapGenerator::SettlementKind::RuralSettlement && middle.distanceFrom(town.center) < town.radius + c.expressway_noiseBarrierDistance;
					});
					// 路肩外側の地盤が路面より高い区間を切土とみなす（区間の過半）。
					const auto curve = roads.getBezier(id);
					int above = 0, samples = 0;
					for (float arc = 0; arc <= edge->length; arc += 20, ++samples)
					{
						const Vec3 point = curve->positionAt(arc);
						const Vec3 outer = point - tangentToRight(curve->tangentAt(arc)) * (ExpresswayCrossSection::carriagewayOffset() + c.expressway_outerShoulder + 2);
						above += groundAt(world, flat(outer)) > point.y + 1.0;
					}
					bool lit = false;
					for (float arc = 0; !lit && arc <= edge->length; arc += 40)
					{
						const Vec2 point = flat(curve->positionAt(arc));
						lit = std::any_of(rampEnds.begin(), rampEnds.end(), [&](Vec2 end) { return end.distanceFrom(point) < c.expressway_mainlineLightDistance; });
					}
					const auto widths = auxiliary.find(id);
					const float auxiliaryA = widths == auxiliary.end() ? 0.0f : widths->second.first, auxiliaryB = widths == auxiliary.end() ? 0.0f : widths->second.second;
					ExpresswayCrossSection::applyCarriageway(*edge, { .emergencyPhone = way->phones.contains(id), .noiseBarrier = urban, .cutting = above * 2 > samples, .lighting = lit,
						.auxiliaryA = auxiliaryA, .auxiliaryB = auxiliaryB });
				}
			}
		}
		// 車線数が変わった節点の車線接続と端部の切り欠きを作り直す。変速車線に掛かる案内標識は路肩側へ寄せる。
		for (const int node : auxiliaryNodes) { roads.rebuildNodeConnectivity(node, node); }
		for (const auto& sign : roads.guideSigns())
		{
			const auto widths = auxiliary.find(sign.parentEdgeId);
			// 門型は変速車線の幅を見込んで置いてあるのでずらさない。
			if (sign.id < 0 || widths == auxiliary.end() || sign.lateralOffset >= 0 || sign.overhead) { continue; }
			if (auto* placed = roads.getGuideSign(sign.id))
			{
				const auto* edge = roads.getEdge(sign.parentEdgeId);
				const float t = edge && edge->length > 0 ? Clamp(sign.arcOffset / edge->length, 0.0f, 1.0f) : 0.0f;
				const float fromA = edge && sign.nodeEndId == edge->nodeA ? t : 1 - t;
				placed->lateralOffset -= Math::Lerp(widths->second.first, widths->second.second, fromA);
			}
		}
		summary.auxiliaryLanes = static_cast<int>(auxiliary.size());

		// ---- 監査: 一般道との立体交差・勾配 ----
		int conflicts = 0; double steepest = 0;
		for (const int id : generated)
		{
			const auto curve = roads.getBezier(id); if (!curve) { continue; }
			for (int i = 0; i <= 16; ++i)
			{
				const Vec3 p = curve->evaluate(i / 16.0f), t = curve->tangent(i / 16.0f);
				steepest = Max(steepest, Abs(t.y) / Max(1e-6, Vec2{ t.x, t.z }.length()));
				const auto crossing = crossings.interval(flat(p));
				if (p.y < crossing.second - 1 && p.y > crossing.first + 1)
				{
					if (conflicts < 5) { DBG_LOG(U"[ExpresswayRoadConflict] edge={} position=({:.0f},{:.1f},{:.0f}) band=({:.1f},{:.1f}) elevated={}"_fmt(id, p.x, p.y, p.z, crossing.first, crossing.second, roads.getEdge(id)->useElevation)); }
					++conflicts;
				}
			}
		}
		summary.mainlineEdges = static_cast<int>(mainlineEdges.size());
		summary.rampEdges = static_cast<int>(rampEdges.size());
		summary.accessEdges = static_cast<int>(accessEdges.size());
		DBG_LOG(U"[Expressway] lines={} lengthKm={:.1f} interchanges={} junctions={} services={} mainlineEdges={} rampEdges={} accessEdges={} tollGates={} signs={} earthworkEdges={} grounded={} elevated={} tunnels={} pierEdges={} maxGrade={:.4f} roadConflicts={} ms={:.0f}"_fmt(
			summary.lines, summary.lengthKm, summary.interchanges, summary.junctions, summary.services, summary.mainlineEdges, summary.rampEdges, summary.accessEdges,
			summary.tollGates, summary.guideSigns, earthwork.size(), grounded, elevated, tunnels, piers, steepest, conflicts, timer.msF()));
		return summary;
	}
}
