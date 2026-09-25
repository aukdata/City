#pragma once
#include "../ui/FrameRateGraph.hpp"
#include "../ui/CommandPalette.hpp"
#include "../render/TunnelRenderer.hpp"
#include "../render/SubsurfaceView.hpp"
#include "../ui/RailInfoPanel.hpp"
#include "../ui/StartScreenControls.hpp"
#include "../gen/DistrictHierarchy.hpp"
#include "../render/RiverRenderer.hpp"
#include "../ui/WalkSurface.hpp"
#include <future>
#include <atomic>
#include <mutex>
#include "SceneCommon.hpp"
#include "../ui/KeyboardActions.hpp"
#include "../ui/PanelManager.hpp"
#include "../sim/SimGraph.hpp"
#include "../sim/SimThread.hpp"
#include "../time/GameClock.hpp"
#include "../world/World.hpp"
#include "../gen/MapGenerator.hpp"
#include "../gen/PlaceNameGenerator.hpp"
#include "../road/RoadNetwork.hpp"
#include "../traffic/VehicleManager.hpp"
#include "../pedestrian/PedestrianManager.hpp"
#include "../render/PedestrianRenderer.hpp"
#include "../traffic/DrivingController.hpp"
#include "../audio/SoundEffects.hpp"
#include "../traffic/BusSystem.hpp"
#include "../zone/ZoneManager.hpp"
#include "../economy/Economy.hpp"
#include "../event/EventSystem.hpp"
#include "../scenario/ScenarioSystem.hpp"
#include "../save/SaveResult.hpp"
#include "../ui/Camera.hpp"
#include "../ui/PauseMenu.hpp"
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
#include "../debug/DebugLog.hpp"
#include "../render/CityLighting.hpp"
#include "../render/GpuFrameTimer.hpp"
#include "GuideSignEditor.hpp"
#include "../road/RoadPreset.hpp"
#include "../gen/RoadAutoPlace.hpp"
#include "../road/RoadPlanDraft.hpp"
#include "../ui/RoadPlanToolbar.hpp"
#include "../ui/TrainTimetableEditor.hpp"

class SettlementDevelopment;

/// @brief ゲーム状態を所有し、入力・シミュレーション・描画を協調させるシーン。
/// @details 実装は Loading / Generation / Storage / Capture / 各編集パネルに分割する。
/// 所有権はこのクラスに集約し、ファイル分割のためのグローバル状態や別インスタンスを作らない。
class GameScene : public App::Scene
{
public:
	explicit GameScene(const InitData& init);
	~GameScene();

	/// @brief ロジック更新と描画を行う（非 const レンダラーがあるため描画も update 内で実施）
	void update() override;

	void draw() const override {}

private:
	SoundEffects m_soundEffects;
	void recordPlaytestFrame();
	void processPlaytestCommand();
	int m_playtestCommandId=0;
	DrivingInput m_playtestDrivingInput;
	double m_playtestDrivingUntil=0;
	void prepareFringeCapture(int variant);
	void prepareRoadsideCapture(int variant);
	TextWriter m_playtestFrames;
	int m_playtestFrame = 0;

	// ---- ゲームフェーズ ----
	enum class GamePhase { Loading, Playing };
	enum class LoadingTask { NewGame, LoadGame };
	GamePhase m_phase = GamePhase::Loading;
	LoadingTask m_loadingTask = LoadingTask::NewGame;

	// ---- ローディング管理 ----
	int       m_totalInitChunks  = 0;    ///< 初期チャンク総数
	Stopwatch m_loadingTimer;            ///< 生成/ロード開始からの経過時間
	mutable std::mutex m_loadingTextMutex; ///< 生成ワーカと画面が共有する進捗文字列を保護する
	String    m_loadingStatus;           ///< 現在実行中の処理内容
	String    m_loadingTitle;            ///< ローディング画面のタイトル
	bool      m_loadGameResult = false;  ///< loadGame() の結果（非同期完了後に参照）
	bool      m_loadingFailed = false;
	String    m_loadingError;

