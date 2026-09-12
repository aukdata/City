#include "MapGenerator.hpp"
#include "RailwayAlignment.hpp"
#include "StreetProfile.hpp"
#include "../debug/DebugLog.hpp"
#include "PlaceNameGenerator.hpp"
#include "DistrictRoads.hpp"
#include <random>
#include <queue>

// ─────────────────────────────────────────────────────────────────────────────
// initWorld()
// ─────────────────────────────────────────────────────────────────────────────

MapGenerator::InitResult MapGenerator::initWorld(
	uint64 seed, World& world)
{
	// ワールド生成パラメータと地名供給源だけを先に初期化し、後段フェーズの前提をそろえる。
	world.setGenerationParams(seed,
		WORLD_SIZE,
		WORLD_SIZE);

	PlaceNameGenerator placeGen;
	placeGen.load(U"assets/placenames/placenames.toml");

	InitResult result;
	result.placeNames = placeGen.generate(4096, seed);
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 地形適性スコア
// ─────────────────────────────────────────────────────────────────────────────

float MapGenerator::scoreSuitability(const RoadPathfinder& pf, int gx, int gz)
{
	// 集落候補セルを標高と周辺傾斜だけで粗くふるい、道路生成前の立地適性を決める。
	const float h = pf.height(gx, gz);
	if (h < 0.5f || h > 800.0f) return 0.0f;

	// elevation: 低地ほど高スコア
	float elevation;
	if      (h < 50.0f)  elevation = 1.0f;
	else if (h < 200.0f) elevation = 0.7f;
	else if (h < 500.0f) elevation = 0.3f;
	else                 elevation = 0.1f;

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
	if      (maxSlope < 0.03f) slopeBonus = 1.0f;
	else if (maxSlope < 0.05f) slopeBonus = 0.8f;
	else if (maxSlope < 0.10f) slopeBonus = 0.5f;
	else                       return 0.0f;  // 傾斜10%超は不適

	return elevation * slopeBonus;
}

// ─────────────────────────────────────────────────────────────────────────────
// 空間ハッシュ（道路ノード最近傍検索用）
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	struct RoadNodeSpatialHash
	{
		static constexpr float kBucketSize = 200.0f;
		HashTable<int64, Array<int>> buckets;

		static int64 key(float x, float z)
		{
			const int bx = static_cast<int>(Math::Floor(x / kBucketSize));
			const int bz = static_cast<int>(Math::Floor(z / kBucketSize));
			return (static_cast<int64>(bx) << 32) | static_cast<uint32>(bz);
		}

		void insert(Vec3 pos, int nodeId)
		{
			buckets[key(static_cast<float>(pos.x), static_cast<float>(pos.z))] << nodeId;
		}

		/// @brief 最寄りノードを検索（maxDist 以内、excludeId を除外、見つからなければ -1）
		int findNearest(Vec3 pos, const RoadNetwork& net, float maxDist, int excludeId = -1) const
		{
			const float px = static_cast<float>(pos.x);
			const float pz = static_cast<float>(pos.z);
			const int range = static_cast<int>(Ceil(maxDist / kBucketSize));
			const int bx0 = static_cast<int>(Math::Floor(px / kBucketSize));
			const int bz0 = static_cast<int>(Math::Floor(pz / kBucketSize));

			float bestDistSq = maxDist * maxDist;
			int bestId = -1;

			for (int dz = -range; dz <= range; ++dz)
			{
				for (int dx = -range; dx <= range; ++dx)
				{
					const int64 k = (static_cast<int64>(bx0 + dx) << 32) | static_cast<uint32>(bz0 + dz);
					const auto it = buckets.find(k);
					if (it == buckets.end()) continue;
					for (const int nid : it->second)
					{
						if (nid == excludeId) continue;
						const RoadNode* node = net.getNode(nid);
						if (!node) continue;
						const float ddx = static_cast<float>(node->position.x) - px;
						const float ddz = static_cast<float>(node->position.z) - pz;
						const float dSq = ddx * ddx + ddz * ddz;
						if (dSq < bestDistSq)
						{
							bestDistSq = dSq;
							bestId = nid;
						}
					}
				}
			}
			return bestId;
		}

		/// @brief 指定ノードの前後ノードから接線方向を返す
		static Vec2 tangentAt(int nodeId, const RoadNetwork& net)
		{
			const RoadNode* node = net.getNode(nodeId);
			if (!node || node->attachments.isEmpty()) return Vec2{ 1, 0 };

			// 最初の接続エッジの方向を返す
			const RoadEdge* edge = net.getEdge(node->attachments[0].edgeId);
			if (!edge) return Vec2{ 1, 0 };

			const RoadNode* other = net.getNode(
				(edge->nodeA == nodeId) ? edge->nodeB : edge->nodeA);
			if (!other) return Vec2{ 1, 0 };

			Vec2 dir{
				static_cast<float>(other->position.x - node->position.x),
				static_cast<float>(other->position.z - node->position.z)
			};
			const float len = static_cast<float>(dir.length());
			return (len > 0.01f) ? Vec2{ dir.x / len, dir.y / len } : Vec2{ 1, 0 };
		}
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// placeAllSettlements: 地形スコアベースの地区配置
// ─────────────────────────────────────────────────────────────────────────────

Array<MapGenerator::Settlement> MapGenerator::placeAllSettlements(
	uint64 seed, const World& world)
{
	// 候補セル収集、Poisson 間引き、城下町/宿場町への格上げを順に行って地区一覧を作る。
	const Stopwatch sw{ StartImmediately::Yes };
	std::mt19937_64 rng(seed ^ 0xABCD1234ULL);

	// 候補セルを収集（スコア付き）
	struct Candidate { Vec2 pos; float score; };
	Array<Candidate> candidates;

	for (int cy = 0; cy < WORLD_CHUNKS; ++cy)
	{
		for (int cx = 0; cx < WORLD_CHUNKS; ++cx)
		{
			const Point coord{ cx, cy };
			const Chunk* chunk = world.getChunk(coord);
			if (!chunk) continue;

			RoadPathfinder pf;
			pf.setupFromHeightMap(chunk->heightMap, coord, kGridW, kGridH, kCellSize);

			for (int gz = 2; gz < kGridH - 2; ++gz)
				for (int gx = 2; gx < kGridW - 2; ++gx)
				{
					const float sc = scoreSuitability(pf, gx, gz);
					if (sc > 0.0f)
						candidates << Candidate{ pf.gridToWorld(gx, gz), sc };
				}
		}
	}

	// スコア降順でソート（同スコアはシャッフルで乱数化）
	std::shuffle(candidates.begin(), candidates.end(), rng);
	candidates.sort_by([](const Candidate& a, const Candidate& b)
	{
		return a.score > b.score;
	});

	Logger << U"[placeAllSettlements] 候補セル: {}"_fmt(candidates.size());

	// Poisson ディスクサンプリング（スコア降順で採用）
	// 8チャンクに1地区 ≒ 2.8km間隔
	constexpr float kMinDist   = 2800.0f;
	constexpr float kMinDistSq = kMinDist * kMinDist;
	constexpr float kHashCell  = kMinDist;

	HashTable<int64, Array<int>> spatialHash;
	Array<Settlement> settlements;

	for (const auto& [wp, sc] : candidates)
	{
		const int hx = static_cast<int>(wp.x / kHashCell);
		const int hz = static_cast<int>(wp.y / kHashCell);

		bool tooClose = false;
		for (int dz = -2; dz <= 2 && !tooClose; ++dz)
		{
			for (int dx = -2; dx <= 2 && !tooClose; ++dx)
			{
				const int64 cellKey = (static_cast<int64>(hx + dx) << 32) | static_cast<uint32>(hz + dz);
				const auto it = spatialHash.find(cellKey);
				if (it == spatialHash.end()) continue;
				for (const int idx : it->second)
				{
					if (wp.distanceFromSq(settlements[idx].center) < kMinDistSq)
					{
						tooClose = true;
						break;
					}
				}
			}
		}
		if (tooClose) continue;

		Settlement s;
		s.center = wp;
		s.kind   = SettlementKind::RuralSettlement;
		s.radius = 150.0f;
		s.score  = sc;

		const int idx = static_cast<int>(settlements.size());
		const int64 cellKey = (static_cast<int64>(hx) << 32) | static_cast<uint32>(hz);
		spatialHash[cellKey] << idx;
		settlements << s;
	}

	Logger << U"[placeAllSettlements] Poisson 完了: {} 地区"_fmt(settlements.size());

	// 種別割当て
	constexpr float kCastleTownExclusionSq = 20000.0f * 20000.0f;  // 20km 排他
	constexpr float kCastleTownMinScore    = 0.6f;

	// Pass 1: スコア上位の候補を城下町化（排他距離内に他の城下町を置かない）
	// settlements はスコア降順で入っているので、先頭から走査すればスコア最高から決まる
	for (auto& s : settlements)
	{
		if (s.score < kCastleTownMinScore) continue;
		bool canBeCastleTown = true;
		for (const auto& other : settlements)
		{
			if (&other == &s) continue;
			if (other.kind == SettlementKind::RegionalCity &&
			    static_cast<float>(s.center.distanceFromSq(other.center)) < kCastleTownExclusionSq)
			{
				canBeCastleTown = false;
				break;
			}
		}
		if (canBeCastleTown)
		{
			s.kind   = SettlementKind::RegionalCity;
			s.radius = 700.0f;
		}
	}

	// Secondary centres reflect a rural market catchment as well as proximity to a city.
	// Their size does not determine whether they originated as a post town.
	for (size_t index=0;index<settlements.size();++index)
	{
		auto& settlement=settlements[index];
		if (settlement.kind==SettlementKind::RegionalCity) { continue; }
		int neighbours=0;
		bool nearRegional=false;
		for (const auto& other : settlements)
		{
			const double distance=settlement.center.distanceFrom(other.center);
			if (distance>1 && distance<7500) { ++neighbours; }
			if (other.kind==SettlementKind::RegionalCity && distance<6000) { nearRegional=true; }
		}
		const uint64 salt=UrbanMorphology::mix(seed ^ (index*0x9e3779b97f4a7c15ULL));
		if (nearRegional || (neighbours>=4 && salt%5==0))
		{
			settlement.kind=SettlementKind::LocalTown;
			settlement.radius=360;
		}
	}
	Array<int> counts(8,0);
	for (size_t index=0;index<settlements.size();++index)
	{
		auto& settlement=settlements[index];
		const auto site=UrbanMorphology::inspectSite(settlement.center,[&](const Vec2& p)
		{
			return world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.y));
		});
		bool nearRegional=false;
		for (const auto& other : settlements)
		{
			if (&other!=&settlement && other.kind==SettlementKind::RegionalCity
				&& settlement.center.distanceFrom(other.center)<8000) { nearRegional=true; break; }
		}
		const uint64 salt=UrbanMorphology::mix(seed ^ (index*0x9e3779b97f4a7c15ULL));
		const auto origin=UrbanMorphology::chooseOrigin(static_cast<uint8>(settlement.kind),site,salt,nearRegional);
		const bool railway=settlement.kind==SettlementKind::RegionalCity || origin==UrbanMorphology::Origin::Planned;
		settlement.plan=UrbanMorphology::makePlan(origin,static_cast<uint8>(settlement.kind),site,salt,railway);
		if (origin==UrbanMorphology::Origin::Port)
		{
			settlement.gridAxisZ=site.shoreDirection;
			settlement.gridAxisX={site.shoreDirection.y,-site.shoreDirection.x};
		}
		else if (settlement.plan.ruralForm==UrbanMorphology::RuralForm::Valley)
		{
			settlement.gridAxisX=site.contourAxis;
			settlement.gridAxisZ={-site.contourAxis.y,site.contourAxis.x};
		}
		++counts[static_cast<size_t>(origin)];
	}
	for (size_t origin=0;origin<counts.size();++origin)
	{
		DBG_LOG(U"[SettlementOrigins] origin={} count={}"_fmt(UrbanMorphology::originName(static_cast<UrbanMorphology::Origin>(origin)),counts[origin]));
	}
	DBG_LOG(U"[SettlementOrigins] total={} elapsedMs={:.1f}"_fmt(settlements.size(),sw.msF()));

	return settlements;
}

