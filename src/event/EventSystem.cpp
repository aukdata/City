#include "EventSystem.hpp"

void EventSystem::update(GameTime gameNow, uint8 month, double dt)
{
	removeExpired(gameNow);
}

void EventSystem::rollMonthly(GameTime gameNow, uint8 month, const RoadNetwork& network)
{
	if (m_lastRollMonth == static_cast<int>(month)) return;
	m_lastRollMonth = static_cast<int>(month);

	// 季節イベント
	if (auto ev = buildSeasonalEvent(month, gameNow))
		addEvent(*ev);

	// ランダムイベント
	if (auto ev = rollRandomEvent(month, gameNow, network))
		addEvent(*ev);
}

float EventSystem::globalSpeedMultiplier() const
{
	float minMul = 1.0f;
	for (const auto& ev : m_active)
		minMul = Min(minMul, ev.speedMultiplier);
	return minMul;
}

Array<GameEvent> EventSystem::popNewNotifications()
{
	Array<GameEvent> out = std::move(m_pendingNotifications);
	m_pendingNotifications.clear();
	return out;
}

void EventSystem::addEvent(GameEvent ev)
{
	ev.id = m_nextId++;
	ev.notified = false;
	m_pendingNotifications << ev;
	m_active << std::move(ev);
}

void EventSystem::removeExpired(GameTime now)
{
	m_active.remove_if([now](const GameEvent& ev) { return ev.endAt < now; });
}

Optional<GameEvent> EventSystem::buildSeasonalEvent(uint8 month, GameTime now) const
{
	constexpr double kDay = 86400.0;  // 1ゲーム日 = 86400 ゲーム秒

	GameEvent ev;
	ev.startAt = now;

	switch (month)
	{
	case 1:
		ev.type = EventType::HatsuMode;
		ev.title = U"初詣渋滞";
		ev.description = U"神社・寺周辺が混雑します";
		ev.speedMultiplier = 0.5f;
		ev.endAt = now + kDay * 3;
		break;
	case 3:
		ev.type = EventType::GraduationSeason;
		ev.title = U"卒業式シーズン";
		ev.description = U"学校周辺の朝ラッシュが悪化します";
		ev.speedMultiplier = 0.8f;
		ev.endAt = now + kDay * 7;
		break;
	case 4:
		ev.type = EventType::GraduationSeason;
		ev.title = U"入学式シーズン";
		ev.description = U"学校周辺の朝ラッシュが悪化します";
		ev.speedMultiplier = 0.85f;
		ev.endAt = now + kDay * 7;
		break;
	case 5:
		ev.type = EventType::GoldenWeek;
		ev.title = U"ゴールデンウィーク";
		ev.description = U"観光地へのアクセス路が大渋滞になります";
		ev.speedMultiplier = 0.55f;
		ev.endAt = now + kDay * 5;
		break;
	case 7:
		ev.type = EventType::SummerFestival;
		ev.title = U"夏祭り";
		ev.description = U"旧市街の一部道路が歩行者天国になります";
		ev.speedMultiplier = 0.7f;
		ev.endAt = now + kDay * 2;
		break;
	case 8:
		ev.type = EventType::OBonRush;
		ev.title = U"お盆帰省ラッシュ";
		ev.description = U"幹線道路の交通量が増加します";
		ev.speedMultiplier = 0.6f;
		ev.endAt = now + kDay * 4;
		break;
	case 10:
		ev.type = EventType::FoliageSeason;
		ev.title = U"紅葉シーズン";
		ev.description = U"観光地への流入が増加します";
		ev.speedMultiplier = 0.75f;
		ev.endAt = now + kDay * 14;
		break;
	case 12:
		ev.type = EventType::NewYearRush;
		ev.title = U"年末年始ラッシュ";
		ev.description = U"幹線道路の交通量が増加します";
		ev.speedMultiplier = 0.65f;
		ev.endAt = now + kDay * 7;
		break;
	default:
		return none;
	}
	return ev;
}

Optional<GameEvent> EventSystem::rollRandomEvent(uint8 month, GameTime now, const RoadNetwork& network) const
{
	constexpr double kDay  = 86400.0;
	constexpr double kHour = 3600.0;

	GameEvent ev;
	ev.startAt = now;

	// 季節ごとのランダムイベント確率テーブル
	struct RollEntry { EventType type; int monthMin; int monthMax; float prob; };
	constexpr RollEntry table[] = {
		{ EventType::Typhoon,              8, 10, 0.05f },
		{ EventType::HeavyRain,            6,  9, 0.15f },
		{ EventType::Snowfall,            12,  2, 0.20f },
		{ EventType::TrafficAccident,      1, 12, 0.10f },
		{ EventType::WaterPipeWork,        1, 12, 0.20f },
		{ EventType::GasPipeWork,          1, 12, 0.10f },
		{ EventType::Rockfall,             1, 12, 0.03f },
		{ EventType::Ekiden,              11,  1, 0.10f },
		{ EventType::Marathon,             4,  4, 0.50f },
	};

	for (const auto& entry : table)
	{
		bool inSeason = (entry.monthMin <= entry.monthMax)
			? (month >= entry.monthMin && month <= entry.monthMax)
			: (month >= entry.monthMin || month <= entry.monthMax);
		if (!inSeason) continue;

		if (Random(0.0f, 1.0f) > entry.prob) continue;

		ev.type = entry.type;
		switch (entry.type)
		{
		case EventType::Typhoon:
			ev.title = U"台風接近";
			ev.description = U"全道路の速度が低下します";
			ev.speedMultiplier = 0.6f;
			ev.endAt = now + kDay * Random(2, 4);
			break;
		case EventType::HeavyRain:
			ev.title = U"大雨・冠水";
			ev.description = U"低地道路が通行困難になります";
			ev.speedMultiplier = 0.7f;
			ev.endAt = now + kDay * Random(1, 3);
			break;
		case EventType::Snowfall:
			ev.title = U"積雪";
			ev.description = U"道路が滑りやすくなります";
			ev.speedMultiplier = 0.7f;
			ev.endAt = now + kDay * Random(1, 7);
			break;
		case EventType::TrafficAccident:
			ev.title = U"交通事故発生";
			ev.description = U"事故区間が一時閉鎖されます";
			ev.speedMultiplier = 0.8f;
			ev.endAt = now + kHour * Random(2, 6);
			break;
		case EventType::WaterPipeWork:
			ev.title = U"水道管工事";
			ev.description = U"生活道路が車線減少します";
			ev.speedMultiplier = 0.85f;
			ev.endAt = now + kDay * 7 * Random(1, 4);
			break;
		case EventType::GasPipeWork:
			ev.title = U"ガス管工事";
			ev.description = U"幹線道路が片側交互通行になります";
			ev.speedMultiplier = 0.6f;
			ev.endAt = now + kDay * 7 * Random(2, 6);
			break;
		case EventType::Rockfall:
			ev.title = U"落石注意";
			ev.description = U"山間道路が一時閉鎖されます";
			ev.speedMultiplier = 0.8f;
			ev.endAt = now + kDay * Random(1, 5);
			break;
		case EventType::Ekiden:
			ev.title = U"駅伝大会";
			ev.description = U"指定ルートが一時閉鎖されます";
			ev.speedMultiplier = 0.7f;
			ev.endAt = now + kHour * 6;
			break;
		case EventType::Marathon:
			ev.title = U"マラソン大会";
			ev.description = U"広範囲のルートが閉鎖されます";
			ev.speedMultiplier = 0.6f;
			ev.endAt = now + kHour * 8;
			break;
		default:
			continue;
		}
		return ev;
	}
	return none;
}