	/// @brief バックグラウンド生成/ロードの非同期タスク。
	/// @details Loading 中は通常更新を止め、get() 後にメインスレッドで GPU 資源を準備する。
	/// 破棄時にも完了を待ち、ワーカが参照するワールドの寿命を守る。
	std::future<void> m_generationFuture;

	/// @brief 生成進捗 [0.0, 1.0]（atomic でバックグラウンドスレッドから更新）
	std::atomic<float> m_genProgress{ 0.0f };

	// ---- 地名データベース ----
	PlaceNameDB                     m_placeNames;
	Array<MapGenerator::Settlement> m_districts;      ///< 全地区リスト（種別込み・名称込み）
	Array<Vec2>                     m_castleTownCenters;   ///< 城下町座標キャッシュ（m_districts の派生）

	// ---- コアシステム ----
	GameClock        m_clock;
	World            m_world;
	RoadNetwork      m_network;
	GameCamera       m_camera;
	DrivingController m_driving;
	TimeSpeed m_beforeDrivingSpeed=TimeSpeed::x1;
	double m_drivingNoticeSeconds=0;
	bool handleDrivingShortcuts();
	void leaveDriving(bool overview=false);
	void updateDriving(double dt,bool blocked);
	ZoneManager      m_zoneManager;
	Economy          m_economy;
	CitySnapshot     m_citySnapshot;
	MonthlyEconomyResult m_lastMonthlyEconomy;

	// ---- 車両・経路サービス ----
	VehicleManager   m_vehicleManager;
	BusSystem        m_busSystem;
	SimThread        m_simThread;           ///< 経路計算サービス（メッセージ駆動）
	std::shared_ptr<const SimGraph> m_simGraph;  ///< Main 所有の SimGraph

	// ---- 鉄道システム ----
	TrainNetwork     m_trainNetwork;
	TrainManager     m_trainManager;
	PedestrianManager m_pedestrianManager;
	PedestrianRenderer m_pedestrianRenderer;
	TrainTimetableEditor m_trainTimetableEditor;
	bool m_railTimetableWasVisible = false;

	// ---- イベント ----
	EventSystem      m_eventSystem;
	ScenarioSystem   m_scenarioSystem;
	Array<GameEvent> m_notifications;  ///< 直近の通知（最大5件）

	// ---- レンダリングターゲット（深度バッファ付きテクスチャ）----
	MSRenderTexture  m_renderTexture;

	// ---- 選択オブジェクトのアウトライン描画 ----
	RenderTexture    m_outlineMask;        ///< 選択対象をソリッド白で描画するマスク
	PixelShader      m_outlinePS;          ///< アウトライン抽出 2D PS
	struct OutlineParams
	{
		Float4 texelSize;     ///< xy = 1/w, 1/h
		Float4 outlineColor;  ///< 線色
		Float4 outlineScale;  ///< x = 線幅 [px]
	};
	ConstantBuffer<OutlineParams> m_outlineCB;

	// ---- レンダラ ----
	Sky              m_sky;
	WorldRenderer    m_worldRenderer;
	RiverRenderer m_riverRenderer;
	WalkSurface m_walkSurface;
	TunnelRenderer m_tunnelRenderer;
	SubsurfaceView m_subsurface;
	bool m_underground = false, m_trackingTrain = false;
	Array<Mesh> m_stationSelectionMeshes;
	void toggleUnderground();
	void selectTrain(int id);
	void selectStation(int id);
	void drawRailInfoPanel();
	Optional<int> visibleNodeNear(Vec3 position, float radius) const;
	DistrictHierarchy m_districtHierarchy;
	CityLighting     m_cityLighting;
	GpuFrameTimer    m_gpuTimer;
	RoadRenderer     m_roadRenderer;
	VehicleRenderer  m_vehicleRenderer;
	UIRenderer       m_uiRenderer;
	CityHudStats     m_hudStats;
	int              m_hudStatsRefreshCountdown = 0;
	HousingCapacityCache m_housingCapacity;
	DebugRenderer    m_debugRenderer;
	FrameRateGraph m_frameRateGraph;
	TrainRenderer        m_trainRenderer;
	PlaceNameRenderer    m_placeNameRenderer;
	RoadRouteSignRenderer m_routeSignRenderer;
	MinimapRenderer      m_minimapRenderer;

