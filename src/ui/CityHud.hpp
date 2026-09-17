#pragma once
#include <Siv3D.hpp>

struct CityHudStats;
struct GameClock;
struct Economy;

/// @brief 常時表示する情報と、必要なときだけ開く詳細を分ける街のHUD。
/// @details 地図の位置は通知件数に依存させない。描画・クリック・街名の回避に同じ領域を使う。
class CityHud
{
public:
	enum class Tab { None, City, Traffic, Notices };
	enum class Action { None, TogglePause, NextSpeed };

	void updateLayout(Size size, bool walking = false, bool driving = false, bool hasMode = false);
	void draw(const GameClock& clock, int vehicleCount, StringView mode, const Economy& economy,
		const CityHudStats& stats, const Font& font) const;
	/// @brief 描画と同じ配置で入力する。無効時も前フレームの消費状態を解除する。
	Action interact(Vec2 point, bool clicked, bool enabled = true);
	[[nodiscard]] bool blocksMouse(Vec2 point) const;
	[[nodiscard]] Array<RectF> bounds() const;
	[[nodiscard]] Optional<RectF> minimapBounds() const;
	[[nodiscard]] RectF summaryBounds() const { return m_summary; }
	[[nodiscard]] RectF tabBounds(Tab tab) const;
	[[nodiscard]] RectF pauseBounds() const;
	[[nodiscard]] RectF speedBounds() const;
	[[nodiscard]] RectF mapToggleBounds() const { return m_mapHeader; }
	[[nodiscard]] RectF helpButtonBounds() const { return m_helpButton; }
	[[nodiscard]] Tab activeTab() const { return m_tab; }
	[[nodiscard]] bool helpVisible() const { return m_helpOpen; }

private:
	Size m_size{1280,768};
	RectF m_summary, m_details, m_mapHeader, m_helpButton, m_helpArea, m_mode;
	bool m_walking = false, m_driving = false, m_hasMode = false;
	bool m_mapOpen = true, m_helpOpen = false, m_consumed = false;
	Tab m_tab = Tab::None;
};
