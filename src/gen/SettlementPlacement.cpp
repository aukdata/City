#include "GenerationSettings.hpp"
#include "SettlementPlacement.hpp"
#include "../debug/DebugLog.hpp"
#include <queue>
#include <limits>

namespace
{
	using Settlement = MapGenerator::Settlement;
	using Kind = MapGenerator::SettlementKind;
	
	
	
	
	
	
	
	
	
	
	

	uint64 locationSalt(uint64 seed, Vec2 p)
	{
		const uint64 x = static_cast<uint64>(Max(0.0, p.x));
		const uint64 z = static_cast<uint64>(Max(0.0, p.y));
		return UrbanMorphology::mix(seed ^ (x << 32) ^ z);
	}

	/// @brief 住居の周りに連続した緩斜面の耕地があるかを複数方向で測る。
	double agriculturalLand(Vec2 center, const SettlementPlacement::HeightSampler& height,
		const SettlementPlacement::HeightSampler& water)
	{
		
		const double ground = height(center);
		int suitable = 0;
		for (int i = 0; i < GenerationSettings::get().placement_directions; ++i)
		{
			const double angle = Math::TwoPi * i / GenerationSettings::get().placement_directions;
			const Vec2 direction{Cos(angle), Sin(angle)};
			const double inner = height(center + direction * (GenerationSettings::get().placement_sampleRadius * 0.5));
			const double outer = height(center + direction * GenerationSettings::get().placement_sampleRadius);
			if (inner > water(center + direction * (GenerationSettings::get().placement_sampleRadius * 0.5)) + GenerationSettings::get().placement_dryHeight
				&& outer > water(center + direction * GenerationSettings::get().placement_sampleRadius) + GenerationSettings::get().placement_dryHeight
				&& Abs(inner - ground) < GenerationSettings::get().placement_sampleRadius * GenerationSettings::get().placement_farmlandGrade
				&& Abs(outer - inner) < GenerationSettings::get().placement_sampleRadius * GenerationSettings::get().placement_farmlandGrade)
			{
				++suitable;
			}
		}
		return static_cast<double>(suitable) / GenerationSettings::get().placement_directions;
	}

	struct SiteCandidate
	{
		Vec2 center;
		float score = 0;
		double farmland = 0;
		uint64 salt = 0;
	};

	/// @brief 集落の生活圏を求める粗い往来費用場。道路の確定線形・行政境界ではない。
	class TravelField
	{
		struct Cell
		{
			double height = 0;
			bool wet = false;
			double cost = std::numeric_limits<double>::infinity();
			Optional<size_t> owner;
			Optional<size_t> next;
		};
		RectF m_bounds;
		int m_width;
		int m_height;
		Array<Cell> m_cells;

		size_t index(Vec2 p) const
		{
			const int x = Clamp(static_cast<int>((p.x - m_bounds.x) / GenerationSettings::get().placement_travelCell), 0, m_width - 1);
			const int z = Clamp(static_cast<int>((p.y - m_bounds.y) / GenerationSettings::get().placement_travelCell), 0, m_height - 1);
			return static_cast<size_t>(z * m_width + x);
		}
		Vec2 position(size_t cell) const
		{
			return {m_bounds.x + (cell % m_width + 0.5) * GenerationSettings::get().placement_travelCell,
				m_bounds.y + (cell / m_width + 0.5) * GenerationSettings::get().placement_travelCell};
		}

