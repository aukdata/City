#include "GenerationSettings.hpp"
#include "RoadPathfinder.hpp"
#include <queue>
#include "RoadConstructionCost.hpp"
#include "RoadDesignLimits.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// グリッド構築
// ─────────────────────────────────────────────────────────────────────────────

void RoadPathfinder::setup(const World& world, Vec2 offset, int gridW, int gridH, float cellSize)
{
	// ワールド地形を道路探索専用の高さグリッドへ写し、以後の A* 評価を軽くする。
	m_offset   = offset;
	m_gridW    = gridW;
	m_gridH    = gridH;
	m_cellSize = cellSize;

	m_heightGrid.resize(gridW * gridH);
	m_waterGrid.assign(static_cast<size_t>(gridW*gridH),0.0f);
	m_flowGrid.assign(static_cast<size_t>(gridW*gridH),Vec2{0,0});
	for (int gz = 0; gz < gridH; ++gz)
		for (int gx = 0; gx < gridW; ++gx)
		{
			const Vec2 wp = gridToWorld(gx, gz);
			m_heightGrid[gz * gridW + gx] = world.sampleHeight(
				static_cast<float>(wp.x), static_cast<float>(wp.y));
			m_waterGrid[gz*gridW+gx]=static_cast<float>(world.waterSurfaceHeight(wp.x,wp.y));
			const auto sample=world.rivers().nearest(wp);
			if (sample.reach>=0 && sample.distance<sample.halfWidth+cellSize)
			{
				const auto& reach=world.rivers().reaches[sample.reach]; const Vec2 direction{reach.end.x-reach.start.x,reach.end.z-reach.start.z};
				if (direction.lengthSq()>.01) { m_flowGrid[gz*gridW+gx]=direction.normalized(); }
			}
		}
}

void RoadPathfinder::setupFromHeightMap(
	const Grid<float>& heightMap, Point chunkCoord,
	int gridW, int gridH, float cellSize)
{
	// 既存チャンクの高さマップを直接参照して探索グリッドを作り、地形再サンプリングを避ける。
	m_offset   = Vec2{ static_cast<float>(chunkCoord.x * CHUNK_SIZE),
	                   static_cast<float>(chunkCoord.y * CHUNK_SIZE) };
	m_gridW    = gridW;
	m_gridH    = gridH;
	m_cellSize = cellSize;

	m_heightGrid.resize(gridW * gridH);
	m_waterGrid.assign(static_cast<size_t>(gridW*gridH),0.0f);
	m_flowGrid.assign(static_cast<size_t>(gridW*gridH),Vec2{0,0});
	for (int gz = 0; gz < gridH; ++gz)
		for (int gx = 0; gx < gridW; ++gx)
		{
			const Vec2 wp = gridToWorld(gx, gz);
			m_heightGrid[gz * gridW + gx] = sampleHeightMap(
				heightMap, chunkCoord,
				static_cast<float>(wp.x), static_cast<float>(wp.y));
		}
}

// ─────────────────────────────────────────────────────────────────────────────
// 座標変換
// ─────────────────────────────────────────────────────────────────────────────

Vec2 RoadPathfinder::gridToWorld(int gx, int gz) const
{
	return Vec2{ m_offset.x + (gx + 0.5f) * m_cellSize,
	             m_offset.y + (gz + 0.5f) * m_cellSize };
}

