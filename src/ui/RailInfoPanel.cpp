#include "RailInfoPanel.hpp"
#include "PlainLabel.hpp"
#include "../railway/RailwaySite.hpp"

namespace RailInfoPanel
{
	Vec3 followPosition(const Train& vehicle, const TrainNetwork& network)
	{
		const auto pose = TrainConsist::carPose(vehicle, network, 0);
		return pose ? pose->position : vehicle.position;
	}
	Summary train(const Train& vehicle, const TrainNetwork& network)
	{
		Summary result;
		result.title = U"電車 #{}"_fmt(vehicle.id);
		result.canFollow = true;
		const auto* service = network.getSchedule(vehicle.scheduleId);
		result.lines << (service ? service->name : U"回送") << U"速度  {:.1f} km/h"_fmt(vehicle.speed * 3.6)
					 << U"編成  {}両  ／  乗客 {}人"_fmt(
							TrainConsist::profile(vehicle.type).cars, vehicle.passengerCount);
		static constexpr StringView state[]{U"走行中", U"駅に停車中", U"信号待ち", U"回送・休止"};
		result.lines << String{state[static_cast<size_t>(vehicle.state)]};
		if (vehicle.nextStopIdx >= 0 && vehicle.nextStopIdx < static_cast<int>(vehicle.serviceStops.size()))
		{
			if (const auto* next = network.getNode(vehicle.serviceStops[vehicle.nextStopIdx].stationNodeId))
			{
				result.lines << U"次の停車駅  {}"_fmt(next->name);
			}
		}
		return result;
	}
	Summary station(int stationId, const TrainNetwork& network)
	{
		Summary result;
		const auto* node = network.getNode(stationId);
		if (!node || node->type != TrackNodeType::Station)
		{
			return result;
		}
		result.title = node->name.ends_with(U"駅") ? node->name : node->name + U"駅";
		result.lines << (node->stationKind == StationKind::Underground	 ? U"地下駅"
							: node->stationKind == StationKind::Terminal ? U"ターミナル駅"
																		 : U"地上駅")
					 << U"ホーム  {}面"_fmt(RailwaySite::stationPaths(network, stationId).size());
		for (const auto& service : network.schedules())
		{
			bool stops = false;
			for (const auto& stop : service.stops)
			{
				stops |= stop.stationNodeId == stationId;
			}
			if (stops)
			{
				result.lines << service.name;
			}
		}
		if (result.lines.size() == 2)
		{
			result.lines << U"停車する定期列車はありません";
		}
		return result;
	}
	int height(const Summary& summary)
	{
		return static_cast<int>(summary.lines.size()) * 25 + 56;
	}
	RectF followButton(const Summary& summary)
	{
		return {8, height(summary) - 40, 130, 30};
	}
	RectF timetableButton(const Summary& summary)
	{
		return {summary.canFollow ? 150.0 : 8.0, static_cast<double>(height(summary) - 40), 130, 30};
	}
	void draw(const Summary& summary, bool tracking, const Font& font)
	{
		int y = 8;
		for (const auto& line : summary.lines)
		{
			PlainLabel::fitted(font, line, 14, {8, y, kWidth - 16, 23}, ColorF{.95}, ColorF{0, 0});
			y += 25;
		}
		const auto button = [&](RectF bounds, StringView label)
		{
			bounds.rounded(4).draw(ColorF{.16, .28, .33});
			PlainLabel::fitted(font, label, 14, bounds.stretched(-7, -4), ColorF{.95}, ColorF{0, 0});
		};
		if (summary.canFollow)
		{
			button(followButton(summary), tracking ? U"追跡をやめる" : U"追跡");
		}
		button(timetableButton(summary), U"ダイヤを開く");
	}
} // namespace RailInfoPanel
