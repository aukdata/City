
#pragma once
#include "../zone/Building.hpp"

/// @brief チャンクのロード状態
enum class ChunkState : uint8
{
	Active,
	Sleeping,
};

/// @brief ワールドの一辺のチャンク数
constexpr int WORLD_CHUNKS = 64;

/// @brief チャンクの一辺の長さ [m]
constexpr int CHUNK_SIZE = 1024;

/// @brief ハイトマップのグリッド分割数（セルサイズ = CHUNK_SIZE / HEIGHT_CELLS = 16m）
constexpr int HEIGHT_CELLS = 64;

/// @brief ゾーンマップのグリッド分割数（セルサイズ = 16m）
constexpr int ZONE_CELLS = 64;

/// @brief buildHeightMap の結果（heightMap + min/max）
struct HeightMapResult
{
	Grid<float> heightMap;
	float       heightMin;
	float       heightMax;
};

/// @brief heightMap からワールド座標の高さをバイリニア補間で取得する
/// @param heightMap  (HEIGHT_CELLS+1)x(HEIGHT_CELLS+1) のグリッド
/// @param chunkCoord チャンク座標
inline float sampleHeightMap(const Grid<float>& heightMap, Point chunkCoord,
                             float wx, float wz)
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	const float lx = wx - static_cast<float>(chunkCoord.x * CHUNK_SIZE);
	const float lz = wz - static_cast<float>(chunkCoord.y * CHUNK_SIZE);
	const float fx = lx / cellSize;
	const float fz = lz / cellSize;
	const int ix = Clamp(static_cast<int>(fx), 0, HEIGHT_CELLS - 1);
	const int iz = Clamp(static_cast<int>(fz), 0, HEIGHT_CELLS - 1);
	const float tx = fx - ix;
	const float tz = fz - iz;
	const int ix1 = Min(ix + 1, HEIGHT_CELLS);
	const int iz1 = Min(iz + 1, HEIGHT_CELLS);
	const float h00 = heightMap[{ ix,  iz  }];
	const float h10 = heightMap[{ ix1, iz  }];
	const float h01 = heightMap[{ ix,  iz1 }];
	const float h11 = heightMap[{ ix1, iz1 }];
	return h00 * (1.0f - tx) * (1.0f - tz)
	     + h10 * tx           * (1.0f - tz)
	     + h01 * (1.0f - tx) * tz
	     + h11 * tx           * tz;
}

/// @brief チャンク座標をハッシュキー (int64) に変換する
inline int64 chunkCoordToKey(Point p)
{
	return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
}

/// @brief チャンクデータ
struct Chunk
{
	Point             coord;                         ///< チャンク座標
	Grid<float>       heightMap;                     ///< (HEIGHT_CELLS+1)×(HEIGHT_CELLS+1) の高さ [m]
	Grid<uint8>       terrainType;                   ///< HEIGHT_CELLS×HEIGHT_CELLS の地表種別
	Grid<ZoneType>    zoneMap;                       ///< ZONE_CELLS×ZONE_CELLS のゾーン
	Grid<Building>    buildingGrid;                  ///< ZONE_CELLS×ZONE_CELLS の建物（type==None で空地）
	bool              isUrbanizationArea = false;    ///< 市街化区域か
	ChunkState        state = ChunkState::Sleeping;
	bool              meshDirty = false;              ///< メッシュ再生成が必要か
	float             heightMin = 0.0f;              ///< heightMap の最小高さ
	float             heightMax = 0.0f;              ///< heightMap の最大高さ

	/// @brief heightMap から heightMin/heightMax を再計算する
	void updateHeightBounds()
	{
		if (heightMap.isEmpty()) return;
		float lo =  1e30f;
		float hi = -1e30f;
		for (int r = 0; r <= HEIGHT_CELLS; ++r)
			for (int c = 0; c <= HEIGHT_CELLS; ++c)
			{
				const float h = heightMap[{ c, r }];
				if (h < lo) lo = h;
				if (h > hi) hi = h;
			}
		heightMin = lo;
		heightMax = hi;
	}

	Chunk() = default;

	explicit Chunk(Point c)
		: coord(c)
		, heightMap(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 0.0f)
		, terrainType(HEIGHT_CELLS, HEIGHT_CELLS, 0)
		, zoneMap(ZONE_CELLS, ZONE_CELLS, ZoneType::Unzoned)
		, buildingGrid(ZONE_CELLS, ZONE_CELLS, Building{})
	{
	}

	/// @brief チャンク原点のワールド座標を返す
	Vec3 worldOrigin() const
	{
		return Vec3(
			static_cast<double>(coord.x) * CHUNK_SIZE,
			0.0,
			static_cast<double>(coord.y) * CHUNK_SIZE);
	}

	/// @brief ワールド座標から高さをバイリニア補間で取得する
	float getHeight(float wx, float wz) const
	{
		return sampleHeightMap(heightMap, coord, wx, wz);
	}
};
