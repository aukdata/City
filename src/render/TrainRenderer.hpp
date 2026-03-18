#pragma once
#include "../railway/TrainManager.hpp"
#include "../railway/TrainNetwork.hpp"

/// @brief 鉄道（線路・列車）の描画クラス
class TrainRenderer
{
public:
	/// @brief 線路を描画する
	void renderTracks(const TrainNetwork& network);

	/// @brief 列車を描画する
	void renderTrains(const Array<Train>& trains);

	/// @brief エッジの線路メッシュキャッシュを無効化する（線路変更時に呼ぶ）
	void markDirty(int edgeId);

private:
	/// @brief エッジの線路メッシュを構築する
	static Mesh buildTrackMesh(const TrackEdge& edge, const CubicBezier& bez);

	HashTable<int, Mesh> m_trackMeshCache;  ///< エッジ ID → 線路メッシュ
};