	public:
		TravelField(const RectF& bounds, const SettlementPlacement::HeightSampler& height,
			const Array<Settlement>& centers, const SettlementPlacement::HeightSampler& water)
			: m_bounds(bounds)
			, m_width(Max(1, static_cast<int>(Ceil(bounds.w / GenerationSettings::get().placement_travelCell))))
			, m_height(Max(1, static_cast<int>(Ceil(bounds.h / GenerationSettings::get().placement_travelCell))))
			, m_cells(static_cast<size_t>(m_width * m_height))
		{
			for (size_t i = 0; i < m_cells.size(); ++i)
			{
				m_cells[i].height = height(position(i));
				m_cells[i].wet = m_cells[i].height < water(position(i)) + GenerationSettings::get().placement_dryHeight;
			}
			using Entry = std::pair<double, size_t>;
			std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
			for (size_t i = 0; i < centers.size(); ++i)
			{
				const size_t cell = index(centers[i].center);
				m_cells[cell].cost = 0;
				m_cells[cell].owner = i;
				queue.emplace(0, cell);
			}
			while (!queue.empty())
			{
				const auto [cost, current] = queue.top();
				queue.pop();
				if (cost != m_cells[current].cost || cost > GenerationSettings::get().placement_catchmentReach) { continue; }
				const int x = static_cast<int>(current % m_width);
				const int z = static_cast<int>(current / m_width);
				for (int dz = -1; dz <= 1; ++dz)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						if ((dx == 0 && dz == 0) || x + dx < 0 || x + dx >= m_width
						|| z + dz < 0 || z + dz >= m_height) { continue; }
						const size_t next = static_cast<size_t>((z + dz) * m_width + x + dx);
						const Vec2 from = position(current), to = position(next);
						const double length = from.distanceFrom(to);
						double previous = m_cells[current].height;
						bool previousWet = m_cells[current].wet;
						double traversal = 0;
						// Intermediate samples prevent a narrow river or ridge disappearing between grid centers.
						
						for (int step = 1; step <= GenerationSettings::get().placement_steps; ++step)
						{
							const double sample = step == GenerationSettings::get().placement_steps ? m_cells[next].height
								: height(from + (to - from) * (static_cast<double>(step) / GenerationSettings::get().placement_steps));
							const double slope = Abs(sample - previous) / (length / GenerationSettings::get().placement_steps);
							const double relativeSlope = slope / GenerationSettings::get().placement_comfortableSlope;
							const Vec2 samplePoint = from + (to - from) * (static_cast<double>(step) / GenerationSettings::get().placement_steps);
							const bool wet = step == GenerationSettings::get().placement_steps ? m_cells[next].wet : sample < water(samplePoint) + GenerationSettings::get().placement_dryHeight;
							const double waterCost = wet || previousWet ? GenerationSettings::get().placement_waterTravelPenalty : 0.0;
							traversal += length / GenerationSettings::get().placement_steps * (1 + Min(GenerationSettings::get().placement_maximumSlopePenalty, GenerationSettings::get().placement_slopePenaltyWeight * relativeSlope * relativeSlope) + waterCost);
							previous = sample;
							previousWet = wet;
						}
						const double proposed = cost + traversal;
						if (proposed < m_cells[next].cost && proposed <= GenerationSettings::get().placement_catchmentReach)
						{
							m_cells[next].cost = proposed;
							m_cells[next].owner = m_cells[current].owner;
							m_cells[next].next = current;
							queue.emplace(proposed, next);
						}
					}
				}
			}
		}

		double cost(Vec2 p) const { return m_cells[index(p)].cost; }
		void assign(Settlement& settlement, const Array<Settlement>& centers) const
		{
			const size_t cell = index(settlement.center);
			const auto& access = m_cells[cell];
			settlement.serviceCenter = access.owner;
			settlement.accessCost = access.owner ? access.cost : GenerationSettings::get().placement_catchmentReach;
			if (!access.owner) { return; }
			size_t target = cell;
			
			for (int step = 0; step < GenerationSettings::get().placement_directionSteps && m_cells[target].next; ++step)
			{
				target = *m_cells[target].next;
			}
			const Vec2 direction = (target == cell ? centers[*access.owner].center : position(target)) - settlement.center;
			if (direction.lengthSq() > 1) { settlement.accessDirection = direction.normalized(); }
		}
	};

	void makePlan(Settlement& settlement, uint64 seed, bool nearRegional,
		const SettlementPlacement::HeightSampler& height)
	{
		const auto site = UrbanMorphology::inspectSite(settlement.center, height);
		const uint64 salt = locationSalt(seed, settlement.center);
		const auto origin = UrbanMorphology::chooseOrigin(static_cast<uint8>(settlement.kind), site, salt, nearRegional);
		const bool railway = settlement.kind == Kind::RegionalCity || origin == UrbanMorphology::Origin::Planned;
		settlement.plan = UrbanMorphology::makePlan(origin, static_cast<uint8>(settlement.kind), site, salt, railway);
		if (settlement.kind==Kind::RegionalCity)
		{
			UrbanStructure::apply(settlement.plan,UrbanStructure::choose(site,salt));
		}
		if (origin == UrbanMorphology::Origin::Port || settlement.plan.structure==UrbanStructure::Type::CoastalHubs)
		{
			settlement.gridAxisZ = site.shoreDirection;
			settlement.gridAxisX = {site.shoreDirection.y, -site.shoreDirection.x};
		}
		else if (settlement.plan.ruralForm == UrbanMorphology::RuralForm::Valley || settlement.plan.structure==UrbanStructure::Type::ConstrainedLinear)
		{
			settlement.gridAxisX = site.contourAxis;
			settlement.gridAxisZ = {-site.contourAxis.y, site.contourAxis.x};
		}
	}
}

