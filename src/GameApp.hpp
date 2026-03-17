
#pragma once
#include "time/GameClock.hpp"
#include "world/World.hpp"
#include "road/RoadNetwork.hpp"
#include "traffic/TrafficManager.hpp"
#include "zone/ZoneManager.hpp"
#include "economy/Economy.hpp"
#include "event/EventSystem.hpp"
#include "ui/Camera.hpp"
#include "render/WorldRenderer.hpp"
#include "render/RoadRenderer.hpp"
#include "render/VehicleRenderer.hpp"
#include "render/UIRenderer.hpp"
#include "debug/DebugRenderer.hpp"
#include "railway/TrainNetwork.hpp"
#include "railway/TrainManager.hpp"
#include "render/TrainRenderer.hpp"

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
	EventSystem      m_eventSystem;

	// ---- 鉄道システム ----
	TrainNetwork     m_trainNetwork;
	TrainManager     m_trainManager;

	// ---- イベント通知バッファ ----
	Array<GameEvent> m_notifications;  ///< 直近の通知（最大5件）

	// ---- レンダラ ----
	Sky              m_sky;
	WorldRenderer    m_worldRenderer;
	RoadRenderer     m_roadRenderer;
	VehicleRenderer  m_vehicleRenderer;
	UIRenderer       m_uiRenderer;
	DebugRenderer    m_debugRenderer;
	TrainRenderer    m_trainRenderer;

	// ---- 編集モード ----
	enum class EditMode { None, RoadDraw, ZonePaint, BusRouteDraw, TerrainEdit, TrainDraw };
	EditMode        m_mode          = EditMode::None;

	// 道路描画
	Optional<int>   m_drawStartNode;              ///< 描画開始ノード id
	Optional<Vec3>  m_cursorGroundPos;            ///< カーソルのグラウンド座標

	// ゾーン塗り
	ZoneType        m_paintZone     = ZoneType::Residential;  ///< 選択中のゾーン種別
	Optional<Vec3>  m_rectStart;                  ///< 矩形塗りの始点

	// バス路線描画
	int             m_editingRouteId = -1;   ///< 編集中のバス路線 id（-1 = なし）

	// 線路描画
	Optional<int>   m_trainDrawStartNode;    ///< 線路描画の開始ノード id

	// 地形編集
	float           m_terrainBrushRadius   = 80.0f;   ///< ブラシ半径 [m]
	float           m_terrainBrushStrength = 2.0f;    ///< ブラシ強度 [m/frame]

	// 月次トリガー管理
	int             m_lastEconYear  = -1;
	int             m_lastEconMonth = -1;
	int             m_followVehicleIdx = 0;   ///< 追従対象の車両インデックス

	// ---- 入力処理 ----
	void handleInput();
	void updateCursor();
	void handleRoadDraw();
	void handleZonePaint();
	void handleBusRouteDraw();
	void handleTerrainEdit();
	void handleTrainDraw();

	/// @brief 現在のモードの表示文字列を返す
	String modeString() const;
};
