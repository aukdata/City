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
};
