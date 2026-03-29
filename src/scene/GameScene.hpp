#pragma once
#include <future>
#include <atomic>
#include "SceneCommon.hpp"
#include "../sim/SimGraph.hpp"
#include "../sim/SimThread.hpp"
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
	Stopwatch m_loadingTimer;            ///< 生成開始からの経過時間
	String    m_loadingStatus;           ///< 現在実行中の処理内容

	/// @brief バックグラウンド生成パイプラインの非同期タスク
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

	// ---- シミュレーションスレッド ----
	SimThread        m_simThread;

	// ---- 鉄道システム ----
	TrainNetwork     m_trainNetwork;
	TrainManager     m_trainManager;

	// ---- イベント通知バッファ ----
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
	struct CtrlDrag { int edgeId; bool isControlPointA; };
	bool               m_sandboxActive     = false;
	Optional<int>      m_sandboxDragNode;
	Optional<CtrlDrag> m_sandboxDragCtrl;
	Vec3               m_sandboxPrevCursor;

	int             m_followVehicleIdx = 0;

	// 道路選択
	Optional<int>   m_selectedEdgeId;
	Optional<int>   m_selectedNodeId;

	// 地名リストパネル
	bool            m_showNameList     = false;
	double          m_nameListScroll   = 0.0;

	// 一時停止トグル用：ポーズ前の速度を記憶する
	TimeSpeed       m_prevSpeed = TimeSpeed::x1;

	// 描画用バッファ（毎フレーム再割り当てを回避）
	Array<Vehicle>  m_renderVehicles;

	// 描画プロファイリング
	double          m_logicMs = 0.0;

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
	void saveGame();
	bool loadGame();
	void addDistricts(const Array<MapGenerator::Settlement>& newDistricts);
	void applyZonesGlobal();
	void updateLoading();
	void drawLoadingScreen(float progress);
	void startSimThread();

	// ---- バックグラウンド生成パイプライン ----
	void generateAllTerrain();
	void placeAllSettlements();
	void generateAllRoads();
	void generateDistrictRoads();
	void postProcessRoads();
	void placeInitialBuildings();

	/// @brief RoadNetwork 変更後に SimGraph を再構築して SimThread に通知する
	void notifyNetworkChanged()
	{
		m_simThread.notifyNetworkChanged(
			std::make_shared<const SimGraph>(SimGraph::build(m_network)));
	}

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
