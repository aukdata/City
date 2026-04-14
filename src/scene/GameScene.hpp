#pragma once
#include <future>
#include <atomic>
#include "SceneCommon.hpp"
#include "../ui/PanelManager.hpp"
#include "../sim/SimGraph.hpp"
#include "../sim/SimThread.hpp"
#include "../time/GameClock.hpp"
#include "../world/World.hpp"
#include "../gen/MapGenerator.hpp"
#include "../gen/PlaceNameGenerator.hpp"
#include "../road/RoadNetwork.hpp"
#include "../traffic/VehicleManager.hpp"
#include "../traffic/BusRoute.hpp"
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
#include "../render/RoadRouteSignRenderer.hpp"
#include "../render/MinimapRenderer.hpp"

/// @brief ゲームプレイシーン
class GameScene : public App::Scene
{
public:
	explicit GameScene(const InitData& init);
	~GameScene();

	/// @brief ロジック更新と描画を行う（非 const レンダラーがあるため描画も update 内で実施）
	void update() override;

	void draw() const override {}

private:
	// ---- ゲームフェーズ ----
	enum class GamePhase { Loading, Playing };
	GamePhase m_phase = GamePhase::Loading;

	// ---- ローディング管理 ----
	int       m_totalInitChunks  = 0;    ///< 初期チャンク総数
	Stopwatch m_loadingTimer;            ///< 生成/ロード開始からの経過時間
	String    m_loadingStatus;           ///< 現在実行中の処理内容
	String    m_loadingTitle;            ///< ローディング画面のタイトル
	bool      m_loadGameResult = false;  ///< loadGame() の結果（非同期完了後に参照）

	/// @brief バックグラウンド生成/ロードの非同期タスク
	std::future<void> m_generationFuture;

	/// @brief 生成進捗 [0.0, 1.0]（atomic でバックグラウンドスレッドから更新）
	std::atomic<float> m_genProgress{ 0.0f };

	// ---- 地名データベース ----
	PlaceNameDB                     m_placeNames;
	Array<MapGenerator::Settlement> m_districts;      ///< 全地区リスト（種別込み・名称込み）
	Array<Vec2>                     m_urbanCenters;   ///< Urban 地区座標キャッシュ（m_districts の派生）

	// ---- コアシステム ----
	GameClock        m_clock;
	World            m_world;
	RoadNetwork      m_network;
	GameCamera       m_camera;
	ZoneManager      m_zoneManager;
	Economy          m_economy;

	// ---- 車両・経路サービス ----
	VehicleManager   m_vehicleManager;
	SimThread        m_simThread;           ///< 経路計算サービス（メッセージ駆動）
	std::shared_ptr<const SimGraph> m_simGraph;  ///< Main 所有の SimGraph

	// ---- 鉄道システム ----
	TrainNetwork     m_trainNetwork;
	TrainManager     m_trainManager;

	// ---- イベント ----
	EventSystem      m_eventSystem;
	Array<GameEvent> m_notifications;  ///< 直近の通知（最大5件）

	// ---- レンダリングターゲット（深度バッファ付きテクスチャ）----
	RenderTexture    m_renderTexture;

	// ---- レンダラ ----
	Sky              m_sky;
	WorldRenderer    m_worldRenderer;
	RoadRenderer     m_roadRenderer;
	VehicleRenderer  m_vehicleRenderer;
	UIRenderer       m_uiRenderer;
	DebugRenderer    m_debugRenderer;
	TrainRenderer        m_trainRenderer;
	PlaceNameRenderer    m_placeNameRenderer;
	RoadRouteSignRenderer m_routeSignRenderer;
	MinimapRenderer      m_minimapRenderer;

	// ---- 編集モード ----
	enum class EditMode { None, RoadDraw, ZonePaint, BusRouteDraw, TerrainEdit, TrainDraw, SandboxEdit };
	EditMode        m_mode          = EditMode::None;

	// 道路描画
	Optional<int>   m_drawStartNode;
	Optional<Vec3>  m_cursorGroundPos;
	RoadEdge        m_drawTemplate;    ///< 設置する道路のテンプレート
	float           m_drawElevation = 0.0f;  ///< 描画モードの高さオフセット [m]

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
	struct CtrlDrag { int edgeId; bool isControlPointA; };
	bool               m_sandboxActive     = false;
	Optional<int>      m_sandboxDragNode;
	Optional<CtrlDrag> m_sandboxDragCtrl;
	Vec3               m_sandboxPrevCursor;

	int             m_followVehicleIdx = 0;

	// 道路選択
	Optional<int>   m_selectedEdgeId;
	Optional<int>   m_selectedNodeId;

	// 車両選択
	Optional<int>   m_selectedVehicleId;
	bool            m_trackingVehicle = false;

