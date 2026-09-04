#include "EventSystem.hpp"

// =============================================================================
// 季節イベント定義テーブル
// =============================================================================

namespace
{
	constexpr double kHour = static_cast<double>(GameClock::kUnitsPerHour);
	constexpr double kDay  = static_cast<double>(GameClock::kUnitsPerDay);

	struct SeasonalEventDef
	{
		uint8       month;
		EventType   type;
		StringView  title;
		StringView  description;
		float       speedMultiplier;
		double      durationDays;
	};

	constexpr SeasonalEventDef kSeasonalEvents[] = {
		{  1, EventType::HatsuMode,         U"初詣渋滞",            U"神社・寺周辺が混雑します",                   0.50f,  3 },
		{  3, EventType::GraduationSeason,  U"卒業式シーズン",      U"学校周辺の朝ラッシュが悪化します",           0.80f,  7 },
		{  4, EventType::GraduationSeason,  U"入学式シーズン",      U"学校周辺の朝ラッシュが悪化します",           0.85f,  7 },
		{  5, EventType::GoldenWeek,        U"ゴールデンウィーク",  U"観光地へのアクセス路が大渋滞になります",     0.55f,  5 },
		{  7, EventType::SummerFestival,    U"夏祭り",              U"旧市街の一部道路が歩行者天国になります",     0.70f,  2 },
		{  8, EventType::OBonRush,          U"お盆帰省ラッシュ",    U"幹線道路の交通量が増加します",               0.60f,  4 },
		{ 10, EventType::FoliageSeason,     U"紅葉シーズン",        U"観光地への流入が増加します",                 0.75f, 14 },
		{ 12, EventType::NewYearRush,       U"年末年始ラッシュ",    U"幹線道路の交通量が増加します",               0.65f,  7 },
	};

	// ランダムイベント候補テーブル
	struct RandomEventDef
	{
		EventType   type;
		int         monthMin;
		int         monthMax;
		float       probability;
		StringView  title;
		StringView  description;
		float       speedMultiplier;
		double      durationMin;
		double      durationMax;
	};

	const RandomEventDef kRandomEvents[] = {
		{ EventType::Typhoon,              8, 10, 0.05f, U"台風接近",       U"全道路の速度が低下します",               0.60f, kDay * 2,     kDay * 4     },
		{ EventType::HeavyRain,            6,  9, 0.15f, U"大雨・冠水",     U"低地道路が通行困難になります",           0.70f, kDay * 1,     kDay * 3     },
		{ EventType::Snowfall,            12,  2, 0.20f, U"積雪",           U"道路が滑りやすくなります",               0.70f, kDay * 1,     kDay * 7     },
		{ EventType::TrafficAccident,      1, 12, 0.10f, U"交通事故発生",   U"事故区間が一時閉鎖されます",             0.80f, kHour * 2,    kHour * 6    },
		{ EventType::WaterPipeWork,        1, 12, 0.20f, U"水道管工事",     U"生活道路が車線減少します",               0.85f, kDay * 7 * 1, kDay * 7 * 4 },
		{ EventType::GasPipeWork,          1, 12, 0.10f, U"ガス管工事",     U"幹線道路が片側交互通行になります",       0.60f, kDay * 7 * 2, kDay * 7 * 6 },
		{ EventType::Rockfall,             1, 12, 0.03f, U"落石注意",       U"山間道路が一時閉鎖されます",             0.80f, kDay * 1,     kDay * 5     },
		{ EventType::Ekiden,              11,  1, 0.10f, U"駅伝大会",       U"指定ルートが一時閉鎖されます",           0.70f, kHour * 6,    kHour * 6    },
		{ EventType::Marathon,             4,  4, 0.50f, U"マラソン大会",   U"広範囲のルートが閉鎖されます",           0.60f, kHour * 8,    kHour * 8    },
	};

	double demandMultiplierForEvent(EventType type)
	{
		switch (type)
		{
		case EventType::GoldenWeek: return 2.50;
		case EventType::SummerFestival: return 1.80;
		case EventType::OBonRush: return 2.00;
		case EventType::Fireworks: return 2.00;
		case EventType::FoliageSeason: return 1.80;
		case EventType::NewYearRush: return 1.80;
		case EventType::HatsuMode: return 1.70;
		case EventType::GraduationSeason: return 1.30;
		case EventType::Typhoon: return 0.70;
		case EventType::HeavyRain: return 0.75;
		case EventType::Snowfall: return 0.80;
		case EventType::TrafficAccident: return 0.90;
		case EventType::LargeVehicleBreakdown: return 0.90;
		case EventType::WaterPipeWork: return 0.95;
		case EventType::GasPipeWork: return 0.90;
		case EventType::Landslide: return 0.80;
		case EventType::Rockfall: return 0.90;
		case EventType::Ekiden: return 1.25;
		case EventType::Marathon: return 1.30;
		}
		return 1.0;
	}
	bool isMonthInRange(uint8 month, int monthMin, int monthMax)
	{
		return (monthMin <= monthMax)
			? (month >= monthMin && month <= monthMax)
			: (month >= monthMin || month <= monthMax);
	}
}