	// ---- 編集モード ----
	enum class EditMode { None, RoadPlan, RoadDraw, ZonePaint, BusRouteDraw, TerrainEdit, TrainDraw, SandboxEdit };
	EditMode        m_mode          = EditMode::None;

	// 道路描画
	Optional<int>   m_drawStartNode;
	Optional<Vec3>  m_cursorGroundPos;
	RoadEdge        m_drawTemplate;    ///< 設置する道路のテンプレート
	RoadPresetStore m_roadPresets;     ///< 道路テンプレートのプリセット管理
	float           m_drawElevation = 0.0f;  ///< 描画モードの高さオフセット [m]
	Array<int>      m_pendingRouteIds;  ///< 敷設時に新規エッジへ紐付けるルート ID リスト

	// スタート/ゴール指定モード（RoadDraw のサブモード）
	bool            m_autoPlaceMode  = false;   ///< スタート/ゴール指定モードが有効か
	Optional<Vec3>  m_autoPlaceStart;            ///< スタート地点（1 クリック目で記録）

	struct DraftRoadPlan
	{
		RoadPlanDraft editor;
		TextEditState nameEdit;
		TextEditState routeNameEdit;
		Optional<int> routeId;
		bool appendToExistingRoute = false;
		Optional<size_t> draggedPoint;
		Array<Vec3> dragPoints;
		bool snapping = true;
		int preset = 0;
		String message;
		bool error = false;
	};
	DraftRoadPlan m_draftRoadPlan;
	RoadPlanSnapIndex m_roadPlanSnapIndex;
	RoadPlanSnapIndex m_locationIndex;
	bool m_locationIndexDirty=true;
	Optional<RoadPlanSnapIndex::Hit> m_roadPlanCursor;
	Optional<int> m_selectedRoadPlanId;

	// ゾーン塗り
	ZoneType        m_paintZone     = ZoneType::LowResidential;
	int             m_zoneBrushRadius = 1;
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
	Optional<Vec3>     m_sandboxDragNodeStartPos;
	Optional<CtrlDrag> m_sandboxDragCtrl;
	Vec3               m_sandboxPrevCursor;

	int             m_followVehicleIdx = 0;

	// 選択状態（道路・付帯設備を統合）
	enum class SelectionKind
	{
		None,
		Edge,
		Node,
		GuideSign,
		Signal,
		Building,
		LandParcel,
		Train,
		Station
	};
	struct Selection {
		SelectionKind kind = SelectionKind::None;
		int id = -1;
		void clear() { kind = SelectionKind::None; id = -1; }
	};
	Selection m_selection;

	/// @brief 建物の一意識別子（チャンク座標 + ゾーンセル座標）
	struct BuildingRef
	{
		int chunkX = 0;
		int chunkZ = 0;
		int col    = 0;
		int row    = 0;
	};
	Optional<BuildingRef> m_selectedBuilding;
	struct LandParcelRef { Point chunkCoord; int id=-1; };
	Optional<LandParcelRef> m_selectedLandParcel;
	Mesh m_landParcelOutline;
	uint64 m_landParcelRevision=0;
	bool m_landParcelEditing=false;
	int m_landParcelDragVertex=-1;
	void drawLandParcelPanel();
	bool selectLandParcelAt(Vec2 position);
	void refreshLandParcelMesh();
	void handleLandParcelEditInput();
	void renderLandParcelEditHandles();


	/// @brief 道路路線選択（Edge/Node 選択と独立に保持）
	Optional<int> m_selectedRouteId;

