
#pragma once
#include "time/GameClock.hpp"
#include "world/World.hpp"
#include "road/RoadNetwork.hpp"
#include "traffic/TrafficManager.hpp"
#include "zone/ZoneManager.hpp"
#include "economy/Economy.hpp"
#include "ui/Camera.hpp"
#include "render/WorldRenderer.hpp"
#include "render/RoadRenderer.hpp"
#include "render/VehicleRenderer.hpp"
#include "render/UIRenderer.hpp"
#include "debug/DebugRenderer.hpp"

/// @brief ゲームループ統括クラス
class GameApp
{
public:
	/// @brief ゲームを起動して実行する（メインループ）
	static void run();

private:
	GameApp();

	/// @brief 毎フレームの更新処理
	void update(double dt);

	/// @brief 毎フレームの描画処理
	void render();

	// ---- コアシステム ----
	GameClock        m_clock;
	World            m_world;
	RoadNetwork      m_network;
	GameCamera       m_camera;
	TrafficManager   m_traffic;
	ZoneManager      m_zoneManager;
	Economy          m_economy;

	// ---- レンダラ ----
	Sky              m_sky;
	WorldRenderer    m_worldRenderer;
	RoadRenderer     m_roadRenderer;
	VehicleRenderer  m_vehicleRenderer;
	UIRenderer       m_uiRenderer;
	DebugRenderer    m_debugRenderer;

	// ---- 編集モード ----
	enum class EditMode { None, RoadDraw, ZonePaint };
	EditMode        m_mode          = EditMode::None;

	// 道路描画
	Optional<int>   m_drawStartNode;              ///< 描画開始ノード id
	Optional<Vec3>  m_cursorGroundPos;            ///< カーソルのグラウンド座標

	// ゾーン塗り
	ZoneType        m_paintZone     = ZoneType::Residential;  ///< 選択中のゾーン種別
	Optional<Vec3>  m_rectStart;                  ///< 矩形塗りの始点

	// 月次トリガー管理
	int             m_lastEconYear  = -1;
	int             m_lastEconMonth = -1;

	// ---- 入力処理 ----
	void handleInput();
	void updateCursor();
	void handleRoadDraw();
	void handleZonePaint();

	/// @brief 現在のモードの表示文字列を返す
	String modeString() const;
};