// ─────────────────────────────────────────────────────────────────────────────
// MST サブセット（Prim's O(n^2)）
// ─────────────────────────────────────────────────────────────────────────────

Array<std::pair<int,int>> MapGenerator::computeMSTSubset(
	const Array<Settlement>& settlements, const Array<int>& indices)
{
	// 地区集合の骨格接続だけを得るため、完全グラフを明示せず Prim 法で最小木を組む。
	const int n = static_cast<int>(indices.size());
	if (n <= 1) return {};

	Array<float> minCost(n, 1e30f);
	Array<int>   minEdge(n, -1);
	Array<bool>  inTree(n, false);
	Array<std::pair<int,int>> mst;

	minCost[0] = 0.0f;

	for (int iter = 0; iter < n; ++iter)
	{
		int u = -1;
		float best = 1e30f;
		for (int i = 0; i < n; ++i)
		{
			if (!inTree[i] && minCost[i] < best)
			{
				best = minCost[i];
				u = i;
			}
		}
		if (u < 0) break;

		inTree[u] = true;
		if (minEdge[u] >= 0)
			mst << std::make_pair(minEdge[u], u);

		for (int v = 0; v < n; ++v)
		{
			if (inTree[v]) continue;
			const float d = static_cast<float>(
				settlements[indices[u]].center.distanceFrom(
					settlements[indices[v]].center));
			if (d < minCost[v])
			{
				minCost[v] = d;
				minEdge[v] = u;
			}
		}
	}

	return mst;
}

