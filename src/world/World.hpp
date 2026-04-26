#pragma once
#include "Chunk.hpp"
#include "../gen/TerrainType.hpp"

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

	/// @brief cont ノイズにマップ端距離補正を加える（海をマップ端に誘導）
	float adjustContinentalness(float rawCont, float wx, float wz) const;

	void rebuildActiveChunkCache();

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
