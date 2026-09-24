#pragma once
#include "Chunk.hpp"
#include "../gen/TerrainType.hpp"
#include "../gen/RiverNetwork.hpp"

/// @brief チャンク管理クラス
/// @details 64×64 固定サイズワールド。カメラ周辺 5×5 チャンクをアクティブに保つ。
class World
{
public:
	// World は固定サイズチャンク集合と地形生成パラメータを持ち、参照と活性管理を仲介する。
	/// @brief カメラ位置を渡してアクティブチャンクを更新する
	void update(Vec3 cameraWorldPos);

	/// @brief 地形生成パラメータをセットする（generate() 前に呼ぶこと）
	void setGenerationParams(uint64 seed, float mapWidth, float mapDepth);

	/// @brief 64×64 チャンク分の配列を事前確保する
	void reserveChunks();
	/// @brief 基本地形から流域を生成する。高さマップ生成前、または保存地形の復帰前に呼ぶ。
	void generateRivers();
	const RiverNetwork& rivers() const { return m_rivers; }
	/// @brief Area counts and moved thickness from the terrain evolution pass.
	struct TerrainEvolutionStats { int uplifted=0, eroded=0, deposited=0; double upliftMetres=0, erosionMetres=0, depositionMetres=0; };
	const TerrainEvolutionStats& terrainEvolution() const { return m_terrainEvolution; }
	double waterSurfaceHeight(double x,double z) const { return m_rivers.waterLevel({x,z}); }
	/// @brief 共有格子点を隣接チャンクにも反映する（地形整形用）。
	void setGridHeight(int x,int z,float height);


	/// @brief 事前計算済みの HeightMapResult をチャンクにインストールする
	void installChunkDirect(Point coord, HeightMapResult&& hmr);

	/// @brief チャンクを取得する（範囲外なら nullptr）
	const Chunk* getChunk(Point coord) const;
	Chunk*       getChunk(Point coord);

	/// @brief アクティブなチャンクの一覧を返す（キャッシュ済み参照）
	const Array<Chunk*>&       getActiveChunks();
	const Array<const Chunk*>& getActiveChunks() const;

	/// @brief ワールド座標から地形高さをサンプリングする（チャンク未生成時は 0 を返す）
	float sampleHeight(float wx, float wz) const;

	/// @brief チャンクを生成せずに直接 Perlin ノイズで高さを計算する（スレッド安全: const）
	float computeHeight(float wx, float wz) const;

	/// @brief ワールド座標のバイオーム種別を返す（スレッド安全: const）
	BiomeType getBiome(float wx, float wz) const;

	/// @brief チャンク座標に対応する heightMap を生成して返す（スレッド安全: const）
	HeightMapResult buildHeightMap(Point chunkCoord) const;

private:
	static Point worldToChunkCoord(Vec3 worldPos)
	{
		const int cx = static_cast<int>(Math::Floor(worldPos.x / CHUNK_SIZE));
		const int cy = static_cast<int>(Math::Floor(worldPos.z / CHUNK_SIZE));
		return { cx, cy };
	}

	static bool isValidCoord(Point coord)
	{
		return coord.x >= 0 && coord.x < WORLD_CHUNKS
		    && coord.y >= 0 && coord.y < WORLD_CHUNKS;
	}

	static int coordToIndex(Point coord)
	{
		return coord.y * WORLD_CHUNKS + coord.x;
	}

	/// @brief バイオームパラメータ（基底高・振幅）を連続補間で計算する
	void computeBiomeParams(float wx, float wz, float& outBase, float& outAmp) const;
	void computeRawBiomeParams(float wx, float wz, float& outBase, float& outAmp) const;
	/// @brief 広域地形を一度だけ計算し、平野面積の上限と内部の丘陵を反映する。
	void buildMacroTerrain();
	/// @brief Uplifted relief is reshaped by drainage, sediment transport and alluvial deposition.
	void evolveMacroTerrain();
	Grid<Float2> m_macroTerrain;
	Grid<uint8> m_macroOcean; ///< Boundary-connected water, distinct from inland lakes.
	double m_macroStepX = 64, m_macroStepZ = 64;

	/// @brief 中央は平野を基本とし、一部の種では山・湾・湖を持つ。
	enum class CentralLandform : uint8 { Plain, Mountain, Bay, Lake };
	CentralLandform m_centralLandform=CentralLandform::Plain;
	double lakeInfluence(double inlandAxis, double alongAxis, double scale) const;
	Vec2 m_featureCenter{.06,0}; ///< 海岸座標系での正規化位置。
	double m_rangeOffset=.4, m_rangeWidth=.18, m_flankSign=1;

	/// @brief 種ごとの海岸法線、湾位置、山並みの位相。全チャンクで同じ座標系を使う。
	Vec2 m_landAxis{1, 0};
	double m_bayCenter = 0;
	double m_terrainPhase = 0;

	void rebuildActiveChunkCache();

	RiverNetwork m_rivers;
	TerrainEvolutionStats m_terrainEvolution;
	float computeBaseHeight(float wx,float wz) const;
	Array<Chunk>          m_chunks;
	Point                 m_cameraChunk{ 0x7FFFFFFF, 0x7FFFFFFF };
	Array<Chunk*>         m_activeChunks;
	Array<const Chunk*>   m_activeChunksConst;

	static constexpr int ACTIVE_RANGE = 4;

	// ----- 地形生成パラメータ -----
	uint64      m_seed     = 20260316ULL;
	float       m_mapWidth = 4096.0f;
	float       m_mapDepth = 4096.0f;
	PerlinNoise m_perlin   = PerlinNoise{ m_seed };
};
