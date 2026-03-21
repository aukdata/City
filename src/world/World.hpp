#pragma once
#include "Chunk.hpp"
#include "../gen/TerrainType.hpp"

/// @brief チャンク管理クラス
/// @details カメラ周辺 5×5 チャンクをアクティブに保つ
class World
{
public:
	/// @brief カメラ位置を渡してアクティブチャンクを更新する
	void update(Vec3 cameraWorldPos);

	/// @brief 地形生成パラメータをセットする（generate() 前に呼ぶこと）
	void setGenerationParams(uint64 seed, TerrainType terrainType, float mapWidth, float mapDepth);

	/// @brief チャンクを取得する（なければ生成して返す）
	Chunk& getOrCreateChunk(Point coord);

	/// @brief 事前計算済みの heightMap を持つチャンクをインストールする
	/// @details バックグラウンドスレッドで計算した heightMap を受け取り、チャンクが未生成の場合のみ登録する。
	void installChunk(Point coord, Grid<float>&& heightMap);

	/// @brief チャンクを取得する（なければ nullptr）
	const Chunk* getChunk(Point coord) const;
	Chunk*       getChunk(Point coord);

	/// @brief アクティブなチャンクの一覧を返す（コピー）
	Array<Chunk*>       getActiveChunks();
	Array<const Chunk*> getActiveChunks() const;

	/// @brief ワールド座標から地形高さをサンプリングする（チャンク未ロード時は 0 を返す）
	float sampleHeight(float wx, float wz) const;

	/// @brief チャンクを生成せずに直接 Perlin ノイズで高さを計算する
	/// @details MapGenerator の A* グリッド構築など、チャンクを事前生成したくない場合に使用する
	float computeHeight(float wx, float wz) const;

	/// @brief 前回 popNewChunks() 呼び出し以降に新規生成されたチャンク座標を取り出してクリアする
	Array<Point> popNewChunks()
	{
		Array<Point> result = std::move(m_newChunks);
		m_newChunks.clear();
		return result;
	}

private:
	using Key = int64;

	/// @brief チャンク座標をハッシュキーに変換する
	static Key makeKey(Point p)
	{
		return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
	}

	/// @brief ワールド座標からチャンク座標を計算する
	static Point worldToChunkCoord(Vec3 worldPos)
	{
		const int cx = static_cast<int>(Math::Floor(worldPos.x / CHUNK_SIZE));
		const int cy = static_cast<int>(Math::Floor(worldPos.z / CHUNK_SIZE));
		return { cx, cy };
	}

	/// @brief チャンクを手続き生成する（地形タイプ別 Perlin ノイズ）
	void generateChunk(Chunk& chunk);

	HashTable<Key, Chunk> m_chunks;
	Point                 m_cameraChunk{ 0x7FFFFFFF, 0x7FFFFFFF };  ///< 初回更新を必ず通すための無効初期値
	Array<Point>          m_newChunks;   ///< 前回 popNewChunks() 以降に新規生成されたチャンク座標

	/// @brief アクティブ範囲（カメラ周辺 ±ACTIVE_RANGE チャンク）
	static constexpr int ACTIVE_RANGE = 2;

	// ----- 地形生成パラメータ -----
	uint64      m_seed        = 20260316ULL;
	TerrainType m_terrainType = TerrainType::Hills;
	float       m_mapWidth    = 4096.0f;
	float       m_mapDepth    = 4096.0f;
	PerlinNoise m_perlin      = PerlinNoise{ m_seed };  ///< シードごとに一度だけ構築する
};
