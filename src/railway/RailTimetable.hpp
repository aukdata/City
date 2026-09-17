#pragma once
#include "TrainNetwork.hpp"

/// @brief 毎日の発車時刻・経路検証・初期ダイヤを共用する。
namespace RailTimetable
{
	inline constexpr int kMinutesPerDay = 24 * 60;
	String formatTime(int minute);
	Optional<int> parseTime(const String& text);
	Array<int> route(const TrainNetwork& network, const TrainSchedule& schedule, bool reverse = false);
	String validate(const TrainNetwork& network, const TrainSchedule& schedule);
	double journeySeconds(const TrainNetwork& network, const TrainSchedule& schedule);
	TrainSchedule makeDefault(const TrainNetwork& network, int from, int to);
	/// @brief 過去の未消化便を積み上げず、現在の運行時間帯の直近の1便だけ発車待ちにする。
	Optional<GameTime> dueDeparture(const TrainSchedule& schedule, GameTime now);
	/// @brief now 以後の発車予定。遅れている場合は、その発車予定時刻を返す。
	Optional<GameTime> nextDeparture(const TrainSchedule& schedule, GameTime now);
	Array<GameTime> departures(const TrainSchedule& schedule, GameTime now, int count = 5);
}