bool SettlementPlacement::retainRuralSite(uint64 seed,Vec2 center)
{
	const double draw=(UrbanMorphology::mix(locationSalt(seed,center)^0x8CB92BA72F3D8DD7ULL)%1000000)/1000000.0;
	return draw<GenerationSettings::get().placement_ruralRetentionRate;
}

Array<MapGenerator::Settlement> SettlementPlacement::generate(uint64 seed, Array<Candidate> candidates,
	const RectF& bounds, const HeightSampler& height, const HeightSampler& water)
{
	// Rank sites by agricultural opportunity, retaining a deterministic irregular choice on identical plains.
	candidates.sort_by([&](const Candidate& a, const Candidate& b)
	{
		if (a.score != b.score) { return a.score > b.score; }
		return locationSalt(seed, a.center) < locationSalt(seed, b.center);
	});
	Array<SiteCandidate> sites;
	HashTable<Point, Array<size_t>> buckets;
	for (const auto& candidate : candidates)
	{
		if (candidate.score <= 0 || !bounds.stretched(-GenerationSettings::get().placement_settlementEdgeMargin).contains(candidate.center) || height(candidate.center) < water(candidate.center) + GenerationSettings::get().placement_homeFreeboard) { continue; }
		const Point bucket{static_cast<int>(candidate.center.x / GenerationSettings::get().placement_candidateSpacing), static_cast<int>(candidate.center.y / GenerationSettings::get().placement_candidateSpacing)};
		bool occupied = false;
		for (int z = -1; z <= 1 && !occupied; ++z)
		{
			for (int x = -1; x <= 1 && !occupied; ++x)
			{
				const auto found = buckets.find(bucket + Point{x, z});
				if (found == buckets.end()) { continue; }
				for (const size_t index : found->second)
				{
					if (candidate.center.distanceFromSq(sites[index].center) < GenerationSettings::get().placement_candidateSpacing * GenerationSettings::get().placement_candidateSpacing)
					{
						occupied = true;
						break;
					}
				}
			}
		}
		if (occupied) { continue; }
		buckets[bucket] << sites.size();
		sites << SiteCandidate{candidate.center, candidate.score, agriculturalLand(candidate.center, height, water), locationSalt(seed, candidate.center)};
	}
	sites.sort_by([](const SiteCandidate& a, const SiteCandidate& b)
	{
		const double scoreA = a.score * (GenerationSettings::get().placement_siteBaseWeight + GenerationSettings::get().placement_farmlandWeight * a.farmland);
		const double scoreB = b.score * (GenerationSettings::get().placement_siteBaseWeight + GenerationSettings::get().placement_farmlandWeight * b.farmland);
		if (scoreA != scoreB) { return scoreA > scoreB; }
		return a.salt < b.salt;
	});
	Array<Settlement> settlements;
	const auto clearOfCenters = [&](Vec2 position, double distance)
	{
		for (const auto& other : settlements)
		{
			if (position.distanceFromSq(other.center) < distance * distance) { return false; }
		}
		return true;
	};
	const auto addCenter = [&](const SiteCandidate& site, Kind kind)
	{
		Settlement settlement;
		settlement.center = site.center;
		settlement.score = site.score;
		settlement.kind = kind;
		settlement.radius = kind == Kind::RegionalCity ? GenerationSettings::get().placement_cityRadius : GenerationSettings::get().placement_townRadius;
		bool nearRegional = false;
		for (const auto& other : settlements)
		{
			if (other.kind == Kind::RegionalCity && site.center.distanceFrom(other.center) < GenerationSettings::get().placement_nearRegionalDistance) { nearRegional = true; }
		}
		makePlan(settlement, seed, nearRegional, height);
		settlements << std::move(settlement);
	};
	int cityCount = 0;
	for (const auto& site : sites)
	{
		if (cityCount >= GenerationSettings::get().placement_maximumCities)
		{
			break;
		}
		if (site.score >= GenerationSettings::get().placement_minimumCityScore && site.farmland >= GenerationSettings::get().placement_minimumCityFarmland && clearOfCenters(site.center, GenerationSettings::get().placement_regionalSpacing))
		{
			addCenter(site, Kind::RegionalCity);
			++cityCount;
		}
	}
	// A market town needs surrounding agricultural sites and separation from other centers.
	// Proximity to a castle town alone never promotes a village to urban land.
	for (const auto& site : sites)
	{
		if (site.score < GenerationSettings::get().placement_minimumTownScore || site.farmland < GenerationSettings::get().placement_minimumTownFarmland || site.salt % GenerationSettings::get().placement_townSelectionDivisor != 0
			|| !clearOfCenters(site.center, GenerationSettings::get().placement_townSpacing)) { continue; }
		int neighbours = 0;
		
		for (const auto& other : sites)
		{
			if (other.farmland >= GenerationSettings::get().placement_minimumNeighbourFarmland && site.center.distanceFromSq(other.center) < GenerationSettings::get().placement_marketRadius * GenerationSettings::get().placement_marketRadius) { ++neighbours; }
		}
		if (neighbours >= GenerationSettings::get().placement_minimumTownNeighbours) { addCenter(site, Kind::LocalTown); }
	}
	if (sites.isEmpty()) { return {}; }
	const size_t centerCount = settlements.size();
	const TravelField access(bounds, height, settlements, water);
	// Accessible farm country has closer villages; remote areas keep wider, uneven gaps.
	sites.sort_by([&](const SiteCandidate& a, const SiteCandidate& b)
	{
		const auto priority = [&](const SiteCandidate& site)
		{
			const double accessBonus = 1 - Min(1.0, access.cost(site.center) / GenerationSettings::get().placement_catchmentReach);
			const double variation = static_cast<double>(site.salt % 1000) / 1000;
			return site.score * (GenerationSettings::get().placement_villageSiteWeight + site.farmland * GenerationSettings::get().placement_villageFarmlandWeight) + accessBonus * GenerationSettings::get().placement_villageAccessWeight + variation * GenerationSettings::get().placement_villageVariationWeight;
		};
		const double pa = priority(a), pb = priority(b);
		return pa != pb ? pa > pb : a.salt < b.salt;
	});
	Array<double> villageSpacing;
	for (const auto& site : sites)
	{
		// 山村は狭い谷底の耕地でも成立する。住宅の局所勾配と水面の検査は共通。
		const double minimumFarmland = height(site.center) >= GenerationSettings::get().placement_uplandHeight
										   ? GenerationSettings::get().placement_mountainVillageFarmland
										   : GenerationSettings::get().placement_minimumVillageFarmland;
		if (site.farmland < minimumFarmland)
		{
			continue;
		}
		const double cost = Min(GenerationSettings::get().placement_catchmentReach, access.cost(site.center));
		const double spacing = GenerationSettings::get().placement_villageMinimumSpacing + std::pow(cost / GenerationSettings::get().placement_catchmentReach, GenerationSettings::get().placement_villageRemotenessExponent) * GenerationSettings::get().placement_villageRemoteSpacing;
		bool occupied = false;
		for (size_t i = 0; i < settlements.size(); ++i)
		{
			const auto& other = settlements[i];
			const double separation = other.kind == Kind::RegionalCity ? GenerationSettings::get().placement_cityFarmSeparation
				: (other.kind == Kind::LocalTown ? GenerationSettings::get().placement_townFarmSeparation : Max(spacing, villageSpacing[i - centerCount]));
			if (site.center.distanceFromSq(other.center) < separation * separation) { occupied = true; break; }
		}
		if (occupied) { continue; }
		Settlement village;
		village.center = site.center;
		village.score = site.score;
		village.radius = GenerationSettings::get().placement_villageRadius;
		access.assign(village, settlements);
		makePlan(village, seed, false, height);
		settlements << std::move(village);
		villageSpacing << spacing;
	}
	// 採用済み集落を間引き、空いた場所が代替集落で埋め戻されるのを防ぐ。
	settlements.remove_if([&](const Settlement& settlement)
	{
		return settlement.kind != Kind::RegionalCity && !retainRuralSite(seed,settlement.center);
	});
	// 既成市街地の外に、地形と既存集落の余白から計画住宅地を選ぶ。
	// 距離・造成量のコストで比較し、適地がなければ無理に造成しない。
	Array<Settlement> newTowns;
	const auto& fabric=GenerationSettings::get();
	for (const auto& city:settlements)
	{
		if (city.kind!=Kind::RegionalCity) { continue; }
		const double draw=(UrbanMorphology::mix(locationSalt(seed,city.center)^0x60EBD719ULL)%1000000)/1000000.0;
		if (draw>=fabric.urbanFabric_newTownCityProbability) { continue; }
		Optional<Settlement> best;
		double bestCost=Math::Inf;
		for (const auto& site:sites)
		{
			const double distance=site.center.distanceFrom(city.center);
			if (distance<fabric.urbanFabric_newTownMinimumDistance || distance>fabric.urbanFabric_newTownMaximumDistance) { continue; }
			if (settlements.any([&](const Settlement& other) { return site.center.distanceFrom(other.center)<fabric.urbanFabric_newTownSeparation; })
				|| newTowns.any([&](const Settlement& other) { return site.center.distanceFrom(other.center)<fabric.urbanFabric_newTownSeparation; })) { continue; }
			Settlement town; town.center=site.center; town.score=site.score; town.kind=Kind::LocalTown;
			town.radius=GenerationSettings::get().placement_townRadius;
			const auto terrain=UrbanMorphology::inspectSite(site.center,height);
			town.plan=UrbanMorphology::makePlan(UrbanMorphology::Origin::Planned,1,terrain,site.salt,true);
			town.gridAxisX=terrain.contourAxis; town.gridAxisZ={-terrain.contourAxis.y,terrain.contourAxis.x};
			bool fits=true;
			double low=Math::Inf,high=-Math::Inf;
			const auto extent=town.plan.halfExtent;
			const int columns=Max(2,static_cast<int>(Ceil(2*extent.x/fabric.urbanFabric_newTownSampleSpacing)));
			const int rows=Max(2,static_cast<int>(Ceil(2*extent.y/fabric.urbanFabric_newTownSampleSpacing)));
			Grid<double> heights(columns+1,rows+1);
			for (int row=0;row<=rows && fits;++row)
			{
				for (int col=0;col<=columns && fits;++col)
				{
					const Vec2 p=site.center+town.gridAxisX*(-extent.x+2*extent.x*col/columns)+town.gridAxisZ*(-extent.y+2*extent.y*row/rows);
					const double h=height(p); heights[{col,row}]=h; low=Min(low,h); high=Max(high,h);
					fits=bounds.stretched(-GenerationSettings::get().placement_settlementEdgeMargin).contains(p) && h>=water(p)+fabric.urbanFabric_newTownFreeboard;
					if (col>0) { fits &= Abs(h-heights[{col-1,row}])<=2*extent.x/columns*fabric.urbanFabric_newTownMaximumGrade; }
					if (row>0) { fits &= Abs(h-heights[{col,row-1}])<=2*extent.y/rows*fabric.urbanFabric_newTownMaximumGrade; }
				}
			}
			if (!fits) { continue; }
			const double cost=distance*fabric.urbanFabric_newTownDistanceWeight+(high-low)*fabric.urbanFabric_newTownReliefWeight;
			if (cost<bestCost) { bestCost=cost; best=std::move(town); }
		}
		if (best) { newTowns << std::move(*best); }
	}
	// 中心地は農村より前にまとめ、既存の生活圏インデックス規則を維持する。
	Array<Settlement> ordered;
	for (const auto& town:settlements) { if (town.kind!=Kind::RuralSettlement) { ordered << town; } }
	ordered.append(newTowns);
	for (const auto& village:settlements) { if (village.kind==Kind::RuralSettlement) { ordered << village; } }
	settlements=std::move(ordered);
	DBG_LOG(U"[NewTownPlacement] districts={}"_fmt(newTowns.size()));
	// 間引きで消えた町の番号を残さず、残存する中心地に生活圏を割り当て直す。
	Array<Settlement> centers;
	for (const auto& settlement : settlements) { if (settlement.kind != Kind::RuralSettlement) { centers << settlement; } }
	const TravelField retainedAccess(bounds, height, centers, water);
	for (auto& settlement : settlements)
	{
		if (settlement.kind == Kind::RuralSettlement) { retainedAccess.assign(settlement, centers); }
	}
	int cities = 0, towns = 0, villages = 0, linked = 0;
	for (const auto& settlement : settlements)
	{
		cities += settlement.kind == Kind::RegionalCity;
		towns += settlement.kind == Kind::LocalTown;
		villages += settlement.kind == Kind::RuralSettlement;
		linked += settlement.serviceCenter.has_value();
	}
	DBG_LOG(U"[SettlementHierarchy] cities={} towns={} villages={} catchmentVillages={} candidates={}"_fmt(cities, towns, villages, linked, sites.size()));
	return settlements;
}

