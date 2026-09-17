
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

/// @brief ワールドの一辺の長さ [m]
constexpr float WORLD_SIZE = static_cast<float>(WORLD_CHUNKS) * CHUNK_SIZE;

/// @brief buildHeightMap の結果（heightMap + min/max）
struct HeightMapResult
{
	// 高さグリッド本体と範囲情報をセットで返し、後段の描画・配置が同じ結果を使い回せるようにする。
	Grid<float> heightMap;
	float       heightMin;
	float       heightMax;
};

/// @brief heightMap からワールド座標の高さを描画三角形と一致する区分線形補間で取得する
/// @param heightMap  (HEIGHT_CELLS+1)x(HEIGHT_CELLS+1) のグリッド
/// @param chunkCoord チャンク座標
inline float sampleHeightMap(const Grid<float>& heightMap, Point chunkCoord,
                             float wx, float wz)
{
	constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
	const float lx = wx - static_cast<float>(chunkCoord.x * CHUNK_SIZE);
	const float lz = wz - static_cast<float>(chunkCoord.y * CHUNK_SIZE);
	const float fx = Clamp(lx / cellSize, 0.0f, static_cast<float>(HEIGHT_CELLS));
	const float fz = Clamp(lz / cellSize, 0.0f, static_cast<float>(HEIGHT_CELLS));
	const int ix = Clamp(static_cast<int>(fx), 0, HEIGHT_CELLS - 1);
	const int iz = Clamp(static_cast<int>(fz), 0, HEIGHT_CELLS - 1);
	const float tx = fx - ix;
	const float tz = fz - iz;
	const int ix1 = Min(ix + 1, HEIGHT_CELLS);
	const int iz1 = Min(iz + 1, HEIGHT_CELLS);
	const double h00 = heightMap[{ ix,  iz  }];
	const double h10 = heightMap[{ ix1, iz  }];
	const double h01 = heightMap[{ ix,  iz1 }];
	const double h11 = heightMap[{ ix1, iz1 }];
	// WorldRenderer uses the diagonal (1,0)-(0,1). Round once after interpolating
	// in double precision so high terrain does not accumulate float addition errors.
	if (tx + tz <= 1.0f)
	{
		return static_cast<float>(h00 + (h10 - h00) * tx + (h01 - h00) * tz);
	}
	return static_cast<float>(h11 + (h01 - h11) * (1.0 - tx) + (h10 - h11) * (1.0 - tz));
}

/// @brief チャンク座標をハッシュキー (int64) に変換する
inline int64 chunkCoordToKey(Point p)
{
	return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
}

/// @brief 地表上に重ねる任意多角形の土地表現
enum class LandPatchType : uint8
{
	ParcelAsphalt = 0,
	ParcelGravel = 1,
	GardenSoil = 2,
	FarmField = 3,
	PaddyField = 4,
	Beach = 5,
	Seawall = 6,
	FarmTrack = 7,       ///< 耕作道。一般車の経路とは別の地表アクセス網。
	IrrigationDitch = 8, ///< ほ場に沿う用排水路。
};

/// @brief 道路・海岸・農地境界から生成される土地ポリゴン
struct LandPatch
{
	int id = -1;
	LandPatchType type = LandPatchType::FarmField;
	Array<Vec2> polygon;
	float elevationOffset = 0.02f;
	uint32 materialVariant = 0;
	int64 sourceParcelKey = -1;
};
/// @brief チャンクデータ
struct Chunk
{
	// 1 チャンクは地形・ゾーン・建物・描画状態をまとめて持つワールド最小管理単位。
	Point             coord;                         ///< チャンク座標
	Grid<float>       heightMap;                     ///< (HEIGHT_CELLS+1)×(HEIGHT_CELLS+1) の高さ [m]
	Grid<uint8>       terrainType;                   ///< HEIGHT_CELLS×HEIGHT_CELLS の地表種別
	Grid<ZoneType>    zoneMap;                       ///< ZONE_CELLS×ZONE_CELLS のゾーン
	Grid<Building>    buildingGrid;                  ///< ZONE_CELLS×ZONE_CELLS の建物（type==None で空地）
	Array<LandPatch>  landPatches;                   ///< 任意多角形の土地利用レイヤ
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

	/// @brief ワールド座標から高さを描画三角形と一致する区分線形補間で取得する
	float getHeight(float wx, float wz) const
	{
		return sampleHeightMap(heightMap, coord, wx, wz);
	}
};
