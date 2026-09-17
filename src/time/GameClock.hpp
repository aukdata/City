#pragma once

/// @brief ゲーム内時刻の型（ゲーム開始からの経過シミュレーション秒）
using GameTime = double;

/// @brief 季節
enum class Season : uint8 { Spring, Summer, Autumn, Winter };

/// @brief ゲーム速度
enum class TimeSpeed : uint8 { Paused, x1, x2, x4 };

/// @brief ゲーム内時計
struct GameClock
{
	static constexpr double kCalendarMinutesPerHour = 60.0;
	static constexpr double kCalendarMinutesPerDay = 24.0 * kCalendarMinutesPerHour;
	static constexpr double kCalendarDaysPerMonth = 30.0;
	static constexpr double kCalendarMinutesPerMonth = kCalendarMinutesPerDay * kCalendarDaysPerMonth;
	static constexpr double kSecondsPerGameDay = 24.0 * 60.0; ///< 標準速度で現実24分が1日
	static constexpr double kSecondsPerGameHour = kSecondsPerGameDay / 24.0;
	static constexpr double kSecondsPerGameMonth = kSecondsPerGameDay * kCalendarDaysPerMonth;
	static constexpr double kCalendarMinutesPerSecond = kCalendarMinutesPerDay / kSecondsPerGameDay;
	static constexpr double kInitialCalendarMinute = 3.0 * kCalendarMinutesPerMonth + 8.0 * kCalendarMinutesPerHour;

	// 旧実装・周辺システム向けの単位エイリアス。GameTime はシミュレーション秒のまま扱う。
	static constexpr double kUnitsPerHour = kSecondsPerGameHour;
	static constexpr double kUnitsPerDay = kSecondsPerGameDay;
	static constexpr double kUnitsPerMonth = kSecondsPerGameMonth;

	GameTime  now    = 0.0;        ///< ゲーム開始からの経過シミュレーション秒
	int       year   = 1;          ///< 年
	uint8     month  = 4;          ///< 月 (1-12)
	uint8     day    = 1;          ///< 日 (1-30)
	float     hour   = 8.0f;       ///< 時刻 (0.0-24.0)
	Season    season = Season::Spring;
	TimeSpeed speed  = TimeSpeed::x1;

	/// @brief リアル経過秒を渡してゲーム時刻を進める
	/// @param realDt リアル経過秒
	void advance(double realDt)
	{
		if (speed == TimeSpeed::Paused) return;
		now += realDt * speedMultiplier();
		syncCalendar();
	}

	/// @brief year/month/day/hour/season を now から再計算する
	void syncCalendar()
	{
		const double calendarMinutes = calendarMinuteFromTime(now);
		const int64 totalDays = static_cast<int64>(Math::Floor(calendarMinutes / kCalendarMinutesPerDay));
		const double dayElapsed = calendarMinutes - static_cast<double>(totalDays) * kCalendarMinutesPerDay;
		hour = static_cast<float>(dayElapsed / kCalendarMinutesPerHour);

		day = static_cast<uint8>(totalDays % 30 + 1);
		const int64 totalMonths = totalDays / 30;
		month = static_cast<uint8>(totalMonths % 12 + 1);
		year = static_cast<int>(totalMonths / 12) + 1;

		if      (month >= 3 && month <= 5)  season = Season::Spring;
		else if (month >= 6 && month <= 8)  season = Season::Summer;
		else if (month >= 9 && month <= 11) season = Season::Autumn;
		else                                season = Season::Winter;
	}

	/// @brief 速度切り替え（Paused → x1 → x2 → x4 → Paused）
	void cycleSpeed()
	{
		switch (speed)
		{
		case TimeSpeed::Paused: speed = TimeSpeed::x1;     break;
		case TimeSpeed::x1:     speed = TimeSpeed::x2;     break;
		case TimeSpeed::x2:     speed = TimeSpeed::x4;     break;
		case TimeSpeed::x4:     speed = TimeSpeed::Paused; break;
		}
	}

	/// @brief 速度倍率を返す
	double speedMultiplier() const
	{
		switch (speed)
		{
		case TimeSpeed::Paused: return 0.0;
		case TimeSpeed::x1:     return 1.0;
		case TimeSpeed::x2:     return 2.0;
		case TimeSpeed::x4:     return 4.0;
		}
		return 0.0;
	}

	/// @brief 現在の年月を 0 起点の通算月インデックスで返す
	int64 monthIndex() const
	{
		return MonthIndexFromTime(now);
	}

	/// @brief 互換用: 現在の年月を 0 起点の通算月インデックスで返す
	int64 elapsedMonthIndex() const
	{
		return monthIndex();
	}

	/// @brief 任意のゲーム時刻から 0 起点の通算月インデックスを返す
	static int64 MonthIndexFromTime(GameTime gameNow)
	{
		return static_cast<int64>(Math::Floor(calendarMinuteFromTime(gameNow) / kCalendarMinutesPerMonth));
	}

	/// @brief 0 起点の通算月インデックスから、その月初の GameTime を返す
	static GameTime TimeFromMonthIndex(int64 monthIndex)
	{
		const double calendarMinutes = static_cast<double>(monthIndex) * kCalendarMinutesPerMonth;
		return (calendarMinutes - kInitialCalendarMinute) / kCalendarMinutesPerSecond;
	}

	/// @brief 0 起点の通算月インデックスから月 (1-12) を返す
	static uint8 MonthFromMonthIndex(int64 monthIndex)
	{
		const int64 wrapped = ((monthIndex % 12) + 12) % 12;
		return static_cast<uint8>(wrapped + 1);
	}

	/// @brief 互換用: 0 起点の通算月インデックスから月 (1-12) を返す
	static uint8 monthFromElapsedMonthIndex(int64 monthIndex)
	{
		return MonthFromMonthIndex(monthIndex);
	}

	/// @brief 任意のゲーム時刻をカレンダー分へ変換する
	static double calendarMinuteFromTime(GameTime gameNow)
	{
		return Max(0.0,kInitialCalendarMinute + gameNow * kCalendarMinutesPerSecond);
	}

	/// @brief 速度文字列を返す
	StringView speedString() const
	{
		switch (speed)
		{
		case TimeSpeed::Paused: return U"||";
		case TimeSpeed::x1:     return U"▶ x1";
		case TimeSpeed::x2:     return U"▶▶ x2";
		case TimeSpeed::x4:     return U"▶▶▶ x4";
		}
		return U"";
	}

	/// @brief 時刻文字列を返す（"Year1 04/01 06:00" 形式）
	String timeString() const
	{
		int h = static_cast<int>(Math::Floor(hour));
		int minute = static_cast<int>(Math::Floor((hour - h) * 60.0f + 0.5f));
		if (minute >= 60)
		{
			minute -= 60;
			++h;
		}
		if (h >= 24) h -= 24;
		return U"Year{} {:02}/{:02} {:02}:{:02}"_fmt(year, month, day, h, minute);
	}
};
