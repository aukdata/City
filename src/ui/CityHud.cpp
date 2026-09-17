#include "CityHud.hpp"
#include "NavigationHelp.hpp"
#include "PlainLabel.hpp"
#include "../render/UIRenderer.hpp"

namespace
{
	const ColorF panelColor{.055,.085,.105,.88};
	const ColorF textColor{.92,.95,.96};
	const ColorF mutedColor{.64,.73,.77};
	const ColorF accentColor{.35,.78,.82};

	/// @brief 長い通知や大きい金額でも、文字を隣の操作領域へはみ出させない。
	void label(const Font& font, StringView text, RectF area, ColorF color = textColor, double size = 13)
	{
		PlainLabel::fitted(font, text, size, area, color, ColorF{.03,.5});
	}

	void button(const Font& font, StringView text, RectF area, bool selected = false)
	{
		const bool hovered = area.mouseOver();
		area.rounded(4).draw(selected ? ColorF{.14,.32,.37} : hovered ? ColorF{.20,.28,.32} : ColorF{.11,.16,.19});
		label(font, text, {area.x+7,area.y+6,area.w-14,20}, selected ? accentColor : textColor, 12);
		if (hovered) { Cursor::RequestStyle(CursorStyle::Hand); }
	}

	ColorF balanceColor(double value) { return value >= 0 ? ColorF{.57,.86,.64} : ColorF{1,.55,.45}; }
}

void CityHud::updateLayout(Size size, bool walking, bool driving, bool hasMode)
{
	m_size = size; m_walking = walking; m_driving = driving; m_hasMode = hasMode;
	const bool streetView = walking || driving;
	m_summary = {10,10,streetView ? 200.0 : Min(360.0,Max(260.0,size.x-240.0)),streetView ? 32.0 : 94.0};
	m_mode = {m_summary.x,m_summary.br().y+5,m_summary.w,26};
	m_details = {m_summary.x,m_summary.br().y+(hasMode && !streetView ? 39 : 8),m_summary.w,238};
	m_mapHeader = {size.x-210.0,10,200,30};
	// 最小ウィンドウでも地名ヘッダ・下部の住所表示・運転計器に重ねない。
	m_helpButton = {size.x-94.0,size.y-38.0,84,28};
	m_helpArea = NavigationHelp::bounds({size.x,size.y-40});
	if (streetView) { m_tab = Tab::None; }
	if (driving) { m_helpOpen = false; }
}

RectF CityHud::tabBounds(Tab tab) const
{
	const double width = (m_summary.w-20)/3;
	return {m_summary.x+6+(static_cast<int>(tab)-1)*(width+4),m_summary.y+62,width,26};
}

RectF CityHud::pauseBounds() const
{
	return {m_summary.br().x-(m_walking || m_driving ? 48 : 100),m_summary.y+3,44,26};
}

RectF CityHud::speedBounds() const
{
	return {m_summary.br().x-50,m_summary.y+3,44,26};
}

Optional<RectF> CityHud::minimapBounds() const
{
	if (!m_mapOpen) { return none; }
	return RectF{m_mapHeader.x,m_mapHeader.br().y+4,200,200};
}

Array<RectF> CityHud::bounds() const
{
	Array<RectF> areas{m_summary,m_mapHeader};
	if (const auto map = minimapBounds()) { areas << *map; }
	if (m_tab != Tab::None) { areas << m_details; }
	if (m_hasMode && !m_driving) { areas << m_mode; }
	if (!m_driving) { areas << m_helpButton; }
	if (m_helpOpen) { areas << m_helpArea; }
	return areas;
}

bool CityHud::blocksMouse(Vec2 point) const
{
	if (m_consumed) { return true; }
	for (const auto& area : bounds()) { if (area.contains(point)) { return true; } }
	return false;
}

CityHud::Action CityHud::interact(Vec2 point, bool clicked, bool enabled)
{
	m_consumed = false;
	if (!enabled || !clicked) { return Action::None; }
	for (const auto& area : bounds()) { m_consumed |= area.contains(point); }
	if (pauseBounds().contains(point)) { return Action::TogglePause; }
	if (!m_walking && !m_driving && speedBounds().contains(point)) { return Action::NextSpeed; }
	if (m_mapHeader.contains(point)) { m_mapOpen = !m_mapOpen; }
	if (!m_driving && m_helpButton.contains(point)) { m_helpOpen = !m_helpOpen; m_tab = Tab::None; }
	if (!m_walking && !m_driving)
	{
		for (const auto tab : {Tab::City,Tab::Traffic,Tab::Notices})
		{
			if (!tabBounds(tab).contains(point)) { continue; }
			m_tab = m_tab == tab ? Tab::None : tab;
			m_helpOpen = false;
		}
	}
	return Action::None;
}