double SettlementPlacement::roadAccessCost(Vec2 from, Vec2 target, const Optional<Vec2>& preferredDirection)
{
	const Vec2 delta = target - from;
	const double distance = delta.length();
	if (!preferredDirection || distance < 1) { return distance; }
	const double alignment = Clamp(delta.dot(*preferredDirection) / distance, -1.0, 1.0);
	return distance * (1 + GenerationSettings::get().placement_accessDirectionWeight * (1 - alignment));
}
// ─────────────────────────────────────────────────────────────────────────────
// 地形適性スコア
// ─────────────────────────────────────────────────────────────────────────────

float MapGenerator::scoreSuitability(const RoadPathfinder& pf, int gx, int gz)
{
	// 集落候補セルを標高と周辺傾斜だけで粗くふるい、道路生成前の立地適性を決める。
	const float h = pf.height(gx, gz);
	if (h < GenerationSettings::get().placement_minimumSiteHeight || h > GenerationSettings::get().placement_maximumSiteHeight) return 0.0f;

	// elevation: 低地ほど高スコア
	float elevation;
	if      (h < GenerationSettings::get().placement_lowlandHeight)  elevation = 1.0f;
	else if (h < GenerationSettings::get().placement_uplandHeight) elevation = GenerationSettings::get().placement_uplandScore;
	else if (h < GenerationSettings::get().placement_mountainHeight) elevation = GenerationSettings::get().placement_mountainScore;
	else                 elevation = GenerationSettings::get().placement_highMountainScore;

	// slope: 周囲3x3の最大傾斜をチェック
	const int gridW = pf.gridW();
	const int gridH = pf.gridH();
	float maxSlope = 0.0f;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
		{
			if (dx == 0 && dz == 0) continue;
			const int nx = gx + dx, nz = gz + dz;
			if (nx < 0 || nx >= gridW || nz < 0 || nz >= gridH) continue;
			const float slope = std::abs(pf.height(nx, nz) - h) / pf.cellSize();
			if (slope > maxSlope) maxSlope = slope;
		}
	}

	float slopeBonus;
	if      (maxSlope < GenerationSettings::get().placement_flatGrade) slopeBonus = 1.0f;
	else if (maxSlope < GenerationSettings::get().placement_gentleGrade) slopeBonus = GenerationSettings::get().placement_gentleGradeScore;
	else if (maxSlope < GenerationSettings::get().placement_maximumSiteGrade) slopeBonus = GenerationSettings::get().placement_slopingSiteScore;
	else                       return 0.0f;  // 傾斜10%超は不適

	return elevation * slopeBonus;
}

