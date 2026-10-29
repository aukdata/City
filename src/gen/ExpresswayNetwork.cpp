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
			return (1 + RailCostProfile::unitCost(Math::Lerp(fromHeight, toHeight, t) - ground) * settings().expressway_terrainCostWeight) * avoid.at(point);
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

	/// @brief 地図端の陸地を、方向の延長線から近い順に探す。
	Optional<Vec2> borderLanding(const World& world, Vec2 from, Vec2 direction)
	{
		const double inset = settings().network_borderInset + 20;
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
		for (double offset = 0; offset <= settings().expressway_borderSearchDistance; offset += 250)
		{
			for (const double sign : { 1.0, -1.0 })
			{
				const Vec2 candidate = hit + along * offset * sign;
				if (insideWorld(candidate, inset - 1) && landAt(world, candidate) && landAt(world, candidate - direction * 200))
				{
					const double rise = Abs(groundAt(world, candidate) - origin);
					const double score = offset + rise * 40;
					if (rise < settings().expressway_borderMaximumRise && score < bestScore) { bestScore = score; best = candidate; }
				}
				if (offset == 0) { break; }
			}
		}
		return best;
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
			double minimum = Max(c.railway_minimumAltitude, ground - c.expressway_maximumTunnelDepth), maximum = ground + c.expressway_maximumViaductHeight;
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
		const auto profile = RailCostProfile::solve(samples, start, end, grade);
		if (!profile.feasible) { return false; }
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
	Array<double> nodeStations(const Line& line, Array<double> forced)
	{
		const double spacing = settings().expressway_nodeSpacing, total = line.length();
		forced.remove_if([&](double s) { return s <= 1 || s >= total - 1; });
		Array<double> result{ 0.0, total };
		result.append(forced);
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
		const auto curves = RoadAlignment::fit(points);
		if (curves.size() + 1 != points.size()) { return none; }
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

	Optional<int> addRamp(RoadNetwork& roads, const World& world, int fromNode, int toNode, const CubicBezier& curve, float speed, Array<int>& created)
	{
		const auto id = roads.addEdge(fromNode, toNode, curve.p1, curve.p2, RoadType::Expressway, 1);
		if (!id) { return none; }
		finishEdge(roads, world, *id, [speed](RoadEdge& edge) { ExpresswayCrossSection::applyRamp(edge, speed); });
		created << *id;
		return id;
	}

	/// @brief 案内標識 1 枚。nodeEnd から arc の位置に、走行方向の左側へ立てる。
	struct SignRow { String name; String reading; float km = 0; };

	int addGuideBoard(RoadNetwork& roads, int edgeId, double arc, const Array<SignRow>& rows)
	{
		const auto* edge = roads.getEdge(edgeId); if (!edge) { return -1; }
		const auto& c = settings();
		Array<GuideSign::DistanceRow> board;
		for (const auto& row : rows) { board << GuideSign::DistanceRow{ row.name, row.reading, row.km }; }
		GuideSignPlacement sign;
		sign.parentEdgeId = edgeId;
		sign.kind = GuideSignKind::DirectionDistance;
		sign.nodeEndId = edge->nodeA;
		sign.arcOffset = static_cast<float>(arc);
		sign.lateralOffset = -(c.expressway_lanesPerDirection * c.expressway_laneWidth * .5f + c.expressway_outerShoulder + c.expressway_guardrailWidth + c.expressway_signLateralMargin);
		sign.poleHeight = c.expressway_signPoleHeight;
		sign.autoGenerated = false;
		sign.bgColor = kExpresswayGreen;
		sign.elements = GuideSign::BuildDistanceBoard(board);
		// 高速道路の案内板は一般道より文字が大きい（約 1.8 倍）。要素は正規化座標なので縦横を同率で拡大する。
		const auto size = GuideSign::computeBoardSizeFor(sign);
		sign.widthOverride = static_cast<float>(size.width * c.expressway_signScale);
		sign.heightOverride = static_cast<float>(size.height * c.expressway_signScale);
		if (const auto curve = roads.getBezier(edgeId))
		{
			const Vec3 at = curve->positionAt(static_cast<float>(arc));
			DBG_LOG(U"[ExpresswaySign] text={} km={} position=({:.0f},{:.1f},{:.0f})"_fmt(rows.front().name, rows.front().km, at.x, at.y, at.z));
		}
		return roads.addGuideSign(sign);
	}

	/// @brief 分岐点の手前 2km・1km・500m と分岐点の案内（CSL AdvanceSigns の距離列）。
	int addAdvanceSigns(RoadNetwork& roads, const Carriageway& road, int divergeIndex, const HashSet<int>& otherDiverges,
		const String& name, const String& reading, const String& caption)
	{
		int count = 0;
		const double target = road.arcs[divergeIndex];
		for (const double distance : { 2000.0, 1000.0, 500.0, 0.0 })
		{
			const double arc = target - Max(distance, 120.0);
			if (arc < 20) { continue; }
			// 手前の別の分岐を越える予告は出さない（CSL: junction を跨がない）。
			bool crosses = false;
			for (const int other : otherDiverges) { crosses |= other != divergeIndex && road.arcs[other] > arc && road.arcs[other] < target; }
			if (crosses) { continue; }
			size_t edge = 0;
			while (edge + 1 < road.edges.size() && road.arcs[edge + 1] < arc) { ++edge; }
			const double local = arc - road.arcs[edge];
			Array<SignRow> rows;
			if (distance > 0) { rows << SignRow{ name, reading, static_cast<float>(distance / 1000.0) }; }
			else { rows << SignRow{ name, reading, 0 } << SignRow{ caption, U"", 0 }; }
			count += addGuideBoard(roads, road.edges[edge], local, rows) >= 0;
		}
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
		double loopLead = -170, farMerge = 430, nearRamp = 380;
		Vec2 loopCenter{ 0, 0 };
		/// @brief 本線の station 方向に IC が占める範囲（中心からの最大距離）。
		[[nodiscard]] double reach() const { return Max({ -loopLead, farMerge, nearRamp }); }
		static TrumpetLayout make()
		{
			const auto& c = GenerationSettings::get();
			TrumpetLayout layout;
			layout.radius = c.expressway_trumpetLoopRadius; layout.loopLift = c.expressway_trumpetLoopLift;
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
		const double endMargin = c.expressway_interchangeEndMargin + TrumpetLayout::make().reach();
		for (const auto& candidate : candidates)
		{
			// 地図端で終わらない路線は末端 IC を残すため、終点側の余裕を短くしない。
			const double upper = line.length() - (lineEndsAtBorder ? endMargin : TrumpetLayout::make().reach() + 200);
			// 取付道路と本線の跨道部が既存道路と重ならない位置まで、IC を路線方向へずらす。
			// IC の敷地（町側: 連絡路・料金所・取付道路、反対側: ループ）に一般道が入らない位置を探す。
			const auto free = [&](double s)
			{
				const Vec2 right = rightOf(line.tangent(s));
				const double townSide = (towns[candidate.town].center - line.at(s)).dot(right) >= 0 ? 1.0 : -1.0;
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
			};
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

	/// @brief 町の外側へ置いた IC 誘導点。地区半径の外へ出し、路線軸に寄せる。
	Vec2 interchangeSite(const Settlement& town, Vec2 axisPoint)
	{
		Vec2 outward = axisPoint - town.center;
		if (outward.lengthSq() < 1) { outward = Vec2{ 1, 0 }; }
		return town.center + outward.normalized() * (town.radius + settings().expressway_interchangeOffset);
	}

	/// @brief 町の並びから本線の通過点を作る。両端は地図端の陸地、なければ最後の町の先で止める。
	struct Corridor { Array<Vec2> waypoints; bool startBorder = false, endBorder = false; int primaryTown = -1; };

	Optional<Corridor> mainCorridor(const World& world, const Array<Settlement>& towns, uint64 seed)
	{
		const auto& c = settings();
		Array<int> major;
		for (size_t i = 0; i < towns.size(); ++i) { if (towns[i].kind == MapGenerator::SettlementKind::RegionalCity) { major << static_cast<int>(i); } }
		if (major.size() < 2)
		{
			for (size_t i = 0; i < towns.size(); ++i) { if (towns[i].kind == MapGenerator::SettlementKind::LocalTown) { major << static_cast<int>(i); } }
		}
		if (major.size() < 2) { return none; }
		int first = major[0], second = major[1]; double longest = -1;
		for (const int a : major) for (const int b : major)
		{
			const double distance = towns[a].center.distanceFromSq(towns[b].center);
			if (distance > longest) { longest = distance; first = a; second = b; }
		}
		const Vec2 axis = (towns[second].center - towns[first].center).normalized();
		const Vec2 normal{ -axis.y, axis.x };
		struct Stop { double along; Vec2 site; int town; };
		Array<Stop> stops;
		for (const int index : major)
		{
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
		const Vec2 head = stops.front().site, tail = stops.back().site;
		if (const auto border = borderLanding(world, head, -axis)) { corridor.waypoints << *border; corridor.startBorder = true; }
		else { corridor.waypoints << head - axis * c.expressway_lineEndLead; }
		for (const auto& stop : stops) { corridor.waypoints << stop.site; }
		if (const auto border = borderLanding(world, tail, axis)) { corridor.waypoints << *border; corridor.endBorder = true; }
		else { corridor.waypoints << tail + axis * c.expressway_lineEndLead; }
		for (auto& point : corridor.waypoints) { point = Vec2{ Clamp(point.x, 60.0, WORLD_SIZE - 60.0), Clamp(point.y, 60.0, WORLD_SIZE - 60.0) }; }
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
		if (!c.expressway_enabled || c.expressway_maximumLines <= 0 || settlements.size() < 2) { return summary; }

		// 既存道路の立体交差判定と土工の除外範囲は、高速道路を足す前の道路網で作る。
		const TransportClearance crossings{ &roads, world };
		const AvoidanceGrid avoid{ settlements, roads };
		OccupiedGround occupied;
		for (const auto& edge : roads.edges()) { if (edge.id >= 0 && edge.hasRoadLanes()) { if (const auto curve = roads.getBezier(edge.id)) { occupied.addCurve(*curve); } } }

		Array<Route> routes;
		const auto corridor = mainCorridor(world, settlements, seed);
		if (!corridor) { DBG_LOG(U"[Expressway] skipped reason=fewer-than-two-towns"); return summary; }
		{
			auto line = routeThrough(world, avoid, corridor->waypoints, 1);
			if (!line) { DBG_LOG(U"[Expressway] skipped reason=no-mainline-corridor"); return summary; }
			Route first;
			first.line = std::move(*line);
			first.primaryTown = corridor->primaryTown;
			routes << std::move(first);
		}

		// ---- JCT: 本線から離れた町へ支線を出す（Y 型の片方向 JCT） ----
		if (c.expressway_maximumLines >= 2)
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
			bool solved = solveWithInterchanges(mainRoute.line, world, crossings, start, end, rules, mainRoute.interchanges, c.expressway_maximumGrade);
			if (!solved && !rules.isEmpty())
			{
				DBG_LOG(U"[Expressway] junction dropped reason=mainline-profile");
				rules.clear(); mainRoute.branch.reset(); routes.resize(1);
				solved = solveWithInterchanges(mainRoute.line, world, crossings, start, end, rules, mainRoute.interchanges, c.expressway_maximumGrade);
			}
			if (!solved) { DBG_LOG(U"[Expressway] skipped reason=mainline-profile-infeasible lengthM={:.0f}"_fmt(mainRoute.line.length())); return summary; }
		}
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
			Array<double> forced;
			for (const auto& ic : route.interchanges)
			{
				// 町の側で分合流の位置が入れ替わるため、両方の向きの節点を置く。
				const auto layout = TrumpetLayout::make();
				for (const double along : { layout.loopLead, layout.farMerge, layout.nearRamp, -layout.nearRamp, -layout.loopLead, -layout.farMerge }) { forced << ic.station + along; }
			}
			if (index == 0 && junctionRamps) { forced << junctionRamps->divergeStation << junctionRamps->mergeStation; }
			{
				Array<double> served;
				for (const auto& ic : route.interchanges) { served << ic.station; }
				if (index == 0 && mainRoute.branch) { served << mainRoute.branch->station; }
				if (route.startsAtJunction) { served << 0.0; }
				route.services = chooseServices(route.line, world, crossings, settlements, served);
				for (const auto& service : route.services) { forced << service.station - c.expressway_serviceLength * .5 << service.station + c.expressway_serviceLength * .5; }
			}
			if (route.startsAtJunction) { forced << splitLength; }
			const auto stations = nodeStations(route.line, forced);
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
		Array<std::pair<Vec2, Vec2>> mainlineSegments;
		for (const int id : mainlineEdges)
		{
			const auto curve = roads.getBezier(id);
			for (float arc = 0; arc < curve->totalLength; arc += 20) { mainlineSegments << std::pair{ flat(curve->positionAt(arc)), flat(curve->positionAt(Min(curve->totalLength, arc + 20))) }; }
		}
		Array<Array<int>> divergeIndices(routes.size() * 2);
		struct PendingSign { size_t carriageway; int index; String name, reading, caption; };
		Array<PendingSign> pendingSigns;
		RoadEdge connector;
		connector.roadType = RoadType::Arterial;
		GeneratedStreet::apply(connector, GeneratedStreet::describe(GeneratedStreet::Role::Street16));
		for (size_t index = 0; index < routes.size(); ++index)
		{
			auto& route = routes[index];
			for (const auto& ic : route.interchanges)
			{
				const auto plan = TrumpetLayout::make();
				const double s = ic.station;
				const Vec2 center = route.line.at(s), t = route.line.tangent(s), right = rightOf(t);
				const int townSide = (settlements[ic.town].center - center).dot(right) >= 0 ? 1 : -1;
				// 局所座標: x は遠い側の車道の進行方向、y は町から離れる向き。遠い側の車道は y=+offset にある。
				const Vec2 ex = t * townSide, ey = -right * townSide;
				const double base = route.line.height(s);
				const auto local = [&](double x, double y, double h) { return lift(center + ex * x + ey * y, base + h); };
				const auto heading = [&](double x, double y) { return ex * x + ey * y; };
				const auto stationOf = [&](double x) { return s + townSide * x; };
				const auto& nearWay = townSide > 0 ? route.backward : route.forward;
				const auto& farWay = townSide > 0 ? route.forward : route.backward;
				const int farDiverge = farWay.nodeAt(stationOf(plan.loopLead)), farMerge = farWay.nodeAt(stationOf(plan.farMerge));
				const int nearDiverge = nearWay.nodeAt(stationOf(plan.nearRamp)), nearMerge = nearWay.nodeAt(stationOf(-plan.nearRamp));
				if (farDiverge < 0 || farMerge < 0 || nearDiverge < 0 || nearMerge < 0) { DBG_LOG(U"[ExpresswayInterchange] skipped name={} reason=missing-node"_fmt(ic.name)); continue; }
				const auto position = [&](int node) { return roads.getNode(node)->position; };
				const Vec2 farTravel = route.line.tangent(stationOf(plan.loopLead)) * townSide, farTravelMerge = route.line.tangent(stationOf(plan.farMerge)) * townSide;
				const Vec2 nearTravel = -route.line.tangent(stationOf(plan.nearRamp)) * townSide, nearTravelMerge = -route.line.tangent(stationOf(-plan.nearRamp)) * townSide;
				const double angle = c.expressway_rampDivergeAngle;
				// 各点（局所座標）。
				const Vec3 loopEntry = local(plan.loopCenter.x, plan.loopCenter.y - plan.radius, 0);
				const Vec3 loopRight = local(plan.loopCenter.x + plan.radius, plan.loopCenter.y, plan.loopLift * .33);
				const Vec3 loopTop = local(plan.loopCenter.x, plan.loopCenter.y + plan.radius, plan.loopLift * .66);
				const Vec3 southboundTop = local(plan.southboundX, plan.loopCenter.y, plan.loopLift);
				const Vec3 northboundTop = local(-plan.southboundX, plan.loopCenter.y, plan.loopLift);
				const Vec3 overLoop = local(plan.loopCenter.x + 20, plan.loopCenter.y + plan.radius + 70, plan.loopLift);
				const Vec3 southboundFoot = local(plan.southboundX, -plan.stemSouth, 0);
				const Vec3 northboundFoot = local(-plan.southboundX, -plan.stemSouth, 0);
				const Vec2 plazaPoint = center + ey * -(plan.stemSouth + 30);
				const Vec3 plazaStart = lift(plazaPoint, base);
				const double arcHandle = .5523 * plan.radius;
				const auto arcCurve = [&](Vec3 from, Vec2 fromDirection, Vec3 to, Vec2 toDirection)
				{
					return CubicBezier{ from, lift(flat(from) + fromDirection * arcHandle, Math::Lerp(from.y, to.y, 1.0 / 3)), lift(flat(to) - toDirection * arcHandle, Math::Lerp(from.y, to.y, 2.0 / 3)), to };
				};
				const auto handled = [&](Vec3 from, Vec2 a, Vec3 to, Vec2 b, double factor, Optional<double> firstHeight = none, Optional<double> secondHeight = none)
				{
					const double handle = flat(to - from).length() * factor;
					return CubicBezier{ from, lift(flat(from) + a.normalized() * handle, firstHeight.value_or(Math::Lerp(from.y, to.y, 1.0 / 3))),
						lift(flat(to) - b.normalized() * handle, secondHeight.value_or(Math::Lerp(from.y, to.y, 2.0 / 3))), to };
				};
				struct Piece { CubicBezier curve; String key; };
				Array<Piece> pieces{
					{ handled(position(farDiverge), farTravel - rightOf(farTravel) * angle, loopEntry, heading(1, 0), .38), U"farLead" },
					{ arcCurve(loopEntry, heading(1, 0), loopRight, heading(0, 1)), U"loop1" },
					{ arcCurve(loopRight, heading(0, 1), loopTop, heading(-1, 0)), U"loop2" },
					{ arcCurve(loopTop, heading(-1, 0), southboundTop, heading(0, -1)), U"loop3" },
					{ handled(southboundTop, heading(0, -1), southboundFoot, heading(0, -1), .38, base + plan.bridgeApex + 1.5, base + plan.bridgeApex - 1), U"southbound" },
					{ handled(northboundFoot, heading(0, 1), northboundTop, heading(0, 1), .38, base + plan.bridgeApex - 1, base + plan.bridgeApex + 1.5), U"northbound" },
					{ handled(northboundTop, heading(0, 1), overLoop, heading(1, 0), .55), U"farOn1" },
					{ handled(overLoop, heading(1, 0), position(farMerge), farTravelMerge + rightOf(farTravelMerge) * angle, .38), U"farOn2" },
					{ handled(position(nearDiverge), nearTravel - rightOf(nearTravel) * angle, southboundFoot, heading(0, -1), .5), U"nearOff" },
					{ handled(northboundFoot, heading(0, 1), position(nearMerge), nearTravelMerge + rightOf(nearTravelMerge) * angle, .5), U"nearOn" },
				};
				// 接続する組を除き、平面で重なる区間は 6m 以上の高低差を取る。跨道部は本線の建築限界を守る。
				const HashSet<String> joined{ U"farLead|loop1", U"loop1|loop2", U"loop2|loop3", U"loop3|southbound", U"northbound|farOn1", U"farOn1|farOn2",
					U"southbound|nearOff", U"northbound|nearOn", U"loop3|farOn1", U"southbound|northbound", U"nearOff|nearOn", U"southbound|farOn1", U"loop3|northbound" };
				String failure;
				for (const auto& piece : pieces)
				{
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
						const double station = route.line.nearest(flat(p)).first;
						const double lateral = Abs((flat(p) - route.line.at(station)).dot(rightOf(route.line.tangent(station))));
						if (lateral < ExpresswayCrossSection::carriagewayOffset() + 7 && p.y - route.line.height(station) < c.expressway_junctionCrossingClearance) { failure = U"mainline:" + piece.key; break; }
					}
				}
				if (!failure.isEmpty()) { DBG_LOG(U"[ExpresswayInterchange] skipped name={} reason={}"_fmt(ic.name, failure)); continue; }
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
				addRamp(roads, world, sbFoot, plaza, joinCurve(sbFoot, plaza), c.expressway_rampSpeed, rampEdges);
				addRamp(roads, world, plaza, nbFoot, joinCurve(plaza, nbFoot), c.expressway_rampSpeed, rampEdges);
				const Vec2 plazaEndPoint = plazaPoint - ey * c.expressway_tollStemLength;
				const int stemEnd = roads.addNode(lift(plazaEndPoint, groundAt(world, plazaEndPoint)), NodeType::Joint);
				if (const auto stem = roads.addEdge(plaza, stemEnd, plazaStart.lerp(position(stemEnd), 1.0 / 3), plazaStart.lerp(position(stemEnd), 2.0 / 3), RoadType::Expressway, 4))
				{
					finishEdge(roads, world, *stem, [](RoadEdge& edge) { ExpresswayCrossSection::applyTollStem(edge); });
					rampEdges << *stem;
					RoadObject gate;
					gate.parentEdgeId = *stem; gate.arcPos = static_cast<float>(c.expressway_tollStemLength * .5); gate.type = RoadObjectType::TollGate;
					gate.scale = ExpresswayCrossSection::tollStemWidth() / c.expressway_tollGateWidth + .3f;
					roads.addObject(gate);
					++summary.tollGates;
				}
				if (ramps[0]) { addDivergeBlinker(roads, *ramps[0], farTravel, flat(position(farDiverge))); }
				if (ramps[8]) { addDivergeBlinker(roads, *ramps[8], nearTravel, flat(position(nearDiverge))); }
				// 町側へ 24m の取付道路（料金所側 2+1）を延ばし、最寄りの一般道へつなぐ。
				const int nearTerminal = stemEnd;
				const Vec2 outward = -ey, nearPoint = plazaEndPoint;
				const Vec2 middlePoint = nearPoint + outward * c.expressway_accessAddedLaneLength, outerPoint = nearPoint + outward * c.expressway_accessLength;
				const int middle = roads.addNode(lift(middlePoint, groundAt(world, middlePoint)), NodeType::Joint);
				const int outer = roads.addNode(lift(outerPoint, groundAt(world, outerPoint)), NodeType::Joint);
				addBypass(roads, middle, nearTerminal, ExpresswayCrossSection::BypassVariant::TwoOne, accessEdges);
				addBypass(roads, outer, middle, ExpresswayCrossSection::BypassVariant::OneOne, accessEdges);
				struct Target { int node; double score; };
				Array<Target> targets;
				const Vec3 outerPosition = roads.getNode(outer)->position;
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
					if (distance > c.expressway_accessConnectRadius || distance < 30) { continue; }
					const double toward = (flat(candidate.position) - flat(outerPosition)).normalized().dot(outward);
					targets << Target{ candidate.id, distance * (arterial ? .6 : 1.0) * (toward > -.2 ? 1.0 : 2.5) };
				}
				targets.sort_by([](const Target& a, const Target& b) { return a.score < b.score; });
				bool connected = false;
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
				roads.addNamedDestination(nearTerminal, ic.name, ic.reading, 1);
				++summary.interchanges;
				const size_t farIndex = index * 2 + (townSide > 0 ? 0 : 1), nearIndex = index * 2 + (townSide > 0 ? 1 : 0);
				const int farSignNode = farWay.indexAt(stationOf(plan.loopLead)), nearSignNode = nearWay.indexAt(stationOf(plan.nearRamp));
				// 出口標識には路線ごとの IC 番号（起点側から 1, 2, …）を添える。
				const String exit = U"出口 {}"_fmt(&ic - route.interchanges.data() + 1);
				pendingSigns << PendingSign{ farIndex, farSignNode, ic.name, ic.reading, exit };
				pendingSigns << PendingSign{ nearIndex, nearSignNode, ic.name, ic.reading, exit };
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
					const double inner = c.expressway_serviceLength * .22;
					const Vec3 aisleStart = aisle(s - side * (half - inner)), aisleEnd = aisle(s + side * (half - inner));
					const Vec2 travelIn = route.line.tangent(from) * side, travelOut = route.line.tangent(to) * side;
					const Vec2 aisleDirection = (flat(aisleEnd) - flat(aisleStart)).normalized();
					const int start = roads.addNode(aisleStart, NodeType::Joint), end = roads.addNode(aisleEnd, NodeType::Joint);
					const Vec3 a = roads.getNode(diverge)->position, b = roads.getNode(merge)->position;
					const auto lead = rampCurve(a, travelIn - rightOf(travelIn) * angle, aisleStart, aisleDirection, Math::Lerp(a.y, aisleStart.y, 1.0 / 3), Math::Lerp(a.y, aisleStart.y, 2.0 / 3));
					const auto back = rampCurve(aisleEnd, aisleDirection, b, travelOut + rightOf(travelOut) * angle, Math::Lerp(aisleEnd.y, b.y, 1.0 / 3), Math::Lerp(aisleEnd.y, b.y, 2.0 / 3));
					const CubicBezier straight{ aisleStart, aisleStart.lerp(aisleEnd, 1.0 / 3), aisleStart.lerp(aisleEnd, 2.0 / 3), aisleEnd };
					const auto first = addRamp(roads, world, diverge, start, lead, c.expressway_rampSpeed, rampEdges);
					const auto middle = addRamp(roads, world, start, end, straight, 20, rampEdges);
					addRamp(roads, world, end, merge, back, c.expressway_rampSpeed, rampEdges);
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
					pendingSigns << PendingSign{ carriageway, signIndex, service.name, service.reading, service.name.ends_with(U"SA") ? U"サービスエリア" : U"パーキングエリア" };
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
				addDivergeBlinker(roads, *first, mainRoute.line.tangent(junctionRamps->divergeStation) * junction.side, flat(roads.getNode(divergeNode)->position));
				const size_t nearIndex = junction.side > 0 ? 0 : 1;
				const int divergeIndex = nearWay.indexAt(junctionRamps->divergeStation);
				pendingSigns << PendingSign{ nearIndex, divergeIndex, junction.name, junction.reading, ExpresswayNetwork::displayName(routes[1].name) };
				divergeIndices[nearIndex] << divergeIndex;
			}
			DBG_LOG(U"[ExpresswayJunction] built name={} diverge={} merge={} branch={} origin=({:.0f},{:.0f})"_fmt(junction.name, first.has_value(), second.has_value(), routes[1].name, junction.origin.x, junction.origin.y));
		}

		// ---- 標識 ----
		for (const auto& pending : pendingSigns)
		{
			const auto& route = routes[pending.carriageway / 2];
			const auto& way = pending.carriageway % 2 == 0 ? route.forward : route.backward;
			if (pending.index < 0) { continue; }
			HashSet<int> others(divergeIndices[pending.carriageway].begin(), divergeIndices[pending.carriageway].end());
			summary.guideSigns += addAdvanceSigns(roads, way, pending.index, others, pending.name, pending.reading, pending.caption);
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
					ExpresswayCrossSection::applyCarriageway(*edge, { .emergencyPhone = way->phones.contains(id), .noiseBarrier = urban, .cutting = above * 2 > samples });
				}
			}
		}

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