Point RoadPathfinder::worldToGrid(float wx, float wz) const
{
	return Point{
		Clamp(static_cast<int>((wx - m_offset.x) / m_cellSize), 0, m_gridW - 1),
		Clamp(static_cast<int>((wz - m_offset.y) / m_cellSize), 0, m_gridH - 1)
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// A*（8 方向グリッド）
// ─────────────────────────────────────────────────────────────────────────────

Array<Point> RoadPathfinder::findPath(
	Point start, Point goal,
	const Array<Vec2>& forbiddenStartDirs,
	const Array<Vec2>& forbiddenGoalDirs,
	const HashSet<int>& occupiedCells) const
{
	// 地形勾配、既存道路、始終点の鋭角接続をコスト化した 8 近傍 A* で道路芯線を探す。
	const int total = m_gridW * m_gridH;

	struct Cell
	{
		float g      = 1e30f;
		float f      = 1e30f;
		int   parent = -1;
		bool  closed = false;
	};

	Array<Cell> cells(total);
	const int si = start.y * m_gridW + start.x;
	cells[si].g  = 0.0f;
	cells[si].f  = static_cast<float>(start.distanceFrom(goal)) * m_cellSize;

	using PQEntry = std::pair<float, int>;
	std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<>> pq;
	pq.push({ cells[si].f, si });

	constexpr int   kDx[8] = {  1, -1,  0,  0,  1,  1, -1, -1 };
	constexpr int   kDz[8] = {  0,  0,  1, -1,  1, -1,  1, -1 };
	constexpr float kDc[8] = {  1,  1,  1,  1, 1.4142f, 1.4142f, 1.4142f, 1.4142f };

	while (!pq.empty())
	{
		auto [cf, ci] = pq.top();
		pq.pop();

		if (cells[ci].closed) continue;
		cells[ci].closed = true;

		const int cx = ci % m_gridW;
		const int cz = ci / m_gridW;
		if (cx == goal.x && cz == goal.y) break;

		for (int d = 0; d < 8; ++d)
		{
			const int nx = cx + kDx[d];
			const int nz = cz + kDz[d];
			if (nx < 0 || nx >= m_gridW || nz < 0 || nz >= m_gridH) continue;

			const int ni = nz * m_gridW + nx;
			if (cells[ni].closed) continue;

			// 勾配ペナルティ
			const bool wet=height(nx,nz)<m_waterGrid[ni]+1 || height(cx,cz)<m_waterGrid[ci]+1;
			const float dh = wet ? 0.0f : height(nx, nz) - height(cx, cz);
			const float slope = std::abs(dh) / (m_cellSize * kDc[d]);
			const double grade = RoadDesignLimits::forType(m_roadType).maximumGrade * GenerationSettings::get().routing_gradeReserve;
			const double excess = Max(0.0, Abs(dh) - m_cellSize * kDc[d] * grade);
			// Coarse terrain-following estimate; final height/curvature costs are evaluated on the actual curves.
			float gradPenalty = static_cast<float>(RoadConstructionCost::unit(dh < 0 ? excess : -excess, 0));
			if (slope > grade && excess <= RoadConstructionCost::SurfaceTolerance()) { gradPenalty = static_cast<float>(RoadConstructionCost::Earthwork()); }

			if (m_railwayRouting) { gradPenalty=m_constructionCost ? 1+Min(2.0f,Square(slope)*2) : 1+Square(slope/GenerationSettings::get().routing_railCoarseGrade); }

			// Water is expensive; railway crossings may use a viaduct.
			float terrainPenalty = 1.0f;
			if (wet)
			{
				const Vec2 direction=Vec2{kDx[d],kDz[d]}.normalized();
				const double parallel=Max(Abs(direction.dot(m_flowGrid[ni])),Abs(direction.dot(m_flowGrid[ci])));
				if (m_railwayRouting) { terrainPenalty = static_cast<float>(GenerationSettings::get().routing_railWaterCost + GenerationSettings::get().routing_railAlongRiverPenalty * parallel * parallel); }
				else
				{
					const auto crossingCost = [&](int index)
					{
						const double ground = m_heightGrid[index], water = m_waterGrid[index];
						return RoadConstructionCost::unit(Max(ground, water + 6), ground, water);
					};
					terrainPenalty = static_cast<float>((crossingCost(ci) + crossingCost(ni)) * .5);
				}
			}
			{
				const Vec2 wp = gridToWorld(nx, nz);
				if (wp.x < 0.0f || wp.x > WORLD_SIZE || wp.y < 0.0f || wp.y > WORLD_SIZE)
					terrainPenalty = 1e6f;
			}

			float move = m_cellSize * kDc[d] * gradPenalty * terrainPenalty;
			if (m_constructionCost) { move*=static_cast<float>(Max(1.0,m_constructionCost(gridToWorld(nx,nz),height(nx,nz)))); }

			// 重複道路ペナルティ
			if (occupiedCells.count(ni) > 0)
				move *= GenerationSettings::get().routing_occupiedPenalty;

			// 鋭角ペナルティ（cos 45° ≈ 0.707）
			const float kNearDist       = GenerationSettings::get().routing_junctionInfluenceCells;
			const float kSharpCosThresh = GenerationSettings::get().routing_junctionSharpCosine;
			const float kSharpPenalty   = GenerationSettings::get().routing_junctionSharpPenalty;
			const Vec2 moveDirN = Vec2{ static_cast<float>(kDx[d]),
			                           static_cast<float>(kDz[d]) }.normalized();

			if (!forbiddenStartDirs.isEmpty() &&
			    static_cast<float>(Point{ nx, nz }.distanceFrom(start)) <= kNearDist)
			{
				for (const Vec2& fd : forbiddenStartDirs)
					if (moveDirN.dot(fd) > kSharpCosThresh) { move *= kSharpPenalty; break; }
			}

			if (!forbiddenGoalDirs.isEmpty() &&
			    static_cast<float>(Point{ nx, nz }.distanceFrom(goal)) <= kNearDist)
			{
				for (const Vec2& fd : forbiddenGoalDirs)
					if (moveDirN.dot(fd) < -kSharpCosThresh) { move *= kSharpPenalty; break; }
			}

			const float ng = cells[ci].g + move;
			if (ng < cells[ni].g)
			{
				cells[ni].g      = ng;
				cells[ni].f      = ng + static_cast<float>(
				    Point{ nx, nz }.distanceFrom(goal)) * m_cellSize;
				cells[ni].parent = ci;
				pq.push({ cells[ni].f, ni });
			}
		}
	}

	// パス復元
	Array<Point> path;
	int cur = goal.y * m_gridW + goal.x;
	if (!cells[cur].closed) { return {}; }
	while (cur >= 0)
	{
		path << Point{ cur % m_gridW, cur / m_gridW };
		cur = cells[cur].parent;
	}
	path.reverse();
	return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// パス → ウェイポイント
// ─────────────────────────────────────────────────────────────────────────────

Array<Vec3> RoadPathfinder::samplePath(const Array<Point>& path, int stepCells) const
{
	// グリッド経路を間引いたワールド座標列へ変換し、後段の曲線生成に使える密度へ整える。
	if (path.isEmpty()) return {};

	Array<Vec3> wps;
	const Vec2 wp0 = gridToWorld(path.front().x, path.front().y);
	wps << Vec3{ wp0.x, height(path.front().x, path.front().y), wp0.y };

	int last=0;
	for (int i=1;i+1<static_cast<int>(path.size());++i)
	{
		const auto wetAt=[&](int index) { const Point p=path[index];const int cell=p.y*m_gridW+p.x;return m_heightGrid[cell]<m_waterGrid[cell]+1; };
		const Point before=path[i]-path[i-1],after=path[i+1]-path[i];
		if (i-last<stepCells && before==after && wetAt(i)==wetAt(i-1)) { continue; }
		const Vec2 wp=gridToWorld(path[i].x,path[i].y);
		wps<<Vec3{wp.x,height(path[i].x,path[i].y),wp.y};last=i;
	}
	const Vec2 wpN = gridToWorld(path.back().x, path.back().y);
	wps << Vec3{ wpN.x, height(path.back().x, path.back().y), wpN.y };
	return wps;
}

// ─────────────────────────────────────────────────────────────────────────────
// ウェイポイント → ベジェ道路エッジ
// ─────────────────────────────────────────────────────────────────────────────

void RoadPathfinder::pathToRoadEdges(
	const Array<Vec3>& wps, RoadNetwork& roads,
	RoadType rt, int lanes,
	int startNodeId, int endNodeId,
	Array<int>* outEdgeIds)
{
	// ウェイポイント列を中間ノード付きのベジェ道路列へ変換し、道路ネットワークへ流し込む。
	if (wps.size() < 2) return;

	Array<int> nodeIds;
	nodeIds << startNodeId;
	for (int i = 1; i < static_cast<int>(wps.size()) - 1; ++i)
		nodeIds << roads.addNode(wps[i], NodeType::Intersection);
	nodeIds << endNodeId;

	const int n = static_cast<int>(wps.size());
	for (int i = 0; i < n - 1; ++i)
	{
		// Catmull-Rom 接線からベジェ制御点を導出する
		// tangent(i) = (P(i+1) - P(i-1)) / 2  （端点は線分方向を使用）
		const Vec3& p0 = wps[i];
		const Vec3& p1 = wps[i + 1];

		const Vec3 tanA = (i > 0)
			? (p1 - wps[i - 1]) * 0.5
			: (p1 - p0);
		const Vec3 tanB = (i + 2 < n)
			? (wps[i + 2] - p0) * 0.5
			: (p1 - p0);

		const Vec3 ctrlA = p0 + tanA * (1.0 / 3.0);
		const Vec3 ctrlB = p1 - tanB * (1.0 / 3.0);
		if (auto eid = roads.addEdge(nodeIds[i], nodeIds[i + 1], ctrlA, ctrlB, rt, lanes))
		{
			if (outEdgeIds) *outEdgeIds << *eid;
		}
	}
}