Array<MapGenerator::Settlement> MapGenerator::placeAllSettlements(
	uint64 seed, const World& world)
{
	// 適地候補を集め、地形上の生活圏を持つ集落配置へ渡す。
	const Stopwatch sw{ StartImmediately::Yes };

	const float kCellSize = GenerationSettings::get().placement_candidateCellSize;
	const int kGridW = static_cast<int>(Ceil(CHUNK_SIZE / kCellSize)), kGridH = kGridW;
	// 候補セルを収集（スコア付き）
	Array<SettlementPlacement::Candidate> candidates;

	for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
	{
		for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
		{
			const Point coord{ cx, cy };
			const Chunk* chunk = world.getChunk(coord);
			if (!chunk) continue;

			RoadPathfinder pf;
			pf.setupFromHeightMap(chunk->heightMap, coord, kGridW, kGridH, kCellSize);

			for (int gz = GenerationSettings::get().placement_candidateMarginCells; gz < kGridH - GenerationSettings::get().placement_candidateMarginCells; gz += GenerationSettings::get().placement_candidateStrideCells)
				for (int gx = GenerationSettings::get().placement_candidateMarginCells; gx < kGridW - GenerationSettings::get().placement_candidateMarginCells; gx += GenerationSettings::get().placement_candidateStrideCells)
				{
					const float sc = scoreSuitability(pf, gx, gz);
					if (sc > 0.0f)
						candidates << SettlementPlacement::Candidate{ pf.gridToWorld(gx, gz), sc };
				}
		}
	}

	auto settlements = SettlementPlacement::generate(seed, std::move(candidates),
		RectF{0, 0, WORLD_SIZE, WORLD_SIZE}, [&](const Vec2& p)
		{
			return world.sampleHeight(static_cast<float>(p.x), static_cast<float>(p.y));
		}, [&](const Vec2& p) { return world.waterSurfaceHeight(p.x, p.y); });
	JSON audit;
	for (size_t i = 0; i < settlements.size(); ++i)
	{
		const auto& settlement = settlements[i];
		JSON row;
		row[U"index"] = i;
		row[U"x"] = settlement.center.x;
		row[U"z"] = settlement.center.y;
		row[U"scale"] = static_cast<int>(settlement.kind);
		row[U"origin"] = UrbanMorphology::originName(settlement.plan.origin);
		row[U"urbanStructure"] = String{UrbanStructure::id(settlement.plan.structure)};
		row[U"centers"] = settlement.plan.centers.size();
		row[U"accessCost"] = settlement.accessCost;
		row[U"height"] = world.sampleHeight(static_cast<float>(settlement.center.x), static_cast<float>(settlement.center.y));
		row[U"water"] = world.waterSurfaceHeight(settlement.center.x, settlement.center.y);
		if (settlement.serviceCenter) { row[U"serviceCenter"] = *settlement.serviceCenter; }
		audit[U"settlements"].push_back(row);
	}
	audit[U"seed"] = seed;
	audit.save(U"settlement_audit.json");
	DBG_LOG(U"[SettlementPlacement] total={} elapsedMs={:.1f}"_fmt(settlements.size(), sw.msF()));
	return settlements;
}
