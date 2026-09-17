#include "TrainTimetableEditor.hpp"
#include "PanelWidget.hpp"

namespace
{
	String fit(const Font& font, String text, double width)
	{
		if (font(text).region().w <= width) { return text; }
		while (!text.isEmpty() && font(text + U"…").region().w > width) { text.pop_back(); }
		return text + U"…";
	}
}
void TrainTimetableEditor::select(const TrainSchedule& schedule)
{
	GameInput::releaseTextFocus();
	m_original = schedule;
	name = TextEditState{schedule.name}; first = TextEditState{RailTimetable::formatTime(schedule.firstDepartureMinute)};
	last = TextEditState{RailTimetable::formatTime(schedule.lastDepartureMinute)}; interval = TextEditState{Format(schedule.headwaySec)};
	enabled = schedule.enabled; type = schedule.type; stops.clear();
	for (const auto& stop : schedule.stops) { stops << StopRow{stop.stationNodeId, TextEditState{Format(stop.dwellSec)}}; }
	m_dirty = false; m_pickerRow = -1; message.clear();
}
TrainSchedule TrainTimetableEditor::draft(String& error) const
{
	TrainSchedule result = m_original; error.clear();
	result.name = name.text.trimmed(); result.enabled = enabled; result.type = type;
	const auto begin = RailTimetable::parseTime(first.text), end = RailTimetable::parseTime(last.text);
	const auto spacing = ParseOpt<float>(interval.text);
	if (result.name.isEmpty()) { error = U"路線名を入力してください"; }
	else if (!begin || !end) { error = U"時刻は08:00のように入力してください"; }
	else if (!spacing || !std::isfinite(*spacing)) { error = U"運行間隔を数字で入力してください"; }
	if (!error.isEmpty()) { return result; }
	result.firstDepartureMinute = *begin; result.lastDepartureMinute = *end; result.headwaySec = *spacing;
	result.stops.clear();
	for (const auto& row : stops)
	{
		const auto dwell = ParseOpt<float>(row.dwell.text);
		if (!dwell || !std::isfinite(*dwell)) { error = U"停車時間を数字で入力してください"; return result; }
		result.stops << StopEntry{row.station, *dwell};
	}
	return result;
}
bool TrainTimetableEditor::apply(TrainNetwork& network)
{
	auto schedule = draft(message);
	if (!message.isEmpty()) { return false; }
	if (!network.applySchedule(schedule, message)) { return false; }
	const auto* saved = schedule.id < 0 ? &network.schedules().back() : network.getSchedule(schedule.id);
	select(*saved); message = U"適用しました。走行中の列車は今の便を続けます";
	return true;
}
TrainTimetableEditor::Action TrainTimetableEditor::draw(const Font& font, const Font& bold,
	const TrainNetwork& network, const TrainManager& trains, GameTime now, bool interactive)
{
	using namespace PanelWidget;
	const auto button = [&](const Font& face, StringView label, bool active, int x, int y, int width, int height)
	{
		return PanelWidget::button(face,label,active,x,y,width,height) && interactive;
	};
	const auto textInput = [&](const Font& face, TextEditState& state, int x, int y, int width, int height, size_t limit)
	{
		if (interactive) { return PanelWidget::textInput(face,state,x,y,width,height,limit); }
		Rect{x,y,width,height}.draw(ColorF{.10,.10,.16}); face(state.text).draw(x+3,y+1,ColorF{.9}); return false;
	};
	const auto toggle = [&](const Font& face, StringView on, StringView off, bool& value, int x, int y, int width, int height)
	{
		if (button(face,value ? on : off,value,x,y,width,height)) { value=!value; return true; }
		return false;
	};
	Action action; constexpr int kRow = 30, kRight = kWidth - 18;
	const ColorF text{.9}, muted{.65,.73,.78}, accent{.60,.87,.89};
	int index = -1;
	for (size_t i = 0; i < network.schedules().size(); ++i) { if (network.schedules()[i].id == m_original.id) { index = static_cast<int>(i); } }
	bold(U"鉄道ダイヤ").draw(8, 4, accent);
	font(U"{} / {}路線"_fmt(index + 1, network.schedules().size())).draw(110, 4, muted);
	int direction = 0;
	if (button(font, U"前の路線", false, 264, 2, 92, 25)) { direction = -1; }
	if (button(font, U"次の路線", false, 364, 2, 92, 25)) { direction = 1; }
	if (direction != 0 && !network.schedules().isEmpty())
	{
		if (m_dirty) { message = U"先に変更を適用するか、取消してください"; }
		else { select(network.schedules()[(Max(0, index) + static_cast<int>(network.schedules().size()) + direction) % network.schedules().size()]); }
	}
	if (button(font, U"新規", false, 464, 2, 80, 25))
	{
		if (m_dirty) { message = U"先に変更を適用するか、取消してください"; }
		else { TrainSchedule schedule; schedule.name = U"新しい路線"; select(schedule); stops = {{-1, TextEditState{U"2"}}, {-1, TextEditState{U"2"}}}; m_dirty = true; }
	}
	label(font, U"路線名", 8, 38, muted); m_dirty |= textInput(font, name, 80, 35, 460, 26, 28);
	m_dirty |= toggle(font, U"運行する", U"運休", enabled, 8, 72, 112, 26);
	if (button(font, type == TrainType::Local ? U"普通 / 2両" : U"快速 / 4両", false, 130, 72, 136, 26))
	{
		type = type == TrainType::Local ? TrainType::Express : TrainType::Local; m_dirty = true;
	}
	label(font, U"両端から交互に発車・毎日繰り返し", 280, 77, muted);
	label(font, U"始発", 8, 112, muted); m_dirty |= textInput(font, first, 48, 108, 80, 26, 5);
	label(font, U"終発", 144, 112, muted); m_dirty |= textInput(font, last, 186, 108, 80, 26, 5);
	label(font, U"間隔", 286, 112, muted); m_dirty |= textInput(font, interval, 328, 108, 70, 26, 4);
	label(font, U"分（ゲーム内）", 408, 112, muted);
	label(font, U"現実24分で1日。終発が早い場合は翌日です", 8, 145, muted);
	bold(U"停車駅と停車時間（ゲーム内の分）").draw(8, 176, accent);
	int y = 204;
	Array<int> stations;
	for (const auto& node : network.nodes()) { if (node.type == TrackNodeType::Station) { stations << node.id; } }
	for (int row = 0; row < static_cast<int>(stops.size()); ++row)
	{
		auto& stop = stops[row]; const auto* station = network.getNode(stop.station);
		label(font, U"{:02}"_fmt(row + 1), 8, y + 4, muted);
		if (button(font, fit(font, station ? station->name : U"駅を選ぶ", 230), m_pickerRow == row, 42, y, 244, 25))
		{
			m_pickerRow = m_pickerRow == row ? -1 : row; m_pickerPage = 0;
		}
		m_dirty |= textInput(font, stop.dwell, 296, y, 46, 25, 3);
		if (button(font, U"↑", false, 352, y, 32, 25) && row > 0)
		{
			GameInput::releaseTextFocus(); std::swap(stops[row], stops[row - 1]); m_dirty = true; m_pickerRow = -1;
		}
		if (button(font, U"↓", false, 390, y, 32, 25) && row + 1 < static_cast<int>(stops.size()))
		{
			GameInput::releaseTextFocus(); std::swap(stops[row], stops[row + 1]); m_dirty = true; m_pickerRow = -1;
		}
		if (button(font, U"地図", false, 428, y, 48, 25) && station) { action.locateStation = station->id; }
		if (button(font, U"削除", false, 484, y, 56, 25))
		{
			GameInput::releaseTextFocus(); stops.remove_at(row); m_dirty = true; m_pickerRow = -1; --row; y += kRow; continue;
		}
		y += kRow;
		if (m_pickerRow == row)
		{
			constexpr int kPageSize = 6;
			const int pages = Max(1, static_cast<int>((stations.size() + kPageSize - 1) / kPageSize));
			if (button(font, U"前の駅", false, 42, y, 88, 24)) { m_pickerPage = (m_pickerPage + pages - 1) % pages; }
			if (button(font, U"次の駅", false, 140, y, 88, 24)) { m_pickerPage = (m_pickerPage + 1) % pages; }
			label(font, U"{} / {}ページ"_fmt(m_pickerPage + 1, pages), 244, y + 3, muted); y += 28;
			for (int item = m_pickerPage * kPageSize; item < Min(static_cast<int>(stations.size()), (m_pickerPage + 1) * kPageSize); ++item)
			{
				const auto* choice = network.getNode(stations[item]);
				if (button(font, fit(font, choice->name, 450), stop.station == choice->id, 42, y, 498, 24))
				{
					stop.station = choice->id; m_dirty = true; m_pickerRow = -1;
				}
				y += 27;
			}
		}
	}
	if (button(font, U"停車駅を追加", false, 42, y, 160, 26) && stops.size() < 12)
	{
		GameInput::releaseTextFocus(); stops << StopRow{-1, TextEditState{U"2"}}; m_dirty = true;
	}
	y += 38;
	String error; auto schedule = draft(error);
	if (error.isEmpty()) { error = RailTimetable::validate(network, schedule); }
	if (!error.isEmpty()) { font(fit(font, error, kRight)).draw(8, y, ColorF{1,.65,.53}); y += 28; }
	else
	{
		font(U"片道の目安 {}時間{}分"_fmt(static_cast<int>(RailTimetable::journeySeconds(network, schedule)) / 60,
			static_cast<int>(RailTimetable::journeySeconds(network, schedule)) % 60)).draw(8, y, muted); y += 26;
		String times;
		for (const auto departure : RailTimetable::departures(schedule, now))
		{
			times += RailTimetable::formatTime(static_cast<int>(GameClock::calendarMinuteFromTime(departure))) + U"  ";
		}
		font(schedule.enabled ? U"発車予定  " + times : U"運休中：新しい列車は発車しません").draw(8, y, text); y += 26;
		if (schedule.headwaySec < RailTimetable::journeySeconds(network, schedule))
		{
			font(U"間隔が短い場合、同じ軌道の空きを待ちます").draw(8, y, ColorF{1,.80,.47}); y += 26;
		}
	}
	int running = 0;
	for (const auto& train : trains.trains()) { if (train.scheduleId == m_original.id) { ++running; } }
	const auto* live = network.getSchedule(m_original.id);
	const bool pending = live && RailTimetable::dueDeparture(*live, now).has_value();
	font(U"走行中 {}編成  /  {}"_fmt(running, pending ? U"発車待ち（線路の空きを確認）" : U"次の発車時刻まで待機")).draw(8, y, muted); y += 34;
	if (button(font, U"変更を適用", true, 8, y, 134, 28)) { action.apply = true; }
	if (button(font, U"取消", false, 152, y, 76, 28))
	{
		select(m_original.id < 0 && !network.schedules().isEmpty() ? network.schedules().front() : m_original);
	}
	if (!stops.isEmpty() && button(font, U"始発駅に車庫", false, 242, y, 136, 28)) { action.depotStation = stops.front().station; }
	if (!network.depots().isEmpty() && button(font, U"車庫を見る", false, 390, y, 150, 28))
	{
		m_depotView = (m_depotView+1) % static_cast<int>(network.depots().size()); action.locateDepot=m_depotView;
	}
	y += 37;
	if (!message.isEmpty()) { font(fit(font, message, kRight)).draw(8, y, accent); y += 27; }
	font(U"適用後の次便から反映。保存はCtrl+Shift+S").draw(8, y, muted); y += 26;
	font(U"H：開閉  ・  駅の並べ替えは↑↓  ・  地図で駅へ移動").draw(8, y, muted);
	m_contentHeight = y + 30;
	return action;
}
