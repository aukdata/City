#pragma once
#include <future>
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
	enum class GamePhase { Loading, PostProcess, Playing };
	GamePhase m_phase = GamePhase::Loading;

	// ---- ローディング管理 ----
	int       m_totalInitChunks  = 0;    ///< 初期チャンク総数
	Stopwatch m_loadingTimer;            ///< 生成開始からの経過時間
	String    m_loadingStatus;           ///< 現在実行中の処理内容

	/// @brief ポスト処理の非同期タスク
	std::future<void> m_postProcessFuture;

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
	struct CtrlDrag { int edgeId; bool isControlPointA; };     ///< ドラッグ中の制御点
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

	// 描画用バッファ（毎フレーム再割り当てを回避）
	Array<Vehicle>  m_renderVehicles;

	// 描画プロファイリング
	double          m_logicMs = 0.0;

	// ---- バックグラウンドチャンク生成 ----

	/// @brief バックグラウンド実行中のチャンク構築タスク
	struct ChunkBuildTask
	{
		Point                                          chunkCoord;
		std::future<MapGenerator::ChunkBuildResult>    future;
	};

	Array<ChunkBuildTask> m_chunkTasks;
	HashSet<int64>        m_pendingChunkKeys;              ///< 投入済みキー（二重投入防止）
	static constexpr int  kMaxMergePerFrame = 1;         ///< 1フレームあたりの最大統合数（Playing時）
	static constexpr int  kMaxChunkTasks    = 4;         ///< 同時バックグラウンドタスク数
	static constexpr int  kInitRange        = 5;         ///< 初期生成半径 (11x11)

	// ---- 無限ワールド: チャンクデータ管理 ----

	/// @brief チャンク内オブジェクトの永続データ（セーブ/ロード単位）
	struct ChunkData
	{
		Point                           chunkCoord;  ///< このチャンクの座標
		Array<RoadNode>                 nodes;       ///< 保有ノード（境界ノード重複あり）
		Array<RoadEdge>                 edges;       ///< 保有エッジ（nodeA 座標で帰属決定）
		Array<MapGenerator::Settlement> districts;   ///< 地区リスト
	};

	/// @brief チャンクキー → ChunkData（永続層）。キーの存在 = 生成済み
	HashTable<int64, ChunkData> m_chunkStore;

	/// @brief 現在ネットワークにロード済みのチャンクキー集合
	HashSet<int64> m_loadedChunkKeys;

	/// @brief リージョン座標 → ワールドオフセット [m]
	static Vec2 regionToWorldOffset(Point region)
	{
		constexpr float kRegionM = 1024.0f;
		return Vec2{ region.x * kRegionM, region.y * kRegionM };
	}

	/// @brief リージョン座標をハッシュキーに変換する
	static int64 regionKey(Point p)
	{
		return (static_cast<int64>(p.x) << 32) | static_cast<uint32>(p.y);
	}

	/// @brief ワールド座標 → チャンク座標（負座標対応の floor 除算）
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
	void initScene();         ///< セーブ有無を判定してロードまたは新規生成へ振り分ける
	void initNewGame();       ///< 新規マップ生成（Loading フェーズへ遷移するのみ）
	void saveGame();          ///< saves/default/ にセーブ
	bool loadGame();          ///< saves/default/ からロード、成功なら true
	void addDistricts(const Array<MapGenerator::Settlement>& newDistricts);
	void applyRoadPostProcess();
	void snapshotAllChunks();
	void checkAndGenerateRegions();
	void pollChunkTasks(int maxMerge = kMaxMergePerFrame);
	void dispatchChunkTasks();
	void mergeChunkResult(MapGenerator::ChunkBuildResult&& result);
	void applyZonesFromGrid(const MapGenerator::ChunkBuildResult& result);
	void updateLoadedChunks();
	void updateLoading();                ///< Loading フェーズの更新
	void drawLoadingScreen(float progress); ///< ローディング画面描画
	void startSimThread();               ///< SimThread を起動する

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
