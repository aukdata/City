#pragma once
#include "SceneCommon.hpp"
#include "../time/GameClock.hpp"
#include "../world/World.hpp"
#include "../gen/MapGenerator.hpp"
#include "../gen/PlaceNameGenerator.hpp"
#include "../road/RoadNetwork.hpp"
#include "../traffic/TrafficManager.hpp"
#include "../zone/ZoneManager.hpp"
#include "../economy/Economy.hpp"
#include "../event/EventSystem.hpp"
#include "../ui/Camera.hpp"
#include "../render/WorldRenderer.hpp"
#include "../render/RoadRenderer.hpp"
#include "../render/VehicleRenderer.hpp"
#include "../render/UIRenderer.hpp"
#include "../debug/DebugRenderer.hpp"
#include "../railway/TrainNetwork.hpp"
#include "../railway/TrainManager.hpp"
#include "../render/TrainRenderer.hpp"
#include "../render/PlaceNameRenderer.hpp"

/// @brief ゲームプレイシーン
class GameScene : public App::Scene
{
public:
	explicit GameScene(const InitData& init);

	/// @brief ロジック更新と描画を行う（非 const レンダラーがあるため描画も update 内で実施）
	void update() override;

	void draw() const override {}

private:
	// ---- 地名データベース ----
	PlaceNameDB                     m_placeNames;
	Array<MapGenerator::Settlement> m_settlements;

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

	// ---- レンダリングターゲット（深度バッファ付き MSAA テクスチャ）----
	MSRenderTexture  m_renderTexture;

	// ---- レンダラ ----
	Sky              m_sky;
	WorldRenderer    m_worldRenderer;
	RoadRenderer     m_roadRenderer;
	VehicleRenderer  m_vehicleRenderer;
	UIRenderer       m_uiRenderer;
	DebugRenderer    m_debugRenderer;
	TrainRenderer        m_trainRenderer;
	PlaceNameRenderer    m_placeNameRenderer;

	// ---- 編集モード ----
	enum class EditMode { None, RoadDraw, ZonePaint, BusRouteDraw, TerrainEdit, TrainDraw, SandboxEdit };
	EditMode        m_mode          = EditMode::None;

	// 道路描画
	Optional<int>   m_drawStartNode;
	Optional<Vec3>  m_cursorGroundPos;

	// ゾーン塗り
	ZoneType        m_paintZone     = ZoneType::Residential;
	Optional<Vec3>  m_rectStart;

	// バス路線描画
	int             m_editingRouteId = -1;

	// 線路描画
	Optional<int>   m_trainDrawStartNode;

	// 地形編集
	float           m_terrainBrushRadius   = 80.0f;
	float           m_terrainBrushStrength = 20.0f;

	// サンドボックス編集
	struct CtrlDrag { int edgeId; bool isA; };     ///< ドラッグ中の制御点
	bool               m_sandboxActive     = false;
	Optional<int>      m_sandboxDragNode;          ///< ドラッグ中のノード id
	Optional<CtrlDrag> m_sandboxDragCtrl;          ///< ドラッグ中の制御点
	Vec3               m_sandboxPrevCursor;        ///< 前フレームのカーソル地面位置

	// 月次トリガー管理
	int             m_lastEconYear  = -1;
	int             m_lastEconMonth = -1;
	int             m_followVehicleIdx = 0;

	// 一時停止トグル用：ポーズ前の速度を記憶する
	TimeSpeed       m_prevSpeed = TimeSpeed::x1;

	// ---- 無限ワールド: リージョン生成管理 ----
	HashTable<int64, bool> m_generatedRegions;   ///< 生成済みリージョンのキー集合

	/// @brief チャンク座標をリージョン座標に変換する（負座標対応の floor 除算）
	static Point chunkToRegion(Point chunk)
	{
		constexpr int kRC = 10;  // 1リージョン = 10チャンク
		auto floorDiv = [](int a, int b) -> int {
			return a / b - (a % b != 0 && (a ^ b) < 0 ? 1 : 0);
		};
		return { floorDiv(chunk.x, kRC), floorDiv(chunk.y, kRC) };
	}

	/// @brief リージョン座標 → ワールドオフセット [m]
	static Vec2 regionToWorldOffset(Point region)
	{
		constexpr float kRegionM = 10 * 1024.0f;
		return Vec2{ region.x * kRegionM, region.y * kRegionM };
	}

	/// @brief リージョン座標をハッシュキーに変換する
	static int64 regionKey(Point p)
	{
		return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
	}

	// ---- 内部メソッド ----
	void initWorld();
	void checkAndGenerateRegions();
	void handleInput();
	void updateCursor();
	void handleRoadDraw();
	void handleZonePaint();
	void handleBusRouteDraw();
	void handleTerrainEdit();
	void handleTrainDraw();
	void handleSandboxEdit();
	void renderWorld();

	String modeString() const;
};
