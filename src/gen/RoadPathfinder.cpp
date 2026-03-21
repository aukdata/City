#include "RoadPathfinder.hpp"
#include <queue>

// ─────────────────────────────────────────────────────────────────────────────
// グリッド構築
// ─────────────────────────────────────────────────────────────────────────────

void RoadPathfinder::setup(World& world, Vec2 offset, int gridW, int gridH, float cellSize)
{
	m_offset   = offset;
	m_gridW    = gridW;
	m_gridH    = gridH;
	m_cellSize = cellSize;

	m_heightGrid.resize(gridW * gridH);
	for (int gz = 0; gz < gridH; ++gz)
		for (int gx = 0; gx < gridW; ++gx)
		{
			const Vec2 wp = gridToWorld(gx, gz);
			m_heightGrid[gz * gridW + gx] = world.computeHeight(
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
			const float dh    = height(nx, nz) - height(cx, cz);
			const float slope = std::abs(dh) / (m_cellSize * kDc[d]);
			float gradPenalty;
			if      (slope < 0.05f) gradPenalty = 1.0f;
			else if (slope < 0.15f) gradPenalty = 2.0f;
			else if (slope < 0.30f) gradPenalty = 5.0f;
			else                    gradPenalty = 20.0f;

			// 水域回避
			const float terrainPenalty = (height(nx, nz) < 0.0f) ? 10.0f : 1.0f;

			float move = m_cellSize * kDc[d] * gradPenalty * terrainPenalty;

			// 重複道路ペナルティ
			if (occupiedCells.count(ni) > 0)
				move *= 2.5f;

			// 鋭角ペナルティ（cos 12.5° ≈ 0.976）
			constexpr float kNearDist       = 6.0f;
			constexpr float kSharpCosThresh = 0.976f;
			constexpr float kSharpPenalty   = 50.0f;
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
	if (path.isEmpty()) return {};

	Array<Vec3> wps;
	const Vec2 wp0 = gridToWorld(path.front().x, path.front().y);
	wps << Vec3{ wp0.x, height(path.front().x, path.front().y), wp0.y };

	for (int i = stepCells; i < static_cast<int>(path.size()) - 1; i += stepCells)
	{
		const Vec2 wp = gridToWorld(path[i].x, path[i].y);
		wps << Vec3{ wp.x, height(path[i].x, path[i].y), wp.y };
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
	int startNodeId, int endNodeId)
{
	if (wps.size() < 2) return;

	Array<int> nodeIds;
	nodeIds << startNodeId;
	for (int i = 1; i < static_cast<int>(wps.size()) - 1; ++i)
		nodeIds << roads.addNode(wps[i], NodeType::Intersection);
	nodeIds << endNodeId;

	for (int i = 0; i < static_cast<int>(nodeIds.size()) - 1; ++i)
	{
		const Vec3& a    = wps[i];
		const Vec3& b    = wps[i + 1];
		const Vec3 ctrlA = a + (b - a) * (1.0 / 3.0);
		const Vec3 ctrlB = a + (b - a) * (2.0 / 3.0);
		roads.addEdge(nodeIds[i], nodeIds[i + 1], ctrlA, ctrlB, rt, lanes);
	}
}