	// 既存コードとの互換ヘルパー
	Optional<int> selectedEdgeId() const
	{
		return (m_selection.kind == SelectionKind::Edge) ? Optional<int>{m_selection.id} : none;
	}
	Optional<int> selectedNodeId() const
	{
		return (m_selection.kind == SelectionKind::Node) ? Optional<int>{m_selection.id} : none;
	}
	Optional<int> selectedRouteId() const { return m_selectedRouteId; }
	Optional<int> selectedRoadPlanId() const { return m_selectedRoadPlanId; }
	void selectEdge(int id)   { m_selectedBuilding=none; m_selectedLandParcel=none; m_landParcelOutline=Mesh{}; m_panelManager.hide(U"land_info"); m_selection = { SelectionKind::Edge, id }; }
	void selectNode(int id)   { m_selectedBuilding=none; m_selectedLandParcel=none; m_landParcelOutline=Mesh{}; m_panelManager.hide(U"land_info"); m_selection = { SelectionKind::Node, id }; }
	void selectGuideSign(int id) { m_selectedBuilding=none; m_selectedLandParcel=none; m_landParcelOutline=Mesh{}; m_panelManager.hide(U"land_info"); m_selection = { SelectionKind::GuideSign, id }; }
	void selectSignal(int nodeId) { m_selectedBuilding=none; m_selectedLandParcel=none; m_landParcelOutline=Mesh{}; m_panelManager.hide(U"land_info"); m_selection = { SelectionKind::Signal, nodeId }; }
	void selectBuilding(BuildingRef ref) { m_selectedBuilding=none; m_selectedLandParcel=none; m_landParcelOutline=Mesh{}; m_panelManager.hide(U"land_info"); m_selection = { SelectionKind::Building, 0 }; m_selectedBuilding = ref; }
	void selectRoute(int id)  { m_selectedRouteId = id; }
	void selectRoadPlan(int id) { m_selectedRoadPlanId = id; }
	void clearSelection()
	{
		m_trackingTrain = false;
		m_stationSelectionMeshes.clear();
		m_panelManager.hide(U"rail_info");
		m_selection.clear();
		m_selectedRouteId = none;
		m_selectedRoadPlanId = none;
		m_selectedBuilding = none;
		m_selectedLandParcel = none;
		m_landParcelEditing = false;
		m_landParcelDragVertex = -1;
		m_landParcelOutline = Mesh{};
		m_panelManager.hide(U"land_info");
	}
	void recomputeGuideSignsAroundNode(int nodeId);

	// 車両選択
	Optional<int>   m_selectedVehicleId;
	bool            m_trackingVehicle = false;

	// エッジ 3D 編集ハンドル（Cutoff A/B のドラッグ状態）
	struct EdgeHandleDrag
	{
		enum class Kind {
			None,
			CutoffA, CutoffB,
			PartCenter, PartLeft, PartRight,
			PartAL, PartAR, PartBL, PartBR,
			LaneCenter, LaneLeftSide, LaneRightSide,
			LaneAL, LaneAR, LaneBL, LaneBR,
		};
		Kind  kind     = Kind::None;
		int   edgeId   = -1;
		int   idx      = -1;
		Vec2  anchorScreen{ 0, 0 };
		// 初期値スナップショット（最大 4 値）
		float anchorValue = 0.0f;
		float anchorA = 0.0f;
		float anchorB = 0.0f;
		float anchorC = 0.0f;
	};
	EdgeHandleDrag m_edgeHandleDrag;

	// パネルシステム
	PanelManager    m_panelManager;
	int             m_signalEditPhase = 0;  ///< 信号編集パネルの選択フェーズ

	// ポーズメニュー
	bool            m_showPauseMenu = false;
	PauseMenu m_pauseMenu;
	TimeSpeed m_pauseResumeSpeed=TimeSpeed::Paused;
	void resumeFromPauseMenu();

	// 一時停止トグル用：ポーズ前の速度を記憶する
	TimeSpeed       m_prevSpeed = TimeSpeed::x1;

	// 描画用バッファ（毎フレーム再割り当てを回避）
	Array<Vehicle>  m_renderVehicles;

	// 描画プロファイリング
	double          m_logicMs = 0.0;
	double          m_lockWaitMs = 0.0;
	MainPerfHistory m_mainPerfHistory;
	SimPerfHistory  m_simPerfHistory;

	// ---- 提出用スクリーンショット ----
	Optional<Vec3>  m_benchmarkOrigin;
	Array<double>   m_captureFrameTimes, m_captureCpuTimes, m_captureGpuTimes;
	int             m_captureFrame = 0;
	int             m_captureIndex = 0;
	HashTable<int, Vec3> m_captureVehicleStartPositions;
	bool            m_captureCameraDirty = true;
	bool            m_cityConstraintValidationPassed = true;
	String          m_cityConstraintValidationSummary;

