#pragma once
#include "GenerationSettings.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief グリッドベース地形 A* パスファインダー
/// MapGenerator（地区間接続）と GameScene（チャンク間接続）で共用する
class RoadPathfinder
{
public:
	void setRailwayRouting(bool enabled) { m_railwayRouting=enabled; }
	void setRoadType(RoadType type) { m_roadType = type; }
	/// @brief 地上回廊の候補探索では許容勾配を超える斜面の短絡を除外する。
	void setSurfaceOnly(bool enabled) { m_surfaceOnly = enabled; }
	void setConstructionCost(std::function<double(Vec2,double)> cost) { m_constructionCost=std::move(cost); }
	static float defaultCellSize() { return GenerationSettings::get().network_localRoutingCell; }

	/// @brief 高さグリッドを構築する
	/// @param world     高さ計算に使用するワールド
	/// @param offset    グリッド左下のワールド XZ 座標 [m]
	/// @param gridW     グリッド横セル数
	/// @param gridH     グリッド縦セル数
	/// @param cellSize  1 セルのサイズ [m]
	void setup(const World& world, Vec2 offset, int gridW, int gridH,
	           float cellSize = defaultCellSize());

	/// @brief 事前計算済み heightMap からパスファインダーグリッドを構築する（computeHeight 不要）
	void setupFromHeightMap(const Grid<float>& heightMap, Point chunkCoord,
	                        int gridW, int gridH,
	                        float cellSize = defaultCellSize());

	// ----- アクセサ -----
	int   gridW()    const { return m_gridW; }
	int   gridH()    const { return m_gridH; }
	float cellSize() const { return m_cellSize; }
	Vec2  offset()   const { return m_offset; }

	/// @brief 高さグリッド全体を返す（バックグラウンド生成結果のキャッシュ用）
	const Array<float>& heightGrid() const { return m_heightGrid; }

	/// @brief セルの高さを返す [m]
	float height(int gx, int gz) const { return m_heightGrid[gz * m_gridW + gx]; }

	/// @brief グリッド座標 → ワールド XZ 中心
	Vec2  gridToWorld(int gx, int gz) const;

	/// @brief ワールド XZ → グリッド座標（クランプ済み）
	Point worldToGrid(float wx, float wz) const;

	/// @brief A* で 2 点間の最短グリッドパスを求める
	/// @param forbiddenStartDirs  始点近傍で鋭角になる進行方向（outward 単位ベクトル）
	/// @param forbiddenGoalDirs   終点近傍で鋭角になる進行方向（outward 単位ベクトル）
	/// @param occupiedCells       既存パスが占有するグリッドセルの flat-index 集合
	Array<Point> findPath(Point start, Point goal,
	                      const Array<Vec2>& forbiddenStartDirs = {},
	                      const Array<Vec2>& forbiddenGoalDirs  = {},
	                      const HashSet<int>& occupiedCells     = {}) const;

	/// @brief グリッドパスをワールド座標ウェイポイント列に変換する
	Array<Vec3> samplePath(const Array<Point>& path, int stepCells = 5) const;

	/// @brief ウェイポイント列をベジェ道路エッジとして RoadNetwork に追加する
	/// @param outEdgeIds (optional) 生成されたエッジ ID を起点→終点の順で追記する
	void pathToRoadEdges(const Array<Vec3>& wps, RoadNetwork& roads,
	                     RoadType rt, int lanes,
	                     int startNodeId, int endNodeId,
	                     Array<int>* outEdgeIds = nullptr);

private:
	std::function<double(Vec2,double)> m_constructionCost;
	bool m_railwayRouting=false;
	bool m_surfaceOnly=false;
	RoadType m_roadType = RoadType::LocalRoad;
	int          m_gridW    = 0;
	int          m_gridH    = 0;
	float        m_cellSize = defaultCellSize();
	Vec2         m_offset;
	Array<float> m_heightGrid;
	Array<float> m_waterGrid;
	Array<Vec2> m_flowGrid;
};