// ─────────────────────────────────────────────────────────────────────────────
// MST diameter（最長パス）
// ─────────────────────────────────────────────────────────────────────────────

Array<int> MapGenerator::findMSTDiameter(
	const Array<std::pair<int,int>>& mst, int nodeCount)
{
	// 最小木の主幹線候補を得るため、2 回 BFS で最長パスを抽出する。
	if (nodeCount <= 1) return { 0 };

	// 隣接リスト
	Array<Array<int>> adj(nodeCount);
	for (const auto& [u, v] : mst)
	{
		adj[u] << v;
		adj[v] << u;
	}

	// BFS で最遠点を見つける
	auto bfs = [&](int start) -> std::pair<int, Array<int>>
	{
		Array<int> dist(nodeCount, -1);
		Array<int> parent(nodeCount, -1);
		std::queue<int> q;
		q.push(start);
		dist[start] = 0;
		int farthest = start;

		while (!q.empty())
		{
			const int u = q.front(); q.pop();
			for (const int v : adj[u])
			{
				if (dist[v] >= 0) continue;
				dist[v] = dist[u] + 1;
				parent[v] = u;
				q.push(v);
				if (dist[v] > dist[farthest])
					farthest = v;
			}
		}

		// パスを復元
		Array<int> path;
		for (int cur = farthest; cur >= 0; cur = parent[cur])
			path << cur;
		path.reverse();
		return { farthest, path };
	};

	// 2回BFS: 任意→最遠u、u→最遠v
	const auto [u, _] = bfs(0);
	const auto [v, path] = bfs(u);
	return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// A* 道路セグメント生成ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// グローバル占有セルのキー（ワールド座標を粗いグリッドに量子化）
	constexpr float kOccupyCellSize = 120.0f;
	int64 occupyKey(float wx, float wz)
	{
		const int gx = static_cast<int>(Math::Floor(wx / kOccupyCellSize));
		const int gz = static_cast<int>(Math::Floor(wz / kOccupyCellSize));
		return (static_cast<int64>(gx) << 32) | static_cast<uint32>(gz);
	}
}