	// 描画タイミング（renderWorld サブメソッド間で共有）
	struct RenderTimings
	{
		double sky = 0, terrain = 0, road = 0, zone = 0;
		double vehicle = 0, train = 0, pedestrian = 0, debug = 0, ui = 0, total = 0;
		// renderScene3D 内訳
		double terrainOnly = 0, roadMesh = 0, signals = 0, routeSigns = 0;
		// render2DUI 内訳
		double uiPlaceNames = 0, uiRouteSigns = 0, uiRenderer = 0;
		double uiMinimap = 0, uiEdgeHandles = 0, uiPanels = 0;
	};
	RenderTimings m_renderTimings;

	/// @brief 時刻から空・太陽パラメータを計算した結果
	struct SkyParams
	{
		float  timeAngle   = 0.0f;   ///< 時角 [rad]
		float  sinTime     = 0.0f;   ///< sin(timeAngle)
		float  dayFactor   = 0.0f;   ///< 昼間度 [0,1]
		float  dawnFactor  = 0.0f;   ///< 黎明/夕暮れ度 [0,1]
		double exposure    = 0.0;    ///< 露出値
	};

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
	/// @brief UnderConstruction エッジの Open 遷移チェック（毎フレーム呼び出し）
	void tickConstruction();
	bool startRoadConstruction(const Array<int>& edgeIds);
	void applyConstructionStart(double cost, const Array<int>& edgeIds, const Array<int>& affectedNodeIds);
	void prepareConstructionSite(const Array<int>& edgeIds);
	RoadConstruction::ClearanceLedger m_clearanceLedger;
	bool m_restoreConstructionSites = false;

	void initScene();
	void initNewGame();
	void initLoadGame();
	void saveGame();
	SaveResult writeGameSnapshot(const FilePath& saveRoot) const;
	SaveResult verifyGameSnapshot(const FilePath& saveRoot) const;
	bool loadGame();
	void addDistricts(const Array<MapGenerator::Settlement>& newDistricts);
	SettlementDevelopment settlementDevelopment();
	void applyZonesGlobal();
	void updateLoading();
	void drawLoadingScreen(float progress);
	void startSimThread();
	/// @brief ローディングフェーズを開始する（共通初期化 + async 起動）
	void startLoadingPhase(LoadingTask task, StringView title, StringView status, std::function<void()> pipeline);
	void setLoadingTitleAndStatus(StringView title, StringView status);
	void setLoadingStatus(StringView status);
	String loadingTitleSnapshot() const;
	String loadingStatusSnapshot() const;

	// ---- バックグラウンド生成パイプライン ----
	void generateAllTerrain();
	void placeAllSettlements();
	void generateAllRoads();
	void generateDistrictRoads();
	void postProcessRoads();
	void placeInitialBuildings(bool preserveLandPatches=false);
	void generateLandPatches(bool preserveExisting = false);
	void migrateLegacyBuildingFrontageReferences();
	bool validateGeneratedCityConstraints();
	void refreshBuildingAnglesFromEdges();
	void updateCaptureCityRenders();
	void updateStreamingBenchmark();
	void updateNavigationBenchmark();
	Vec3 captureFocusPoint() const;
	Vec3 captureStreetCornerPoint(Vec3 fallback) const;
	Vec3 captureIntersectionPoint(Vec3 fallback, int variant) const;
	Vec3 captureCoastalBufferPoint(Vec3 fallback) const;
	Vec3 captureRuralFringePoint(Vec3 fallback) const;
	String captureFileName(int index) const;

	/// @brief m_districts の各地区に対応する最寄り RoadNode を
	///   NamedDestination として RoadNetwork に登録し、案内標識を自動生成する。
	/// @details plan/21_guide_sign_spec.md §2.2 参照
	void registerGuideDestinations();

