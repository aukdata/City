#pragma once

/// @brief ゲーム内時刻の型（ゲーム開始からの経過ゲーム秒）
using GameTime = double;

/// @brief 季節
enum class Season : uint8 { Spring, Summer, Autumn, Winter };

/// @brief ゲーム速度
enum class TimeSpeed : uint8 { Paused, x1, x2, x4 };

/// @brief ゲーム内時計
struct GameClock
{
	GameTime  now    = 8.0 * 60; ///< ゲーム開始からの経過ゲーム秒（午前8時スタート）
	int       year   = 1;          ///< 年
	uint8     month  = 4;          ///< 月 (1-12)
	uint8     day    = 1;          ///< 日 (1-30)
	float     hour   = 8.0f;      ///< 時刻 (0.0-24.0)
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
		constexpr int64 SecsPerHour = 60;
		constexpr int64 SecsPerDay  = 60 * 24;

		const int64 s = static_cast<int64>(now);
		hour  = static_cast<float>((s % SecsPerDay) / static_cast<double>(SecsPerHour));
		const int64 totalDays   = s / SecsPerDay;
		day   = static_cast<uint8>(totalDays % 30 + 1);
		const int64 totalMonths = totalDays / 30;
		month = static_cast<uint8>(totalMonths % 12 + 1);
		year  = static_cast<int>(totalMonths / 12) + 1;

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
		const int h = static_cast<int>(hour);
		const int s = static_cast<int>((hour - h) * 60);
		return U"Year{} {:02}/{:02} {:02}:{:02}"_fmt(year, month, day, h, s);
	}
};
