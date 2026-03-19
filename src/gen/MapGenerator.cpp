#include "MapGenerator.hpp"
#include <random>
#include <queue>

// ─────────────────────────────────────────────────────────────────────────────
// generate() : トップレベル
// ─────────────────────────────────────────────────────────────────────────────

MapGenerator::Result MapGenerator::generate(
	uint64 seed, TerrainType terrainType,
	World& world, RoadNetwork& roads,
	ZoneManager& zones, TrainNetwork& trainNet)
{
	// 生成パラメータをセットして全チャンクを事前生成する
	world.setGenerationParams(seed, terrainType, kMapWidth, kMapDepth);
	for (int cz = 0; cz < kMapChunksZ; ++cz)
		for (int cx = 0; cx < kMapChunksX; ++cx)
			world.getOrCreateChunk({ cx, cz });

	// Phase 1: 高さグリッドを構築（A* のために全セルをキャッシュ）
	buildHeightGrid(world);

	// Phase 2: 集落配置
	placeSettlements(seed);

	// Phase 3: 旧道生成
	generateRoads(roads, seed);

	// Phase 6: 初期ゾーン
	assignZones(world, zones);

	// 鉄道
	setupTrain(trainNet, world);

	// カメラ注視点 = 都市核の位置
	Result result;
	if (!m_settlements.isEmpty())
	{
		const auto& urban = m_settlements[0];
		const float y = world.sampleHeight(urban.center.x, urban.center.y);
		result.cameraFocus = Vec3{ urban.center.x, y, urban.center.y };
	}
	else
	{
		result.cameraFocus = Vec3{ kMapWidth * 0.5, 0.0, kMapDepth * 0.5 };
	}
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1: 高さグリッド構築
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::buildHeightGrid(World& world)
{
	m_heightGrid.resize(kGridW * kGridH);
	for (int gz = 0; gz < kGridH; ++gz)
	{
		for (int gx = 0; gx < kGridW; ++gx)
		{
			const Vec2 wp = gridToWorld(gx, gz);
			m_heightGrid[gridIdx(gx, gz)] = world.sampleHeight(
				static_cast<float>(wp.x), static_cast<float>(wp.y));
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2: 集落配置（Poisson ディスクサンプリング）
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

void MapGenerator::placeSettlements(uint64 seed)
{
	std::mt19937_64 rng(seed ^ 0xABCD1234ULL);

	// 候補セルを収集（端から5セル内側）
	Array<Point> candidates;
	for (int gz = 5; gz < kGridH - 5; ++gz)
		for (int gx = 5; gx < kGridW - 5; ++gx)
			if (isSuitable(gx, gz))
				candidates << Point{ gx, gz };

	// シャッフル
	std::shuffle(candidates.begin(), candidates.end(), rng);

	// Poisson ディスクサンプリング（最小距離 600 m）
	constexpr float kMinDist = 600.0f;
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
		if (m_settlements.size() >= 7) break;
	}

	// 集落が 3 未満のときは条件を緩和して再試行（高さ上限を広げる）
	if (m_settlements.size() < 3)
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
			if (m_settlements.size() >= 7) break;
		}
	}

	if (m_settlements.isEmpty())
	{
		// フォールバック: マップ中央付近を都市核にする
		Settlement s;
		s.center = Vec2{ kMapWidth * 0.5f, kMapDepth * 0.5f };
		m_settlements << s;
	}

	// マップ中心に最も近い集落を都市核とする
	const Vec2 mapCenter{ kMapWidth * 0.5f, kMapDepth * 0.5f };
	m_settlements.sort_by([&](const Settlement& a, const Settlement& b)
	{
		return a.center.distanceFromSq(mapCenter) < b.center.distanceFromSq(mapCenter);
	});

	// 種別・影響半径を割当てる
	for (int i = 0; i < static_cast<int>(m_settlements.size()); ++i)
	{
		if (i == 0)
		{
			m_settlements[i].type   = SettlementType::Urban;
			m_settlements[i].radius = 500.0f;
		}
		else if (i <= 3)
		{
			m_settlements[i].type   = SettlementType::District;
			m_settlements[i].radius = 200.0f;
		}
		else
		{
			m_settlements[i].type   = SettlementType::Rural;
			m_settlements[i].radius = 100.0f;
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
			const float d = m_settlements[i].center.distanceFrom(m_settlements[j].center);
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

// --- A*（16m グリッド・8 方向）---

Array<Point> MapGenerator::findPath(Point start, Point goal) const
{
	// 平坦配列でノード管理
	struct Cell
	{
		float g      = 1e30f;
		float f      = 1e30f;
		int   parent = -1;
		bool  closed = false;
	};

	Array<Cell> cells(kGridW * kGridH);
	const int si = gridIdx(start.x, start.y);
	cells[si].g = 0.0f;
	cells[si].f = start.distanceFrom(goal) * kCellSize;

	// min-heap: (f, flat_idx)
	using PQEntry = std::pair<float, int>;
	std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<>> pq;
	pq.push({ cells[si].f, si });

	constexpr int kDx[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
	constexpr int kDz[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
	constexpr float kDc[8] = { 1,1,1,1,1.4142f,1.4142f,1.4142f,1.4142f };

	while (!pq.empty())
	{
		auto [cf, ci] = pq.top();
		pq.pop();

		if (cells[ci].closed) continue;
		cells[ci].closed = true;

		const int cx = ci % kGridW;
		const int cz = ci / kGridW;
		if (cx == goal.x && cz == goal.y) break;

		for (int d = 0; d < 8; ++d)
		{
			const int nx = cx + kDx[d];
			const int nz = cz + kDz[d];
			if (nx < 0 || nx >= kGridW || nz < 0 || nz >= kGridH) continue;

			const int ni = gridIdx(nx, nz);
			if (cells[ni].closed) continue;

			// 勾配ペナルティ（仕様書 §5 Step 2）
			const float dh    = gridHeight(nx, nz) - gridHeight(cx, cz);
			const float slope = std::abs(dh) / (kCellSize * kDc[d]);
			float gradPenalty;
			if      (slope < 0.05f) gradPenalty = 1.0f;
			else if (slope < 0.15f) gradPenalty = 2.0f;
			else if (slope < 0.30f) gradPenalty = 5.0f;
			else                    gradPenalty = 20.0f;

			// 地形ペナルティ（水域回避）
			const float terrainPenalty = (gridHeight(nx, nz) < 0.0f) ? 10.0f : 1.0f;

			const float move = kCellSize * kDc[d] * gradPenalty * terrainPenalty;
			const float ng   = cells[ci].g + move;

			if (ng < cells[ni].g)
			{
				cells[ni].g      = ng;
				cells[ni].f      = ng + Point{ nx, nz }.distanceFrom(goal) * kCellSize;
				cells[ni].parent = ci;
				pq.push({ cells[ni].f, ni });
			}
		}
	}

	// パス復元
	Array<Point> path;
	int cur = gridIdx(goal.x, goal.y);
	while (cur >= 0)
	{
		path << Point{ cur % kGridW, cur / kGridW };
		cur = cells[cur].parent;
	}
	path.reverse();
	return path;
}

// --- パスをウェイポイントにサンプリング ---

Array<Vec3> MapGenerator::samplePath(const Array<Point>& path, int stepCells) const
{
	if (path.isEmpty()) return {};

	Array<Vec3> wps;
	// 始点
	const Vec2 wp0 = gridToWorld(path.front().x, path.front().y);
	wps << Vec3{ wp0.x, m_heightGrid[gridIdx(path.front().x, path.front().y)], wp0.y };

	for (int i = stepCells; i < static_cast<int>(path.size()) - 1; i += stepCells)
	{
		const Vec2 wp = gridToWorld(path[i].x, path[i].y);
		wps << Vec3{ wp.x, m_heightGrid[gridIdx(path[i].x, path[i].y)], wp.y };
	}

	// 終点
	const Vec2 wpN = gridToWorld(path.back().x, path.back().y);
	wps << Vec3{ wpN.x, m_heightGrid[gridIdx(path.back().x, path.back().y)], wpN.y };

	return wps;
}

// --- ウェイポイント列 → ベジェ道路エッジ ---

void MapGenerator::pathToRoadEdges(
	const Array<Vec3>& wps,
	RoadNetwork& roads,
	RoadType rt, int lanes,
	int startNodeId, int endNodeId)
{
	if (wps.size() < 2) return;

	// 中間ウェイポイントにノードを追加する（始終点は既存 ID を使う）
	Array<int> nodeIds;
	nodeIds << startNodeId;

	for (int i = 1; i < static_cast<int>(wps.size()) - 1; ++i)
	{
		const int id = roads.addNode(wps[i], NodeType::Intersection);
		nodeIds << id;
	}
	nodeIds << endNodeId;

	// 連続セグメントをベジェエッジとして登録する
	for (int i = 0; i < static_cast<int>(nodeIds.size()) - 1; ++i)
	{
		const Vec3& a = wps[i];
		const Vec3& b = wps[i + 1];
		const Vec3 ctrlA = a + (b - a) * (1.0 / 3.0);
		const Vec3 ctrlB = a + (b - a) * (2.0 / 3.0);
		roads.addEdge(nodeIds[i], nodeIds[i + 1], ctrlA, ctrlB, rt, lanes);
	}
}

// --- 旧道生成 メイン ---

void MapGenerator::generateRoads(RoadNetwork& roads, uint64 seed)
{
	if (m_settlements.isEmpty()) return;

	// 各集落の道路ノード ID を記録する
	const int n = static_cast<int>(m_settlements.size());
	Array<int> settleNodeId(n, -1);

	for (int i = 0; i < n; ++i)
	{
		const Vec2& c  = m_settlements[i].center;
		const float y  = m_heightGrid[gridIdx(
			Clamp(static_cast<int>(c.x / kCellSize), 0, kGridW - 1),
			Clamp(static_cast<int>(c.y / kCellSize), 0, kGridH - 1))];
		settleNodeId[i] = roads.addNode(Vec3{ c.x, y, c.y }, NodeType::Intersection);
	}

	// MST で幹線網を決定する
	const auto mstEdges = computeMST();

	for (const auto& [u, v] : mstEdges)
	{
		const Point gs = worldToGrid(
			static_cast<float>(m_settlements[u].center.x),
			static_cast<float>(m_settlements[u].center.y));
		const Point ge = worldToGrid(
			static_cast<float>(m_settlements[v].center.x),
			static_cast<float>(m_settlements[v].center.y));

		const Array<Point> path = findPath(gs, ge);
		if (path.isEmpty()) continue;

		// 幹線種別: Urban ↔ District/Urban → 幹線道路、それ以外 → 一般道
		const bool isArterial =
			m_settlements[u].type != SettlementType::Rural &&
			m_settlements[v].type != SettlementType::Rural;
		const RoadType rt    = isArterial ? RoadType::Arterial : RoadType::LocalRoad;
		const int      lanes = isArterial ? 4 : 2;

		const Array<Vec3> wps = samplePath(path, 12); // ~192 m 間隔でサンプリング
		pathToRoadEdges(wps, roads, rt, lanes, settleNodeId[u], settleNodeId[v]);
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

		const Array<Point> path = findPath(gs, ge);
		if (path.isEmpty()) continue;

		const Array<Vec3> wps = samplePath(path, 12);
		pathToRoadEdges(wps, roads, RoadType::LocalRoad, 2,
		                settleNodeId[u], settleNodeId[v]);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 6: 初期ゾーン自動設定
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::assignZones(World& world, ZoneManager& zones)
{
	for (const auto& s : m_settlements)
	{
		// 300 m 以内 → Commercial / Residential
		// 300〜600 m → LowResidential
		const int innerR  = static_cast<int>(300.0f / kCellSize); // 19 cells
		const int outerR  = static_cast<int>(600.0f / kCellSize); // 38 cells
		const float innerSq = (300.0f * 300.0f);
		const float outerSq = (600.0f * 600.0f);

		const int gx0 = worldToGrid(static_cast<float>(s.center.x) - 600.0f, 0.0f).x;
		const int gx1 = worldToGrid(static_cast<float>(s.center.x) + 600.0f, 0.0f).x;
		const int gz0 = worldToGrid(0.0f, static_cast<float>(s.center.y) - 600.0f).y;
		const int gz1 = worldToGrid(0.0f, static_cast<float>(s.center.y) + 600.0f).y;

		for (int gz = gz0; gz <= gz1; ++gz)
		{
			for (int gx = gx0; gx <= gx1; ++gx)
			{
				if (gx < 0 || gx >= kGridW || gz < 0 || gz >= kGridH) continue;

				const Vec2  wp   = gridToWorld(gx, gz);
				const float dSq  = s.center.distanceFromSq(wp);
				const float h    = gridHeight(gx, gz);

				// 水域セルはゾーン付与しない
				if (h < 0.0f) continue;

				const Vec3 wp3{ wp.x, h, wp.y };

				if (dSq <= innerSq)
				{
					const ZoneType zt = (s.type == SettlementType::Urban)
						? ZoneType::Commercial
						: ZoneType::Residential;
					zones.paintZone(world, wp3, zt, 0);
				}
				else if (dSq <= outerSq)
				{
					zones.paintZone(world, wp3, ZoneType::LowResidential, 0);
				}
			}
		}
	}

	// 農地：低地（h < 2 m）かつゾーン未設定のセルを Agricultural に
	for (int gz = 0; gz < kGridH; ++gz)
	{
		for (int gx = 0; gx < kGridW; ++gx)
		{
			const float h = gridHeight(gx, gz);
			if (h < 0.0f || h >= 2.0f) continue;

			const Vec2 wp = gridToWorld(gx, gz);
			const Vec3 wp3{ wp.x, h, wp.y };

			const ZoneType existing = zones.getZone(world, wp3);
			if (existing == ZoneType::Unzoned)
				zones.paintZone(world, wp3, ZoneType::Agriculture, 0);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 鉄道初期設定
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::setupTrain(TrainNetwork& trainNet, World& world)
{
	// Urban 集落と District 集落（最大 4 駅）を結ぶ路線を 1 本生成する
	Array<int> stationIds;

	for (const auto& s : m_settlements)
	{
		if (s.type == SettlementType::Rural) continue;
		if (stationIds.size() >= 4) break;

		const float y  = world.sampleHeight(
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