void MapGenerator::buildRoadSegment(
	const World& world, RoadNetwork& network,
	int startNodeId, Vec3 startPos, int endNodeId, Vec3 endPos,
	RoadType roadType, int lanes,
	HashSet<int64>& globalOccupied,
	const Array<Vec2>& forbiddenStartDirs,
	const Array<Vec2>& forbiddenGoalDirs,
	Array<int>* outEdgeIds)
{
	constexpr float kMargin = 300.0f;
	const float sx = static_cast<float>(startPos.x);
	const float sz = static_cast<float>(startPos.z);
	const float ex = static_cast<float>(endPos.x);
	const float ez = static_cast<float>(endPos.z);

	const float minX = Min(sx, ex) - kMargin;
	const float minZ = Min(sz, ez) - kMargin;
	const float maxX = Max(sx, ex) + kMargin;
	const float maxZ = Max(sz, ez) + kMargin;

	const float dist = std::sqrt((ex - sx) * (ex - sx) + (ez - sz) * (ez - sz));
	const float cellSize = (dist > 5000.0f) ? 120.0f
	                     : (dist > 2000.0f) ? 80.0f
	                     : RoadPathfinder::kDefaultCellSize;

	const int pfW = Max(2, static_cast<int>(Ceil((maxX - minX) / cellSize)));
	const int pfH = Max(2, static_cast<int>(Ceil((maxZ - minZ) / cellSize)));

	RoadPathfinder pf;
	pf.setup(world, Vec2{ minX, minZ }, pfW, pfH, cellSize);

	const Point gs = pf.worldToGrid(sx, sz);
	const Point ge = pf.worldToGrid(ex, ez);

	// グローバル占有セルをローカルグリッドの occupiedCells に変換
	HashSet<int> localOccupied;
	for (const int64 key : globalOccupied)
	{
		const int gx = static_cast<int>(key >> 32);
		const int gz = static_cast<int>(static_cast<uint32>(key));
		const float wx = (gx + 0.5f) * kOccupyCellSize;
		const float wz = (gz + 0.5f) * kOccupyCellSize;
		const Point lp = pf.worldToGrid(wx, wz);
		if (lp.x >= 0 && lp.x < pfW && lp.y >= 0 && lp.y < pfH)
			localOccupied.insert(lp.y * pfW + lp.x);
	}

	const int sampleStep = (cellSize > 60.0f) ? 2 : 3;

	const Array<Point> path = pf.findPath(gs, ge, forbiddenStartDirs, forbiddenGoalDirs, localOccupied);

	if (path.isEmpty() || path.size() < 2)
	{
		if (auto eid = network.addEdge(startNodeId, endNodeId,
		                startPos + (endPos - startPos) * (1.0 / 3.0),
		                startPos + (endPos - startPos) * (2.0 / 3.0),
		                roadType, lanes))
		{
			if (outEdgeIds) *outEdgeIds << *eid;
		}
	}
	else
	{
		// 通過セルをグローバル占有セットに登録
		for (const Point& gp : path)
		{
			const Vec2 wp = pf.gridToWorld(gp.x, gp.y);
			globalOccupied.insert(occupyKey(static_cast<float>(wp.x), static_cast<float>(wp.y)));
		}

		Array<Vec3> wps = pf.samplePath(path, sampleStep);
		wps.front() = startPos;
		wps.back()  = endPos;
		pf.pathToRoadEdges(wps, network, roadType, lanes, startNodeId, endNodeId, outEdgeIds);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// generateGlobalRoads: 3層分岐構造で道路を一括生成する
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::generateGlobalRoads(
	[[maybe_unused]] uint64 seed,
	const Array<Settlement>& settlements,
	const World& world,
	RoadNetwork& network,
	ProgressCallback onProgress)
{
	if (settlements.isEmpty()) return;

	const Stopwatch swTotal{ StartImmediately::Yes };
	Stopwatch swStep{ StartImmediately::Yes };

	// 全地区のノードを作成
	Array<int> nodeIds;
	nodeIds.reserve(settlements.size());
	for (const auto& s : settlements)
	{
		const float y = world.computeHeight(
			static_cast<float>(s.center.x), static_cast<float>(s.center.y));
		nodeIds << network.addNode(Vec3{ s.center.x, y, s.center.y }, NodeType::Intersection);
	}

	// 種別ごとにインデックスを分類
	Array<int> castleTownIdx, postTownIdx, villageIdx;
	for (int i = 0; i < static_cast<int>(settlements.size()); ++i)
	{
		switch (settlements[i].kind)
		{
		case SettlementKind::RegionalCity:   castleTownIdx << i; break;
		case SettlementKind::LocalTown: postTownIdx << i; break;
		case SettlementKind::RuralSettlement:   villageIdx << i; break;
		}
	}

	Logger << U"[Roads] ノード作成: {} (城下町={}, 宿場町={}, 農村={})"_fmt(
		nodeIds.size(), castleTownIdx.size(), postTownIdx.size(), villageIdx.size());

	RoadNodeSpatialHash roadHash;
	HashSet<int64> globalOccupied;  // 既存道路が通過するワールドセルの集合

	// =====================================================================
	// Layer 1: 幹線街道（城下町間チェーン）
	// plan/22_road_route_spec.md §1 参照: 生成した幹線街道を RoadRoute::NationalRoute として登録
	// =====================================================================
	swStep.restart();

	// 国道指定用の edge ID 収集
	Array<int> chainEdges;   // diameter チェーンの連続 edge 群
	Array<int> frontExtEdges; // diameter.front() 側の map 延伸 (城下町 → map edge 順で格納)
	Array<int> backExtEdges;  // diameter.back() 側の map 延伸
	Array<Array<int>> branchRoutes;  // MST 枝線 (diameter に含まれないもの)

	if (castleTownIdx.size() >= 2)
	{
		// 城下町のみで MST を計算
		auto castleTownMST = computeMSTSubset(settlements, castleTownIdx);

		// MST 上の diameter（最長パス）を求める
		Array<int> diameter = findMSTDiameter(castleTownMST, static_cast<int>(castleTownIdx.size()));

		// diameter 上のノードをセットに登録（枝線判定用）
		HashSet<int> diameterSet;
		for (const int di : diameter)
			diameterSet.insert(di);

		// diameter チェーンを A* で接続
		for (int i = 0; i + 1 < static_cast<int>(diameter.size()); ++i)
		{
			const int si = castleTownIdx[diameter[i]];
			const int ei = castleTownIdx[diameter[i + 1]];
			const RoadNode* sn = network.getNode(nodeIds[si]);
			const RoadNode* en = network.getNode(nodeIds[ei]);
			if (!sn || !en) continue;

			buildRoadSegment(world, network,
				nodeIds[si], sn->position, nodeIds[ei], en->position,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &chainEdges);
		}

		// MST 枝線（diameter に含まれない城下町 → diameter 上の親へ接続）
		for (const auto& [parentLocal, childLocal] : castleTownMST)
		{
			if (diameterSet.contains(parentLocal) && diameterSet.contains(childLocal))
				continue;  // diameter 上の辺は既に処理済み

			const int si = castleTownIdx[parentLocal];
			const int ei = castleTownIdx[childLocal];
			const RoadNode* sn = network.getNode(nodeIds[si]);
			const RoadNode* en = network.getNode(nodeIds[ei]);
			if (!sn || !en) continue;

			Array<int> branchEdges;
			buildRoadSegment(world, network,
				nodeIds[si], sn->position, nodeIds[ei], en->position,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &branchEdges);
			if (!branchEdges.isEmpty())
				branchRoutes << std::move(branchEdges);
		}
		// diameter 両端からマップ外端への幹線延伸
		const float worldSize = WORLD_SIZE;
		const int endIndices[2] = { diameter.front(), diameter.back() };
		for (int ei2 = 0; ei2 < 2; ++ei2)
		{
			const int di = endIndices[ei2];
			const int si = castleTownIdx[di];
			const RoadNode* sn = network.getNode(nodeIds[si]);
			if (!sn) continue;

			const float sx = static_cast<float>(sn->position.x);
			const float sz = static_cast<float>(sn->position.z);

			// 4辺への距離を計算し、最も近い外端の座標を選ぶ
			const float dLeft   = sx;
			const float dRight  = worldSize - sx;
			const float dTop    = sz;
			const float dBottom = worldSize - sz;
			const float minEdgeDist = Min({ dLeft, dRight, dTop, dBottom });

			float edgeX, edgeZ;
			if (minEdgeDist == dLeft)        { edgeX = 0.0f;      edgeZ = sz; }
			else if (minEdgeDist == dRight)  { edgeX = worldSize;  edgeZ = sz; }
			else if (minEdgeDist == dTop)    { edgeX = sx;         edgeZ = 0.0f; }
			else                             { edgeX = sx;         edgeZ = worldSize; }

			// マップ端に少し余裕を持たせる（10m内側）
			edgeX = Clamp(edgeX, 10.0f, worldSize - 10.0f);
			edgeZ = Clamp(edgeZ, 10.0f, worldSize - 10.0f);

			const float ey = world.computeHeight(edgeX, edgeZ);
			const Vec3 edgePos{ edgeX, ey, edgeZ };
			const int edgeNodeId = network.addNode(edgePos, NodeType::Endpoint);

			Array<int>& extOut = (ei2 == 0) ? frontExtEdges : backExtEdges;
			buildRoadSegment(world, network,
				nodeIds[si], sn->position, edgeNodeId, edgePos,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &extOut);
		}
	}
	else if (castleTownIdx.size() == 1)
	{
		// 城下町が1つだけ → マップ外端への接続のみ
		const float worldSize = WORLD_SIZE;
		const int si = castleTownIdx[0];
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (sn)
		{
			const float sx = static_cast<float>(sn->position.x);
			const float sz = static_cast<float>(sn->position.z);
			const float dLeft = sx, dRight = worldSize - sx;
			const float dTop = sz, dBottom = worldSize - sz;
			const float minD = Min({ dLeft, dRight, dTop, dBottom });
			float ex, ez;
			if (minD == dLeft)       { ex = 10.0f;           ez = sz; }
			else if (minD == dRight) { ex = worldSize - 10.0f; ez = sz; }
			else if (minD == dTop)   { ex = sx;               ez = 10.0f; }
			else                     { ex = sx;               ez = worldSize - 10.0f; }
			const float ey = world.computeHeight(ex, ez);
			const Vec3 ep{ ex, ey, ez };
			const int enid = network.addNode(ep, NodeType::Endpoint);
			buildRoadSegment(world, network, nodeIds[si], sn->position, enid, ep,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &chainEdges);
		}
	}

	// Layer 1 幹線を国道として登録
	// 主要国道: frontExt (逆順) + chain + backExt を 1 本の route として
	{
		Array<int> mainRoute;
		mainRoute.reserve(frontExtEdges.size() + chainEdges.size() + backExtEdges.size());
		// frontExt は 城下町→map_edge 順で格納されているので、route 先頭に持ってくるには逆順
		for (int i = static_cast<int>(frontExtEdges.size()) - 1; i >= 0; --i)
			mainRoute << frontExtEdges[i];
		for (const int eid : chainEdges)    mainRoute << eid;
		for (const int eid : backExtEdges)  mainRoute << eid;

		if (!mainRoute.isEmpty())
		{
			network.addRoute(RoadRouteKind::NationalRoute, U"", std::move(mainRoute));
		}
	}
	// 支線国道（MST 枝線）
	for (auto& br : branchRoutes)
	{
		if (!br.isEmpty())
			network.addRoute(RoadRouteKind::NationalRoute, U"", std::move(br));
	}
	Logger << U"[Roads] Layer1 国道指定: main={} branches={}"_fmt(
		(chainEdges.size() + frontExtEdges.size() + backExtEdges.size() > 0 ? 1 : 0),
		branchRoutes.size());

	// Layer 1 の全中間ノードを空間ハッシュに登録
	for (const auto& node : network.nodes())
	{
		if (node.id >= 0)
			roadHash.insert(node.position, node.id);
	}

	Logger << U"[Roads] Layer1 幹線街道: {:.0f}ms (nodes={}, edges={})"_fmt(
		swStep.msF(), network.nodes().size(), network.edges().size());
	if (onProgress) onProgress(0.15f);

	// =====================================================================
	// Layer 2: 地方道（宿場町 → 最寄り幹線ノードへの分岐）
	// =====================================================================
	swStep.restart();
	int postTownsConnected = 0;
	Array<int> deferredPostTowns;

	for (const int si : postTownIdx)
	{
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (!sn) continue;

		const int nearId = roadHash.findNearest(sn->position, network, 5000.0f, nodeIds[si]);
		if (nearId < 0)
		{
			deferredPostTowns << si;
			continue;
		}

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) { deferredPostTowns << si; continue; }

		// Y字分岐: 幹線の接線方向を forbiddenGoalDirs に設定
		const Vec2 tangent = RoadNodeSpatialHash::tangentAt(nearId, network);
		buildRoadSegment(world, network,
			nearId, nearNode->position, nodeIds[si], sn->position,
			RoadType::Arterial, 2, globalOccupied,
			{}, { tangent });

		++postTownsConnected;
	}

	// deferred: 幹線から5km以上離れた宿場町 → 最寄りの接続済み宿場町へ
	// まず空間ハッシュを最新ノードで一括更新
	{
		const auto& nodes = network.nodes();
		for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
			if (nodes[i].id >= 0)
				roadHash.insert(nodes[i].position, nodes[i].id);
	}

	for (const int si : deferredPostTowns)
	{
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (!sn) continue;

		const int nearId = roadHash.findNearest(sn->position, network, 15000.0f, nodeIds[si]);
		if (nearId < 0) continue;

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) continue;

		const int prevNodeCount = static_cast<int>(network.nodes().size());
		buildRoadSegment(world, network,
			nearId, nearNode->position, nodeIds[si], sn->position,
			RoadType::LocalRoad, 1, globalOccupied);

		// 新規ノードだけ空間ハッシュに追加
		const auto& nodes = network.nodes();
		for (int i = prevNodeCount; i < static_cast<int>(nodes.size()); ++i)
			if (nodes[i].id >= 0)
				roadHash.insert(nodes[i].position, nodes[i].id);

		++postTownsConnected;
	}

	Logger << U"[Roads] Layer2 地方道: {:.0f}ms (宿場町接続={}/{})"_fmt(
		swStep.msF(), postTownsConnected, postTownIdx.size());
	if (onProgress) onProgress(0.40f);

	// =====================================================================
	// Layer 3: 集落道（農村 → 最寄り道路ノードへの直線接続）
	// =====================================================================
	swStep.restart();

	// Layer 2 完了時点の全ノードで空間ハッシュを再構築
	roadHash.buckets.clear();
	for (const auto& node : network.nodes())
		if (node.id >= 0)
			roadHash.insert(node.position, node.id);

	int villagesConnected = 0, villagesRoadFacing = 0;
	const int totalVillages = static_cast<int>(villageIdx.size());

	for (int ri = 0; ri < totalVillages; ++ri)
	{
		const int si = villageIdx[ri];
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (!sn) continue;

		const int nearId = roadHash.findNearest(sn->position, network, 10000.0f, nodeIds[si]);
		if (nearId < 0) continue;

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) continue;

		const float dist = static_cast<float>(sn->position.distanceFrom(nearNode->position));

		if (dist < 300.0f)
		{
			++villagesRoadFacing;
		}
		else
		{
			const Vec2 tangent = RoadNodeSpatialHash::tangentAt(nearId, network);
			const int prevNodeCount = static_cast<int>(network.nodes().size());

			buildRoadSegment(world, network,
				nearId, nearNode->position, nodeIds[si], sn->position,
				RoadType::LocalRoad, 1, globalOccupied,
				{}, { tangent });

			// 新規ノードを空間ハッシュに追加（後続の農村が利用）
			const auto& nodes = network.nodes();
			for (int i = prevNodeCount; i < static_cast<int>(nodes.size()); ++i)
				if (nodes[i].id >= 0)
					roadHash.insert(nodes[i].position, nodes[i].id);

			++villagesConnected;
		}

		if (onProgress && (ri % 500 == 0))
			onProgress(0.40f + 0.60f * static_cast<float>(ri) / totalVillages);
	}

	Logger << U"[Roads] Layer3 集落道: {:.0f}ms (接続={}, 街道沿い={}, 計={})"_fmt(
		swStep.msF(), villagesConnected, villagesRoadFacing, totalVillages);
	if (onProgress) onProgress(0.90f);

	// =====================================================================
	// 最終パス: 未接続地区の強制接続
	// =====================================================================
	swStep.restart();

	// BFS で接続済みノードを判定
	HashSet<int> reachable;
	{
		// 最初の城下町ノードから BFS
		int startNode = -1;
		for (const int ui : castleTownIdx)
		{
			const RoadNode* n = network.getNode(nodeIds[ui]);
			if (n && !n->attachments.isEmpty()) { startNode = nodeIds[ui]; break; }
		}
		if (startNode < 0)
		{
			// 城下町がなければエッジを持つ任意のノードから
			for (const auto& n : network.nodes())
				if (n.id >= 0 && !n.attachments.isEmpty()) { startNode = n.id; break; }
		}

		if (startNode >= 0)
		{
			std::queue<int> q;
			q.push(startNode);
			reachable.insert(startNode);
			while (!q.empty())
			{
				const int cur = q.front(); q.pop();
				const RoadNode* node = network.getNode(cur);
				if (!node) continue;
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					const RoadEdge* edge = network.getEdge(eid);
					if (!edge) continue;
					const int next = (edge->nodeA == cur) ? edge->nodeB : edge->nodeA;
					if (!reachable.contains(next))
					{
						reachable.insert(next);
						q.push(next);
					}
				}
			}
		}
	}

	// 空間ハッシュを再構築（接続済みノードのみ）
	roadHash.buckets.clear();
	for (const auto& node : network.nodes())
		if (node.id >= 0 && reachable.contains(node.id))
			roadHash.insert(node.position, node.id);

	int forceConnected = 0;
	const int nSettlements = static_cast<int>(settlements.size());
	for (int i = 0; i < nSettlements; ++i)
	{
		if (reachable.contains(nodeIds[i])) continue;

		const RoadNode* sn = network.getNode(nodeIds[i]);
		if (!sn) continue;

		const int nearId = roadHash.findNearest(sn->position, network, 50000.0f, nodeIds[i]);
		if (nearId < 0) continue;

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) continue;

		buildRoadSegment(world, network,
			nearId, nearNode->position, nodeIds[i], sn->position,
			RoadType::LocalRoad, 1, globalOccupied);

		// BFS で新たに到達可能になったノードを reachable に追加
		{
			std::queue<int> q;
			q.push(nodeIds[i]);
			reachable.insert(nodeIds[i]);
			while (!q.empty())
			{
				const int cur = q.front(); q.pop();
				const RoadNode* node = network.getNode(cur);
				if (!node) continue;
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					const RoadEdge* edge = network.getEdge(eid);
					if (!edge) continue;
					const int next = (edge->nodeA == cur) ? edge->nodeB : edge->nodeA;
					if (!reachable.contains(next))
					{
						reachable.insert(next);
						q.push(next);
						roadHash.insert(network.getNode(next)->position, next);
					}
				}
			}
		}
		++forceConnected;
	}

	Logger << U"[Roads] 未接続強制接続: {:.0f}ms ({}地区)"_fmt(swStep.msF(), forceConnected);
	if (onProgress) onProgress(1.0f);

	Logger << U"[Roads] 合計: nodes={}, edges={} ({:.0f}ms)"_fmt(
		network.nodes().size(), network.edges().size(), swTotal.msF());
}

