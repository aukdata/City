#include "MapGenerator.hpp"
#include "PlaceNameGenerator.hpp"
#include <random>
#include <queue>

// ─────────────────────────────────────────────────────────────────────────────
// initWorld() : ワールドパラメータ設定 + 地名生成（軽量・メインスレッド）
// ─────────────────────────────────────────────────────────────────────────────

MapGenerator::InitResult MapGenerator::initWorld(
	uint64 seed, TerrainType terrainType, World& world)
{
	world.setGenerationParams(seed, terrainType, 10.0f * CHUNK_SIZE, 10.0f * CHUNK_SIZE);

	PlaceNameGenerator placeGen;
	placeGen.load(U"assets/placenames/placenames.toml");

	InitResult result;
	// 地名は仮の地区数で生成（後から地区データに名前を反映）
	result.placeNames = placeGen.generate(128, terrainType, seed);
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1: 高さグリッド構築
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::buildHeightGrid(const Grid<float>& heightMap, Point chunkCoord)
{
	// 事前計算済み heightMap からバイリニア補間でパスファインダーグリッドを構築
	m_pathfinder.setupFromHeightMap(heightMap, chunkCoord, kGridW, kGridH, kCellSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2: 地区配置（Poisson ディスクサンプリング）
// ─────────────────────────────────────────────────────────────────────────────

bool MapGenerator::isSuitable(int gx, int gz) const
{
	const float h = gridHeight(gx, gz);
	// 水域・低地・急峻な山岳を除外
	if (h < 0.5f || h > 800.0f) return false;

	// 近傍の最大傾斜チェック（10% 以下）
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
		{
			if (dx == 0 && dz == 0) continue;
			const int nx = gx + dx, nz = gz + dz;
			if (nx < 0 || nx >= kGridW || nz < 0 || nz >= kGridH) continue;
			const float dh = std::abs(gridHeight(nx, nz) - h);
			if (dh / kCellSize > 0.10f) return false;
		}
	}
	return true;
}

void MapGenerator::placeSettlements(uint64 seed, const Array<Vec2>& existingUrbanCenters)
{
	std::mt19937_64 rng(seed ^ 0xABCD1234ULL);

	// 候補セルを収集（端から2セル内側）
	Array<Point> candidates;
	for (int gz = 2; gz < kGridH - 2; ++gz)
		for (int gx = 2; gx < kGridW - 2; ++gx)
			if (isSuitable(gx, gz))
				candidates << Point{ gx, gz };

	// シャッフル
	std::shuffle(candidates.begin(), candidates.end(), rng);

	// Poisson ディスクサンプリング（1024m チャンクスケール）
	constexpr float kMinDist = 200.0f;
	constexpr float kMinDistSq = kMinDist * kMinDist;

	m_settlements.clear();

	for (const auto& c : candidates)
	{
		const Vec2 wp = gridToWorld(c.x, c.y);
		bool tooClose = false;
		for (const auto& s : m_settlements)
		{
			if (wp.distanceFromSq(s.center) < kMinDistSq)
			{
				tooClose = true;
				break;
			}
		}
		if (!tooClose)
		{
			Settlement s;
			s.center = wp;
			m_settlements << s;
		}
		if (m_settlements.size() >= 1) break;  // チャンク内は最大1地区
	}

	// 地区が 0 のときは条件を緩和して再試行（高さ上限を広げる）
	if (m_settlements.size() < 1)
	{
		m_settlements.clear();
		for (const auto& c : candidates)
		{
			const float h = gridHeight(c.x, c.y);
			if (h < 0.0f || h > 1000.0f) continue;

			const Vec2 wp = gridToWorld(c.x, c.y);
			bool tooClose = false;
			for (const auto& s : m_settlements)
			{
				if (wp.distanceFromSq(s.center) < kMinDistSq * 0.25f)
				{
					tooClose = true;
					break;
				}
			}
			if (!tooClose)
			{
				Settlement s;
				s.center = wp;
				m_settlements << s;
			}
			if (m_settlements.size() >= 1) break;
		}
	}

	if (m_settlements.isEmpty())
	{
		// フォールバック: マップ中央付近を都市核にする
		Settlement s;
		s.center = Vec2{ m_regionOffset.x + kMapWidth * 0.5f,
		                 m_regionOffset.y + kMapDepth * 0.5f };
		m_settlements << s;
	}

	// 種別・影響半径を割当てる
	// Poisson 採用順（擬似ランダム）のまま処理する
	// Urban  : 周囲 10 km 以内に別の Urban がない場合
	// Suburbs: 周囲  1 km 以内に Urban がある場合（全 Urban 確定後に判定）
	// Rural  : それ以外

	constexpr float kUrbanExclusionSq = 10000.0f * 10000.0f;
	constexpr float kSuburbsRadiusSq  =  1000.0f *  1000.0f;

	// まず全地区を Rural に初期化
	for (auto& s : m_settlements)
	{
		s.type   = SettlementType::Rural;
		s.radius = 150.0f;
	}

	// 例外: マップ中心に最も近い地区を先行 Urban 化
	const Vec2 mapCenter{ m_regionOffset.x + kMapWidth * 0.5f,
	                      m_regionOffset.y + kMapDepth * 0.5f };
	Settlement* centerSettlement = &m_settlements[0];
	float bestCenterDistSq = static_cast<float>(centerSettlement->center.distanceFromSq(mapCenter));
	for (auto& s : m_settlements)
	{
		const float d = static_cast<float>(s.center.distanceFromSq(mapCenter));
		if (d < bestCenterDistSq) { bestCenterDistSq = d; centerSettlement = &s; }
	}
	centerSettlement->type   = SettlementType::Urban;
	centerSettlement->radius = 700.0f;

	// パス 1: Urban を決定（処理済み Urban との距離で判定・先行 Urban を起点にする）
	for (auto& s : m_settlements)
	{
		bool canBeUrban = true;
		for (const auto& other : m_settlements)
		{
			if (&other == &s) continue;
			if (other.type == SettlementType::Urban &&
			    static_cast<float>(s.center.distanceFromSq(other.center)) < kUrbanExclusionSq)
			{
				canBeUrban = false;
				break;
			}
		}
		// 他チャンクの既存 Urban との距離チェック
		if (canBeUrban)
		{
			for (const Vec2& uc : existingUrbanCenters)
			{
				if (static_cast<float>(s.center.distanceFromSq(uc)) < kUrbanExclusionSq)
				{ canBeUrban = false; break; }
			}
		}
		if (canBeUrban)
		{
			s.type   = SettlementType::Urban;
			s.radius = 700.0f;
		}
	}

	// パス 2: Suburbs 判定（全 Urban 確定後）
	for (auto& s : m_settlements)
	{
		if (s.type == SettlementType::Urban) continue;
		for (const auto& other : m_settlements)
		{
			if (other.type == SettlementType::Urban &&
			    static_cast<float>(s.center.distanceFromSq(other.center)) < kSuburbsRadiusSq)
			{
				s.type   = SettlementType::Suburbs;
				s.radius = 300.0f;
				break;
			}
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: 旧道生成
// ─────────────────────────────────────────────────────────────────────────────

// --- MST（Kruskal 法）---

Array<std::pair<int,int>> MapGenerator::computeMST() const
{
	const int n = static_cast<int>(m_settlements.size());
	if (n <= 1) return {};

	// 全辺を距離でソート
	using Edge = std::tuple<float, int, int>;
	Array<Edge> edges;
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
		{
			const float d = static_cast<float>(m_settlements[i].center.distanceFrom(m_settlements[j].center));
			edges << Edge{ d, i, j };
		}
	edges.sort_by([](const Edge& a, const Edge& b){ return std::get<0>(a) < std::get<0>(b); });

	// Union-Find
	Array<int> parent(n);
	std::iota(parent.begin(), parent.end(), 0);

	std::function<int(int)> find = [&](int x) -> int
	{
		return parent[x] == x ? x : parent[x] = find(parent[x]);
	};

	Array<std::pair<int,int>> mst;
	for (const auto& [d, u, v] : edges)
	{
		const int pu = find(u), pv = find(v);
		if (pu != pv)
		{
			parent[pu] = pv;
			mst << std::make_pair(u, v);
			if (static_cast<int>(mst.size()) == n - 1) break;
		}
	}
	return mst;
}

// --- 旧道生成 メイン ---

void MapGenerator::generateRoads(RoadNetwork& roads, uint64 seed)
{
	if (m_settlements.isEmpty()) return;

	// 各地区の道路ノード ID を記録する
	const int n = static_cast<int>(m_settlements.size());
	Array<int> settleNodeId(n, -1);

	for (int i = 0; i < n; ++i)
	{
		const Vec2& c  = m_settlements[i].center;
		const Point gp = worldToGrid(static_cast<float>(c.x), static_cast<float>(c.y));
		const float y  = m_pathfinder.height(gp.x, gp.y);
		settleNodeId[i] = roads.addNode(Vec3{ c.x, y, c.y }, NodeType::Intersection);
	}

	// MST で幹線網を決定する
	const auto mstEdges = computeMST();

	// 生成済みパスが占有するグリッドセル（始終点除く）を記録する。
	// 後続パスがこれらのセルを通ると割高になり、平行重複を自然に回避する。
	HashSet<int> occupiedCells;

	// ノード nodeId に接続済みの全エッジの outward 方向（XZ 単位ベクトル）を返す。
	// findPath の鋭角ペナルティに渡すための禁止方向リストを構築する。
	auto getOutwardDirs = [&](int nodeId) -> Array<Vec2>
	{
		const RoadNode* node = roads.getNode(nodeId);
		if (!node) return {};
		Array<Vec2> dirs;
		for (int eid : node->edgeIds)
		{
			const RoadEdge* edge = roads.getEdge(eid);
			if (!edge) continue;
			const Vec3 nodePos = node->position;
			const Vec3 outward = (edge->nodeA == nodeId)
				? (edge->ctrlA - nodePos)
				: (edge->ctrlB - nodePos);
			const float len = static_cast<float>(Vec2{ outward.x, outward.z }.length());
			if (len > 1e-6f)
				dirs << Vec2{ outward.x, outward.z } / len;
		}
		return dirs;
	};

	for (const auto& [u, v] : mstEdges)
	{
		const Point gs = worldToGrid(
			static_cast<float>(m_settlements[u].center.x),
			static_cast<float>(m_settlements[u].center.y));
		const Point ge = worldToGrid(
			static_cast<float>(m_settlements[v].center.x),
			static_cast<float>(m_settlements[v].center.y));

		const Array<Vec2> forbidStart = getOutwardDirs(settleNodeId[u]);
		const Array<Vec2> forbidGoal  = getOutwardDirs(settleNodeId[v]);
		const Array<Point> path = m_pathfinder.findPath(gs, ge, forbidStart, forbidGoal, occupiedCells);
		if (path.isEmpty()) continue;

		// 使用セルを登録（始終点は地区ノードなので除外）
		for (int i = 1; i < static_cast<int>(path.size()) - 1; ++i)
			occupiedCells.emplace(path[i].y * kGridW + path[i].x);

		// 幹線種別: Urban ↔ Suburbs/Urban → 幹線道路、それ以外 → 一般道
		const bool isArterial =
			m_settlements[u].type != SettlementType::Rural &&
			m_settlements[v].type != SettlementType::Rural;
		const RoadType rt    = isArterial ? RoadType::Arterial : RoadType::LocalRoad;
		const int      lanes = isArterial ? 4 : 2;

		const Array<Vec3> wps = m_pathfinder.samplePath(path, 5);
		m_pathfinder.pathToRoadEdges(wps, roads, rt, lanes, settleNodeId[u], settleNodeId[v]);
	}

	// 迂回路を追加する（仕様書：全体の 30% 追加接続）
	std::mt19937_64 rng(seed ^ 0x99887766ULL);
	const int extraCount = Max(1, static_cast<int>(mstEdges.size() * 0.3));
	for (int k = 0; k < extraCount && n >= 3; ++k)
	{
		const int u = static_cast<int>(rng() % n);
		int v = static_cast<int>(rng() % (n - 1));
		if (v >= u) ++v;

		// 既に MST で接続されている辺は除外（簡易判定）
		bool alreadyConnected = false;
		for (const auto& [mu, mv] : mstEdges)
		{
			if ((mu == u && mv == v) || (mu == v && mv == u))
			{
				alreadyConnected = true;
				break;
			}
		}
		if (alreadyConnected) continue;

		const Point gs = worldToGrid(
			static_cast<float>(m_settlements[u].center.x),
			static_cast<float>(m_settlements[u].center.y));
		const Point ge = worldToGrid(
			static_cast<float>(m_settlements[v].center.x),
			static_cast<float>(m_settlements[v].center.y));

		const Array<Vec2> forbidStart = getOutwardDirs(settleNodeId[u]);
		const Array<Vec2> forbidGoal  = getOutwardDirs(settleNodeId[v]);
		const Array<Point> path = m_pathfinder.findPath(gs, ge, forbidStart, forbidGoal, occupiedCells);
		if (path.isEmpty()) continue;

		for (int i = 1; i < static_cast<int>(path.size()) - 1; ++i)
			occupiedCells.emplace(path[i].y * kGridW + path[i].x);

		const Array<Vec3> wps = m_pathfinder.samplePath(path, 5);
		m_pathfinder.pathToRoadEdges(wps, roads, RoadType::LocalRoad, 2,
		                     settleNodeId[u], settleNodeId[v]);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 鉄道初期設定
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::setupTrain(TrainNetwork& trainNet, const World& world,
                              const Array<Settlement>& districts)
{
	// Urban 地区と Suburbs 地区（最大 4 駅）を結ぶ路線を 1 本生成する
	Array<int> stationIds;

	for (const auto& s : districts)
	{
		if (s.type == SettlementType::Rural) continue;
		if (stationIds.size() >= 4) break;

		const float y  = world.computeHeight(
			static_cast<float>(s.center.x),
			static_cast<float>(s.center.y));
		const String name = (stationIds.isEmpty()) ? U"中央駅"
		                  : (stationIds.size() == 1) ? U"北駅"
		                  : (stationIds.size() == 2) ? U"南駅"
		                  : U"東駅";
		stationIds << trainNet.addStation(
			Vec3{ s.center.x, y, s.center.y }, name);
	}

	if (stationIds.size() < 2) return;

	// 隣接駅間をエッジで繋ぐ
	for (int i = 0; i + 1 < static_cast<int>(stationIds.size()); ++i)
	{
		const Vec3* na = trainNet.getNode(stationIds[i])     ? &trainNet.getNode(stationIds[i])->position     : nullptr;
		const Vec3* nb = trainNet.getNode(stationIds[i + 1]) ? &trainNet.getNode(stationIds[i + 1])->position : nullptr;
		if (!na || !nb) continue;
		trainNet.addEdge(stationIds[i], stationIds[i + 1],
		                 *na + (*nb - *na) * (1.0 / 3.0),
		                 *na + (*nb - *na) * (2.0 / 3.0),
		                 80.0f);
	}

	// 基本ダイヤ（仕様書 §6: 1日4往復相当）
	TrainSchedule sched;
	sched.id         = 0;
	sched.headwaySec = 600.0f;  // 10 分間隔
	sched.loop       = true;
	for (int nodeId : stationIds)
	{
		StopEntry stop;
		stop.stationNodeId = nodeId;
		stop.dwellSec      = 30.0f;
		sched.stops << stop;
	}
	trainNet.addSchedule(sched);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildChunk() : チャンク構築（スレッド安全）
// ─────────────────────────────────────────────────────────────────────────────

MapGenerator::ChunkBuildResult MapGenerator::buildChunk(
	Vec2 regionOffset, uint64 seed,
	const World& world,
	const Array<Vec2>& existingUrbanCenters,
	const Array<NodeSnapshot>& existingNodes,
	bool skipPostProcess)
{
	const int rx = static_cast<int>(regionOffset.x / kMapWidth);
	const int rz = static_cast<int>(regionOffset.y / kMapDepth);
	const uint64 regionSeed = seed
		^ (static_cast<uint64>(static_cast<uint32>(rx)) << 32)
		^ static_cast<uint64>(static_cast<uint32>(rz));

	ChunkBuildResult result;
	result.chunkCoord = Point{ rx, rz };

	// heightMap を先に生成し、パスファインダーグリッドはそこからバイリニア補間で導出する
	auto hmr = world.buildHeightMap(result.chunkCoord);
	result.terrainHeightMap = std::move(hmr.heightMap);
	result.terrainHeightMin = hmr.heightMin;
	result.terrainHeightMax = hmr.heightMax;

	MapGenerator gen;
	gen.m_regionOffset = regionOffset;
	gen.buildHeightGrid(result.terrainHeightMap, result.chunkCoord);
	gen.placeSettlements(regionSeed, existingUrbanCenters);

	result.gridOffset = gen.m_pathfinder.offset();
	result.gridW      = gen.m_pathfinder.gridW();
	result.gridH      = gen.m_pathfinder.gridH();
	result.cellSize   = gen.m_pathfinder.cellSize();
	result.heightGrid = gen.m_pathfinder.heightGrid();

	RoadNetwork tempRoads;

	if (!gen.m_settlements.isEmpty())
	{
		gen.generateRoads(tempRoads, regionSeed);

		// ---- 既存ネットワークへの接続 ----
		if (!existingNodes.isEmpty())
		{
			for (const auto& s : gen.m_settlements)
			{
				const float sy = sampleHeightMap(result.terrainHeightMap, result.chunkCoord,
					static_cast<float>(s.center.x), static_cast<float>(s.center.y));
				const Vec3 sPos{ s.center.x, sy, s.center.y };

				// ローカル RoadNetwork から settlement に最も近いノードを探す
				const Optional<int> sNodeOpt = tempRoads.findNodeNear(sPos, 50.0f);
				if (!sNodeOpt) continue;

				// 既存ノードスナップショットから最近傍を検索
				float bestDist   = 6000.0f;
				int   bestNodeId = -1;
				Vec3  bestNodePos{ 0, 0, 0 };
				for (const auto& snap : existingNodes)
				{
					const float d = static_cast<float>(
						Vec2{ snap.position.x, snap.position.z }.distanceFrom(s.center));
					if (d < bestDist) { bestDist = d; bestNodeId = snap.id; bestNodePos = snap.position; }
				}
				if (bestNodeId < 0) continue;

				// 既存ノードをローカルネットワークに追加
				RoadNode connNode;
				connNode.id       = bestNodeId;
				connNode.position = bestNodePos;
				connNode.type     = NodeType::Intersection;
				tempRoads.addNodeRaw(connNode);

				// A* pathfinding (World::computeHeight のみ使用)
				constexpr float kMargin = 200.0f;
				const float minX = static_cast<float>(Min(sPos.x, bestNodePos.x)) - kMargin;
				const float minZ = static_cast<float>(Min(sPos.z, bestNodePos.z)) - kMargin;
				const float maxX = static_cast<float>(Max(sPos.x, bestNodePos.x)) + kMargin;
				const float maxZ = static_cast<float>(Max(sPos.z, bestNodePos.z)) + kMargin;
				const int pfW = Max(2, static_cast<int>(
					Ceil((maxX - minX) / RoadPathfinder::kDefaultCellSize)));
				const int pfH = Max(2, static_cast<int>(
					Ceil((maxZ - minZ) / RoadPathfinder::kDefaultCellSize)));

				RoadPathfinder pf;
				pf.setup(world, Vec2{ minX, minZ }, pfW, pfH);

				const Point gs = pf.worldToGrid(static_cast<float>(sPos.x), static_cast<float>(sPos.z));
				const Point ge = pf.worldToGrid(static_cast<float>(bestNodePos.x), static_cast<float>(bestNodePos.z));
				const Array<Point> path = pf.findPath(gs, ge);

				if (path.isEmpty())
				{
					tempRoads.addEdge(*sNodeOpt, bestNodeId,
					                  sPos + (bestNodePos - sPos) * (1.0 / 3.0),
					                  sPos + (bestNodePos - sPos) * (2.0 / 3.0),
					                  RoadType::LocalRoad, 2);
				}
				else
				{
					Array<Vec3> wps = pf.samplePath(path, 5);
					wps.front() = sPos;
					wps.back()  = bestNodePos;
					pf.pathToRoadEdges(wps, tempRoads, RoadType::LocalRoad, 2,
					                   *sNodeOpt, bestNodeId);
				}

				result.connectionNodeId = bestNodeId;
			}
		}

		// ---- ポスト処理 (ローカルネットワーク上) ----
		if (!skipPostProcess)
		{
			while (tempRoads.fixSharpAngles(12.5f));
			tempRoads.smoothAllCurves();
			tempRoads.resolveIntersections();
			tempRoads.spreadIntersectionTangents();
			tempRoads.removeDuplicateEdges(seed);
		}
	}

	result.localNodes  = tempRoads.nodes();
	result.localEdges  = tempRoads.edges();
	result.settlements = gen.m_settlements;

	return result;
}
