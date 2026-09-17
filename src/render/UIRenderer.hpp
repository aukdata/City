#pragma once
#include "../ui/CollapsibleHudPanel.hpp"
#include <Siv3D.hpp>
#include "../time/GameClock.hpp"
#include "../economy/Economy.hpp"

/// @brief HUD に表示する街の概況
struct CityHudStats
{
	double monthlyIncome        = 0.0;  ///< 月次収入 [億円/月]
	double monthlyExpense       = 0.0;  ///< 月次支出 [億円/月]
	double monthlyBalance       = 0.0;  ///< 月次収支 [億円/月]
	int64  housingCapacity      = 0;    ///< 住宅収容人数 [人]
	double housingFulfillment   = 0.0;  ///< 住宅充足率（住宅容量 / 人口）
	double averageCongestion    = 0.0;  ///< 車両密度ベースの平均混雑度 [0,1]
	double maxCongestion        = 0.0;  ///< 車両密度ベースの最大混雑度 [0,1]
	int    congestedEdgeCount   = 0;    ///< 混雑閾値を超えた道路区間数
	int    roadEdgeCount        = 0;    ///< 集計対象の道路区間数
	int    observedVehicleCount = 0;    ///< 速度集計対象のアクティブ車両数
	double averageSpeedKmh      = 0.0;  ///< アクティブ車両の平均速度 [km/h]
	double averageSpeedRatio    = 0.0;  ///< 制限速度に対する平均速度比
	int    slowVehicleCount     = 0;    ///< 制限速度比が低いアクティブ車両数
	Array<String> activeEventSummaries;
	Array<String> notificationSummaries;
};

/// @brief 2D HUD の描画クラス
class UIRenderer
{
	CollapsibleHudPanel m_left,m_right;
public:
	void updateLayout(const CityHudStats& stats);
	void handleInput();
	/// @brief 街名ラベルが実際の開閉状態のHUDへ重ならないための領域。
	Array<RectF> panelBounds() const { return {m_left.bounds(), m_right.bounds()}; }
	[[nodiscard]] bool isMouseOnHud() const { return m_left.bounds().mouseOver() || m_right.bounds().mouseOver(); }
	[[nodiscard]] Optional<RectF> minimapBounds() const
	{
		if (m_right.collapsed) { return none; }
		return RectF{m_right.expanded.x+m_right.expanded.w-210,m_right.expanded.y+m_right.expanded.h-210,200,200};
	}

	/// @brief HUD を描画する（毎フレーム呼ぶ）
	/// @param clock        ゲーム時計
	/// @param vehicleCount 現在の車両数
	/// @param modeText     現在の編集モード文字列
	/// @param economy      経済状態
	/// @param stats        HUD 用に集計済みの街の概況
	void render(const GameClock& clock, int vehicleCount, StringView modeText,
	            const Economy& economy, const CityHudStats& stats, bool walking = false, bool driving = false);
};