	/// @brief loadGame 内: 全チャンクの地形データを並列で読み込む（またはフォールバック生成する）
	/// @param saveRoot セーブデータルートパス (e.g. "saves/default")
	/// @param step     呼び出し元の区間ストップウォッチ（完了後に restart される）
	void loadTerrainChunks(const String& saveRoot, Stopwatch& step);

	/// @brief 指定位置に最も近い案内標識 ID を返す（XZ 平面距離、設置位置基準）
	Optional<int> findGuideSignAt(Vec3 pos, float radius) const;

	/// @brief 指定位置に最も近い信号機ノード ID を返す（signalPlacement を持つノードのみ）
	Optional<int> findSignalAt(Vec3 pos, float radius) const;

	/// @brief カメラからのレイに最も近い建物セルを返す（OBB レイキャスト）
	Optional<BuildingRef> findBuildingAt(const Ray& ray);

	/// @brief RoadNetwork 変更後に SimGraph を差分更新して通知する
	void notifyNetworkChanged(const NetworkChangeContext& context)
	{
		m_trainNetwork.synchronize();
		m_pedestrianManager.invalidate();
		Stopwatch step{ StartImmediately::Yes };
		const bool isFastPath = (context.kind == NetworkChangeKind::MovedIntersectionNode);
		const Array<int>& dirtyNodeIds = context.dirtyNodeIds;

		m_routeSignRenderer.invalidate();
		m_locationIndexDirty=true;
		m_trainRenderer.invalidateRoadClearance();
		m_tunnelRenderer.dirty=true;
		m_subsurface.invalidate();
		if (dirtyNodeIds.isEmpty())
		{
			m_worldRenderer.invalidateAllTerrain();
			const double terrainMs = step.msF();
			step.restart();
			// フォールバック: 全再構築
			m_simGraph = std::make_shared<const SimGraph>(SimGraph::build(m_network));
			const double simGraphMs = step.msF();
			step.restart();
			m_minimapRenderer.updateRoadOverlay(m_network, m_world);
			const double minimapMs = step.msF();
			step.restart();
			m_simThread.pushRequest(NetworkUpdate{ m_simGraph, context.kind, dirtyNodeIds });
			const double simThreadMs = step.msF();
			step.restart();
			m_vehicleManager.onNetworkChanged(*m_simGraph, m_network, context);
			const double vehicleMs = step.msF();
			DBG_LOG(U"[NetworkChange] path={} terrain={:.2f}ms simGraph={:.2f}ms minimap={:.2f}ms simThread={:.2f}ms vehicle={:.2f}ms dirtyNodes={}"_fmt(
				isFastPath ? U"fast" : U"generic",
				terrainMs, simGraphMs, minimapMs, simThreadMs, vehicleMs,
				dirtyNodeIds.size()));
			return;
		}

		if (isFastPath && context.movedNodeId >= 0 && context.oldNodePos)
		{
			m_worldRenderer.invalidateTerrainNearMovedNode(
				m_network, context.movedNodeId, *context.oldNodePos);
		}
		else
		{
			m_worldRenderer.invalidateTerrainNearDirtyNodes(m_network, dirtyNodeIds);
		}
		const double terrainMs = step.msF();
		step.restart();

		// 差分更新
		auto sg = std::make_shared<SimGraph>(*m_simGraph);
		sg->updateAround(dirtyNodeIds, m_network);
		m_simGraph = std::move(sg);
		const double simGraphMs = step.msF();
		step.restart();

		m_minimapRenderer.updateRoadOverlayAround(dirtyNodeIds, m_network);
		const double minimapMs = step.msF();
		step.restart();

		m_simThread.pushRequest(NetworkUpdate{ m_simGraph, context.kind, dirtyNodeIds });
		const double simThreadMs = step.msF();
		step.restart();

		m_vehicleManager.onNetworkChanged(*m_simGraph, m_network, context);
		const double vehicleMs = step.msF();

		DBG_LOG(U"[NetworkChange] path={} terrain={:.2f}ms simGraph={:.2f}ms minimap={:.2f}ms simThread={:.2f}ms vehicle={:.2f}ms dirtyNodes={} movedNode={}"_fmt(
			isFastPath ? U"fast" : U"generic",
			terrainMs, simGraphMs, minimapMs, simThreadMs, vehicleMs,
			dirtyNodeIds.size(), context.movedNodeId));
	}

