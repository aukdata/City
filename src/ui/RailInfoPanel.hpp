#pragma once
#include "../railway/TrainConsist.hpp"

/// @brief 列車・駅の情報を表示する。選択・追跡の所有元はシーン。
namespace RailInfoPanel
{
	struct Summary
	{
		String title;
		Array<String> lines;
		bool canFollow = false;
	};
	/// @brief 実際の先頭車両の位置を追跡する。編成が未配置なら列車中心を使う。
	Vec3 followPosition(const Train& vehicle, const TrainNetwork& network);
	Summary train(const Train& vehicle, const TrainNetwork& network);
	Summary station(int stationId, const TrainNetwork& network);
	constexpr int kWidth = 300;
	int height(const Summary& summary);
	RectF followButton(const Summary& summary);
	RectF timetableButton(const Summary& summary);
	void draw(const Summary& summary, bool tracking, const Font& font);
} // namespace RailInfoPanel