	// パネルシステム
	PanelManager    m_panelManager;
	int             m_signalEditPhase = 0;  ///< 信号編集パネルの選択フェーズ

	// ポーズメニュー
	bool            m_showPauseMenu = false;

	// 一時停止トグル用：ポーズ前の速度を記憶する
	TimeSpeed       m_prevSpeed = TimeSpeed::x1;

	// 描画用バッファ（毎フレーム再割り当てを回避）
	Array<Vehicle>  m_renderVehicles;

	// 描画プロファイリング
	double          m_logicMs = 0.0;
	double          m_lockWaitMs = 0.0;
	MainPerfHistory m_mainPerfHistory;
	SimPerfHistory  m_simPerfHistory;

	// 描画タイミング（renderWorld サブメソッド間で���有）
	struct RenderTimings
	{
		double sky = 0, terrain = 0, road = 0, zone = 0;
		double vehicle = 0, train = 0, debug = 0, ui = 0, total = 0;
	};
	RenderTimings m_renderTimings;

	// ---- ユーティリティ ----

	/// @brief ワールド座標 → チャンク座標
	static Point worldPosToChunk(Vec3 pos)
	{
		constexpr float kChunkM = 1024.0f;
		const auto fd = [](float v) -> int
		{
			const int q = static_cast<int>(v / kChunkM);
			return (v < 0.0f && v != static_cast<float>(q) * kChunkM) ? q - 1 : q;
		};
		return Point{ fd(static_cast<float>(pos.x)), fd(static_cast<float>(pos.z)) };
	}

	// ---- 内部メソッド ----
	void initScene();
	void initNewGame();
	void initLoadGame();
	void saveGame();
	bool loadGame();
	void addDistricts(const Array<MapGenerator::Settlement>& newDistricts);
	void applyZonesGlobal();
	void updateLoading();
	void drawLoadingScreen(float progress);
	void startSimThread();
	/// @brief ローディングフェーズを開始する（共通初期化 + async 起動）
	void startLoadingPhase(StringView title, StringView status, std::function<void()> pipeline);

	// ---- バックグラウンド生成パイプライン ----
	void generateAllTerrain();
	void placeAllSettlements();
	void generateAllRoads();
	void generateDistrictRoads();
	void postProcessRoads();
	void placeInitialBuildings();

	/// @brief RoadNetwork 変更後に SimGraph を差分更新して通知する
	/// @param dirtyNodeIds 変更されたノードの ID リスト（空なら全再構築）
	void notifyNetworkChanged(const Array<int>& dirtyNodeIds = {})
	{
		if (dirtyNodeIds.isEmpty())
		{
			// フォールバック: 全再構築
			m_simGraph = std::make_shared<const SimGraph>(SimGraph::build(m_network));
			m_minimapRenderer.updateRoadOverlay(m_network, m_world);
		}
		else
		{
			// 差分更新
			auto sg = std::make_shared<SimGraph>(*m_simGraph);
			sg->updateAround(dirtyNodeIds, m_network);
			m_simGraph = std::move(sg);
			m_minimapRenderer.updateRoadOverlayAround(dirtyNodeIds, m_network);
		}
		m_simThread.pushRequest(NetworkUpdate{ m_simGraph });
		m_vehicleManager.onNetworkChanged(*m_simGraph, m_network);
	}

	// ---- 入力処理 (GameScene_Input.cpp) ----
	void handleInput();
	void updateCursor();
	void handleRoadDraw();
	void handleZonePaint();
	void handleBusRouteDraw();
	void handleTerrainEdit();
	void handleTrainDraw();
	void handleSandboxEdit();
	String modeString() const;
	/// @brief 道路描画用の制御点を自動計算する
	std::pair<Vec3, Vec3> calcRoadDrawControlPoints(int startNodeId, Vec3 endPos) const;
	/// @brief 高架面とのレイ交差でノード/エッジを検索する
	struct ElevatedHitResult { Optional<int> nodeId; Optional<int> edgeId; };
	ElevatedHitResult raycastElevated(Vec2 screenPos) const;
	/// @brief パネルを画面右端に表示するための位置を返す
	Vec2 panelRightPos(StringView panelId) const;

	// ---- 描画 (GameScene_Render.cpp) ----
	void renderWorld();
	void renderScene3D();
	void renderVehicles();
	void renderSelectionHighlights();
	void renderEditModeOverlays();
	void render2DUI();

	// ---- パネル描画 (GameScene_Panels.cpp) ----
	void drawEdgePanel();
	void drawDrawTemplatePanel();
	void drawNodePanel();
	void drawSignalEditPanel();
	void drawVehiclePanel();
	void drawNameListPanel();
	void drawPauseMenu();
};
