#include "RailTimetable.hpp"
#include "TrainConsist.hpp"

namespace RailTimetable
{
	namespace
	{
		constexpr double kEpsilon = .0001;
		double calendarAt(GameTime now) { return GameClock::calendarMinuteFromTime(now); }
		GameTime simulationAt(double minute) { return minute - calendarAt(0); }
		double duration(const TrainSchedule& schedule)
		{
			return (schedule.lastDepartureMinute - schedule.firstDepartureMinute + kMinutesPerDay) % kMinutesPerDay;
		}
		bool validTimes(const TrainSchedule& schedule)
		{
			return InRange(schedule.firstDepartureMinute, 0, kMinutesPerDay - 1)
				&& InRange(schedule.lastDepartureMinute, 0, kMinutesPerDay - 1)
				&& std::isfinite(schedule.headwaySec) && schedule.headwaySec >= 1 && schedule.headwaySec <= kMinutesPerDay;
		}
	}
	String formatTime(int minute)
	{
		minute = (minute % kMinutesPerDay + kMinutesPerDay) % kMinutesPerDay;
		return U"{:02}:{:02}"_fmt(minute / 60, minute % 60);
	}
	Optional<int> parseTime(const String& text)
	{
		const auto pieces = text.trimmed().split(U':');
		if (pieces.size() != 2) { return none; }
		const auto hour = ParseIntOpt<int>(pieces[0], Arg::radix = 10), minute = ParseIntOpt<int>(pieces[1], Arg::radix = 10);
		if (!hour || !minute || !InRange(*hour, 0, 23) || !InRange(*minute, 0, 59)) { return none; }
		return *hour * 60 + *minute;
	}
	Array<int> route(const TrainNetwork& network, const TrainSchedule& schedule, bool reverse)
	{
		Array<int> result;
		for (size_t index = 1; index < schedule.stops.size(); ++index)
		{
			// 復路は駅順と探索方向を反転する。一方通行の軌道では往路と別の区間を使える。
			const size_t from = reverse ? schedule.stops.size()-index : index-1;
			const size_t to = reverse ? from-1 : index;
			const auto leg = network.findRoute(schedule.stops[from].stationNodeId,schedule.stops[to].stationNodeId);
			if (leg.isEmpty()) { return {}; }
			result.append(leg);
		}
		return result;
	}
	String validateSettings(const TrainSchedule& schedule)
	{
		if (!validTimes(schedule)) { return U"始発・終発は00:00〜23:59、間隔は1〜1440分です"; }
		if (schedule.stops.size() < 2 || schedule.stops.size() > 12) { return U"停車駅を2〜12駅選んでください"; }
		HashSet<int> stations;
		for (const auto& stop : schedule.stops)
		{
			if (stop.stationNodeId < 0) { return U"存在する駅を選んでください"; }
			if (stations.contains(stop.stationNodeId)) { return U"同じ駅は一度だけ指定します。復路は自動で逆順です"; }
			stations.insert(stop.stationNodeId);
			if (!std::isfinite(stop.dwellSec) || !InRange(stop.dwellSec, 0.0f, 120.0f)) { return U"停車時間は0〜120分です"; }
		}
		return U"";
	}
	String validate(const TrainNetwork& network, const TrainSchedule& schedule)
	{
		const String settingsError = validateSettings(schedule);
		if (!settingsError.isEmpty()) { return settingsError; }
		HashSet<int> usedEdges;
		for (size_t index = 0; index < schedule.stops.size(); ++index)
		{
			const auto& stop = schedule.stops[index];
			const auto* station = network.getNode(stop.stationNodeId);
			if (!station || station->type != TrackNodeType::Station) { return U"存在する駅を選んでください"; }
			if (index == 0) { continue; }
			const auto leg = network.findRoute(schedule.stops[index - 1].stationNodeId, stop.stationNodeId);
			if (leg.isEmpty()) { return U"停車駅の間が線路でつながっていません"; }
			if (network.findRoute(stop.stationNodeId,schedule.stops[index-1].stationNodeId).isEmpty()) { return U"復路に使える向きの軌道がありません"; }
			double length = 0;
			for (const int id : leg)
			{
				const auto* edge = network.getEdge(id);
				if (!edge->electrified || edge->speedLimit <= 0 || !std::isfinite(edge->speedLimit)) { return U"全区間を電化し、制限速度を設定してください"; }
				if (usedEdges.contains(id)) { return U"途中で折り返さない順序に停車駅を並べてください"; }
				usedEdges.insert(id); length += edge->length;
			}
			if (length <= TrainConsist::length(schedule.type) + 2) { return U"駅間が編成長より短いため発車できません"; }
		}
		return U"";
	}
	double journeySeconds(const TrainNetwork& network, const TrainSchedule& schedule)
	{
		double seconds = 0;
		for (const int id : route(network, schedule))
		{
			const auto* edge = network.getEdge(id);
			seconds += edge->length / Max(1.0, Min(static_cast<double>(edge->speedLimit), static_cast<double>(TrainConsist::profile(schedule.type).maximumSpeed)) / 3.6);
		}
		// 各駅の加減速と停車を含む目安。経路予約による待ち時間は含めない。
		for (const auto& stop : schedule.stops) { seconds += stop.dwellSec + 15; }
		return seconds;
	}
	TrainSchedule makeDefault(const TrainNetwork& network, int from, int to)
	{
		TrainSchedule schedule;
		schedule.stops = {{from, 2}, {to, 2}};
		const auto* a = network.getNode(from); const auto* b = network.getNode(to);
		schedule.name = a && b ? U"{}・{}線"_fmt(a->name, b->name) : U"新しい路線";
		if (a && b && a->position.distanceFromSq(b->position) > Square(12000.0)) { schedule.type = TrainType::Express; }
		schedule.headwaySec = static_cast<float>(Clamp(std::ceil((journeySeconds(network, schedule) + 15) / 30) * 30, 120.0, 1440.0));
		return schedule;
	}
	Optional<GameTime> dueDeparture(const TrainSchedule& schedule, GameTime now)
	{
		if (!schedule.enabled || !validTimes(schedule) || !std::isfinite(now)) { return none; }
		const double minute = calendarAt(now), today = std::floor(minute / kMinutesPerDay) * kMinutesPerDay;
		for (const int day : {0, -1})
		{
			const double start = today + day * kMinutesPerDay + schedule.firstDepartureMinute;
			if (minute < start || minute >= start + duration(schedule) + 1) { continue; }
			const double latest = start + std::floor((Min(minute - start, duration(schedule)) + kEpsilon) / schedule.headwaySec) * schedule.headwaySec;
			const GameTime departure = simulationAt(latest);
			if (departure > schedule.lastSpawnAt + kEpsilon) { return departure; }
		}
		return none;
	}
	Optional<GameTime> nextDeparture(const TrainSchedule& schedule, GameTime now)
	{
		if (!schedule.enabled || !validTimes(schedule)) { return none; }
		if (const auto due = dueDeparture(schedule, now)) { return due; }
		const double minute = calendarAt(now), today = std::floor(minute / kMinutesPerDay) * kMinutesPerDay;
		const double after = Max(minute, calendarAt(Max(0.0, schedule.lastSpawnAt)) + kEpsilon);
		for (int day = -1; day <= 2; ++day)
		{
			const double start = today + day * kMinutesPerDay + schedule.firstDepartureMinute;
			const double next = start + Max(0.0, std::ceil((after - start) / schedule.headwaySec)) * schedule.headwaySec;
			if (next <= start + duration(schedule) + kEpsilon) { return simulationAt(next); }
		}
		return none;
	}
	Array<GameTime> departures(const TrainSchedule& schedule, GameTime now, int count)
	{
		Array<GameTime> result; auto preview = schedule;
		for (int index = 0; index < count; ++index)
		{
			const auto next = nextDeparture(preview, now);
			if (!next) { break; }
			result << *next; preview.lastSpawnAt = Max(*next, now); now = Max(now, *next);
		}
		return result;
	}
}
