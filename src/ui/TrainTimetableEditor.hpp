#pragma once
#include "../railway/RailTimetable.hpp"
#include "../railway/TrainManager.hpp"

/// @brief 適用前のダイヤを保持する。Testと本体で同じ編集・描画を使用する。
class TrainTimetableEditor
{
public:
	static constexpr int kWidth = 570;
	static constexpr int kHeight = 640;
	struct StopRow { int station = -1; TextEditState dwell; };
	struct Action { bool apply = false; Optional<int> locateStation; Optional<int> depotStation; Optional<int> locateDepot; };
	void select(const TrainSchedule& schedule);
	TrainSchedule draft(String& error) const;
	bool apply(TrainNetwork& network);
	Action draw(const Font& font, const Font& bold, const TrainNetwork& network, const TrainManager& trains, GameTime now, bool interactive = true);
	int contentHeight() const { return m_contentHeight; }
	bool dirty() const { return m_dirty; }
	int selectedId() const { return m_original.id; }
	// The fields are the single source of truth for text entry and scripted UI tests.
	TextEditState name, first, last, interval;
	Array<StopRow> stops;
	bool enabled = true;
	TrainType type = TrainType::Local;
	String message;
private:
	TrainSchedule m_original;
	bool m_dirty = false;
	int m_depotView = -1;
	int m_pickerRow = -1, m_pickerPage = 0, m_contentHeight = kHeight;
};