// =============================================================================
// 公開メソッド
// =============================================================================

void EventSystem::update(GameTime gameNow)
{
	removeExpired(gameNow);
}

void EventSystem::rollMonthly(GameTime gameNow, uint8 month, int64 elapsedMonthIndex, const RoadNetwork& network)
{
	// 月替わりごとに季節イベントを確定し、さらに条件に合うランダムイベントを 1 回だけ抽選する。
	if (m_lastRollMonthIndex == elapsedMonthIndex)
	{
		return;
	}
	m_lastRollMonthIndex = elapsedMonthIndex;

	if (auto ev = buildSeasonalEvent(month, gameNow))
	{
		addEvent(*ev);
	}

	if (auto ev = rollRandomEvent(month, gameNow, network))
	{
		addEvent(*ev);
	}
}

float EventSystem::globalSpeedMultiplier() const
{
	float minMul = 1.0f;
	for (const auto& ev : m_active)
	{
		minMul = Min(minMul, ev.speedMultiplier);
	}
	return minMul;
}

double EventSystem::globalDemandMultiplier() const
{
	double multiplier = 1.0;
	for (const auto& event : m_active)
	{
		multiplier = Max(multiplier, demandMultiplierForEvent(event.type));
	}
	return multiplier;
}
Array<GameEvent> EventSystem::popNewNotifications()
{
	// 新着通知は UI 側で一度だけ消費できるよう、保留配列を丸ごと受け渡して空にする。
	Array<GameEvent> out = std::move(m_pendingNotifications);
	m_pendingNotifications.clear();
	return out;
}

void EventSystem::addEvent(GameEvent ev)
{
	// 発生イベントは通知待ちキューと有効イベント一覧の両方へ積み、同時に寿命管理対象へ載せる。
	ev.id = m_nextId++;
	ev.notified = false;
	m_pendingNotifications << ev;
	m_active << std::move(ev);
}

void EventSystem::removeExpired(GameTime now)
{
	// 期限を過ぎたイベントだけを落とし、速度補正などの全体効果を自動的に現在分へ絞る。
	m_active.remove_if([now](const GameEvent& ev) { return ev.endAt < now; });
}

// =============================================================================
// 季節イベント生成（テーブル駆動）
// =============================================================================

Optional<GameEvent> EventSystem::buildSeasonalEvent(uint8 month, GameTime now) const
{
	// 季節イベントは月テーブルから固定で 1 件選び、その場で有効期間付きイベントへ展開する。
	for (const auto& def : kSeasonalEvents)
	{
		if (def.month != month)
		{
			continue;
		}

		GameEvent ev;
		ev.type            = def.type;
		ev.title           = String{ def.title };
		ev.description     = String{ def.description };
		ev.speedMultiplier = def.speedMultiplier;
		ev.startAt         = now;
		ev.endAt           = now + kDay * def.durationDays;
		return ev;
	}
	return none;
}

// =============================================================================
// ランダムイベント（テーブル駆動）
// =============================================================================

Optional<GameEvent> EventSystem::rollRandomEvent(uint8 month, GameTime now,
                                                  [[maybe_unused]] const RoadNetwork& network) const
{
	// ランダムイベントは月条件と確率を順に評価し、最初に成立した候補をその月の突発事象として採用する。
	for (const auto& def : kRandomEvents)
	{
		if (!isMonthInRange(month, def.monthMin, def.monthMax))
		{
			continue;
		}
		if (Random(0.0f, 1.0f) > def.probability)
		{
			continue;
		}

		GameEvent ev;
		ev.type            = def.type;
		ev.title           = String{ def.title };
		ev.description     = String{ def.description };
		ev.speedMultiplier = def.speedMultiplier;
		ev.startAt         = now;
		ev.endAt           = now + Random(def.durationMin, def.durationMax);
		return ev;
	}
	return none;
}