void CityHud::draw(const GameClock& clock, int vehicleCount, StringView mode, const Economy& economy,
	const CityHudStats& stats, const Font& font) const
{
	m_summary.rounded(5).draw(panelColor);
	const bool streetView = m_walking || m_driving;
	const String date = streetView
		? U"{}/{} {:02}:{:02}"_fmt(clock.month,clock.day,static_cast<int>(clock.hour),static_cast<int>(clock.hour*60)%60)
		: clock.timeString();
	label(font,date,{m_summary.x+9,m_summary.y+8,m_summary.w-(streetView ? 56 : 104),20});
	button(font,clock.speed == TimeSpeed::Paused ? U"再開" : U"停止",pauseBounds());
	if (!streetView)
	{
		button(font,clock.speed == TimeSpeed::Paused ? U"速度" : clock.speed == TimeSpeed::x1 ? U"×1" : clock.speed == TimeSpeed::x2 ? U"×2" : U"×4",speedBounds());
		const double half = (m_summary.w-24)*.5;
		label(font,U"人口 {}人"_fmt(economy.population),{m_summary.x+9,m_summary.y+37,half,20});
		label(font,U"資金 {:.1f}億円"_fmt(economy.funds),{m_summary.x+15+half,m_summary.y+37,half,20},balanceColor(economy.funds));
		button(font,U"街の情報",tabBounds(Tab::City),m_tab == Tab::City);
		button(font,U"交通",tabBounds(Tab::Traffic),m_tab == Tab::Traffic);
		const int noticeCount = static_cast<int>(stats.activeEventSummaries.size()+stats.notificationSummaries.size());
		button(font,noticeCount ? U"通知 {}"_fmt(noticeCount) : U"通知",tabBounds(Tab::Notices),m_tab == Tab::Notices);
	}

	// 徒歩での運転開始失敗など、操作に必要な案内は街の詳細を畳んでも残す。
	if (m_hasMode && !m_driving)
	{
		m_mode.rounded(4).draw(panelColor);
		label(font,mode,{m_mode.x+9,m_mode.y+5,m_mode.w-18,18},accentColor,12);
	}

	button(font,m_mapOpen ? U"周辺地図   −" : U"周辺地図   ＋",m_mapHeader);
	if (!m_driving) { button(font,m_helpOpen ? U"閉じる ?" : U"操作 ?",m_helpButton,m_helpOpen); }
	if (m_helpOpen) { NavigationHelp::draw(font,{m_size.x,m_size.y-40},m_walking); }
	if (m_tab == Tab::None) { return; }

	m_details.rounded(5).draw(panelColor);
	double y = m_details.y+12;
	const auto row = [&](StringView text, ColorF color = textColor)
	{
		label(font,text,{m_details.x+12,y,m_details.w-24,20},color);
		y += 25;
	};
	if (m_tab == Tab::City)
	{
		row(U"街の情報",accentColor);
		row(U"月次収支  {:+.2f}億円"_fmt(stats.monthlyBalance),balanceColor(stats.monthlyBalance));
		row(U"収入 +{:.2f} / 支出 −{:.2f}億円"_fmt(stats.monthlyIncome,stats.monthlyExpense),mutedColor);
		row(U"幸福度  {:.0f}%"_fmt(Clamp(economy.happiness,0.0,1.0)*100));
		row(U"住宅  {}人分"_fmt(stats.housingCapacity));
		row(U"住宅充足率  {:.0f}%"_fmt(stats.housingFulfillment*100),mutedColor);
		row(U"同じタブを押すと閉じます",mutedColor);
	}
	else if (m_tab == Tab::Traffic)
	{
		row(U"交通の状況",accentColor);
		row(U"車両 {}台 / 渋滞区間 {}"_fmt(vehicleCount,stats.congestedEdgeCount));
		row(U"平均速度 {:.1f} km/h"_fmt(stats.averageSpeedKmh));
		row(U"制限速度比 {:.0f}% / 低速 {}台"_fmt(stats.averageSpeedRatio*100,stats.slowVehicleCount),mutedColor);
		row(U"混雑 平均 {:.0f}% / 最大 {:.0f}%"_fmt(stats.averageCongestion*100,stats.maxCongestion*100),
			stats.maxCongestion < .65 ? textColor : ColorF{1,.73,.35});
		row(U"同じタブを押すと閉じます",mutedColor);
	}
	else
	{
		row(U"街の動き・お知らせ",accentColor);
		for (const auto& text : stats.activeEventSummaries) { if (y < m_details.br().y-48) { row(text,ColorF{1,.79,.45}); } }
		for (const auto& text : stats.notificationSummaries) { if (y < m_details.br().y-48) { row(text,textColor); } }
		if (stats.activeEventSummaries.isEmpty() && stats.notificationSummaries.isEmpty()) { row(U"新しいお知らせはありません",mutedColor); }
	}
}
