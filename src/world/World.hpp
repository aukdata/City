#pragma once
#include "Chunk.hpp"

/// @brief チャンク管理クラス
/// @details カメラ周辺 5×5 チャンクをアクティブに保つ
class World
{
public:
	/// @brief カメラ位置を渡してアクティブチャンクを更新する
	void update(Vec3 cameraWorldPos);

	/// @brief チャンクを取得する（なければ生成して返す）
	Chunk& getOrCreateChunk(Point coord);

	/// @brief チャンクを取得する（なければ nullptr）
	const Chunk* getChunk(Point coord) const;
	Chunk*       getChunk(Point coord);

	/// @brief アクティブなチャンクの一覧を返す（コピー）
	Array<Chunk*>       getActiveChunks();
	Array<const Chunk*> getActiveChunks() const;

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

	/// @brief チャンクを手続き生成する（Phase 1: フラット地形）
	static void generateChunk(Chunk& chunk);

	HashTable<Key, Chunk> m_chunks;
	Point                 m_cameraChunk{ 0x7FFFFFFF, 0x7FFFFFFF };  ///< 初回更新を必ず通すための無効初期値

	/// @brief アクティブ範囲（カメラ周辺 ±ACTIVE_RANGE チャンク）
	static constexpr int ACTIVE_RANGE = 2;
};