// ─────────────────────────────────────────────────────────────────────────────
// 地区内道路生成
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::generateDistrictRoads(
	uint64 seed,
	Array<Settlement>& settlements,
	const World& world,
	RoadNetwork& network,
	ProgressCallback onProgress)
{
	const Stopwatch swTotal{ StartImmediately::Yes };
	// Regional roads outside the eventual town footprint are two-way roads with shoulders.
	// Expressways retain their own grade-separated cross section.
	for (const auto& source : network.edges())
	{
		if (source.id<0) { continue; }
		if (source.roadType!=RoadType::Arterial && source.roadType!=RoadType::LocalRoad) { continue; }
		if (auto* edge=network.getEdge(source.id))
		{
			GeneratedStreet::apply(*edge,GeneratedStreet::describe(source.roadType==RoadType::Arterial
				? GeneratedStreet::Role::Regional : GeneratedStreet::Role::Village));
		}
	}


	for (int si = 0; si < static_cast<int>(settlements.size()); ++si)
	{
		auto& s = settlements[si];
		const float kaidoSearchRadius = Max(180.0f, s.radius * 1.6f);
		const DistrictRoads::KaidoSegment kaido = DistrictRoads::extractKaido(
			s, network, kaidoSearchRadius);

		DistrictRoads::generateSettlement(seed, si, s, kaido, world, network);

		if (onProgress)
			onProgress(static_cast<float>(si + 1) / settlements.size());
	}

	Array<int> emptyNodes;
	for (const auto& node : network.nodes())
	{
		if (node.id>=0 && node.attachments.isEmpty()) { emptyNodes << node.id; }
	}
	for (const int id : emptyNodes) { network.removeNode(id); }

	Logger << U"[DistrictRoads] 完了: districts={} ({:.0f}ms)"_fmt(
		settlements.size(), swTotal.msF());
}

// ─────────────────────────────────────────────────────────────────────────────
// 鉄道初期設定
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::setupTrain(TrainNetwork& trainNet, World& world,
                              const Array<Settlement>& districts, const RoadNetwork* roads)
{
	RailwayAlignment::generate(trainNet,world,districts,roads);
}