	/// @brief 既存呼び出し向けの GenericEdit 通知
	void notifyNetworkChanged(const Array<int>& dirtyNodeIds = {})
	{
		NetworkChangeContext context;
		context.kind = NetworkChangeKind::GenericEdit;
		context.dirtyNodeIds = dirtyNodeIds;
		notifyNetworkChanged(context);
	}

	// ---- 入力処理 (GameScene_Input.cpp) ----
	void handleInput();
	void handleGlobalShortcuts();
	void updateCursor();
	/// @brief 通常モード (EditMode::None) でのクリック選択処理（車両→ノード→エッジの優先順）
	void handleSelectionClick();
	/// @brief 選択中エッジの 3D ハンドル入力を処理する。クリックを消費したら true
	bool handleEdgeHandleInput();
	/// @brief 選択中エッジの 3D ハンドル（Cutoff A/B）を描画する
	void renderEdgeHandles();
	void handleRoadDraw();
	void updateRoadPlanReview();
	Array<int> m_streetReviewEdges;
	void updateStreetReview();
	void updateTransportReview();
	void updateTransportObjectsReview();
	void jumpToMapPosition(Vec2 target);
	void updateConstructionReview();
	/// @brief スタート/ゴール指定モードの経路探索・敷設を実行する
	void invokeAutoPlace(Vec3 start, Vec3 goal);
	// ---- 道路計画の操作 (GameScene_RoadPlan.cpp) ----
	void handleRoadPlan();
	bool generateDraftRoadPlan();
	bool rebuildDraftRoadPlan();
	void clearDraftRoadPlan();
	bool commitDraftRoadPlan();
	void handleZonePaint();
	void setZonePaintMode(bool enabled);
	void drawZonePalette();
	void setRailTimetableVisible(bool visible);
	void drawRailTimetable();
	void addRailDepot(int station);
	void updateZoneDevelopment(double simulationSeconds);
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
	void prepareVehicleRenderData();
	void renderVehicles();
	void renderSelectionHighlights();
	/// @brief 選択オブジェクトを別レイヤに白塗りで描画 → 2D で外縁のみ合成する
	void renderSelectionOutline();
	void renderEditModeOverlays();
	void render2DUI();
	void executeCommand(StringView input);
	CommandPalette m_commandPalette;
	/// @brief 時刻から空・太陽パラメータを計算する
	SkyParams calcSkyParams() const;
	/// @brief renderWorld のパフォーマンス計測値をリングバッファに記録する
	void pushPerfStats();

	// ---- パネル描画 (GameScene_Panels.cpp) ----
	void drawEdgePanel();
	void drawDrawTemplatePanel();
	void drawRoadPlanPanel();
	void drawNodePanel();
	void drawSignalEditPanel();
	void drawVehiclePanel();
	void drawBuildingPanel();
	void drawNameListPanel();
	void drawRoutePanel();
	void drawPauseMenu();

	/// @brief 道路路線編集パネル用の名前テキスト編集状態
	TextEditState m_routeNameEditState;

	/// @brief 敷設テンプレートパネルの「新規ルート作成」入力状態
	struct NewRouteState
	{
		bool           expanded = false;
		RoadRouteKind  kind     = RoadRouteKind::NationalRoute;
		int            number   = 1;
		TextEditState  nameEdit;
	};
	NewRouteState m_newRouteState;

	/// @brief 案内標識セクションを描画（edge_info パネル内、plan/21_guide_sign_spec.md）
	/// @return 変更があったら true（呼び出し側でキャッシュ無効化）
	bool drawGuideSignSection(class PanelBuilder& ui, RoadEdge& edge);

	/// @brief 案内標識編集パネル（プロパティ表示 + プレビュー）
	void drawGuideSignEditPanel();

	/// @brief 案内標識 WYSIWYG エディタパネル（画面中央・ドラッグ/回転/整列）
	void drawGuideSignEditorPanel();

	/// @brief 案内標識の編集状態とパネル描画を集約
	GuideSignEditor m_guideSignEditor;
};
