#include "GameScene.hpp"
#include "../gen/RoadPathfinder.hpp"
#include "../save/RoadBinary.hpp"
#include "../sim/SimGraph.hpp"
#include <Siv3D/ViewFrustum.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// 初期化
// ─────────────────────────────────────────────────────────────────────────────

GameScene::GameScene(const InitData& init)
	: IScene{ init }
{
	m_renderTexture = RenderTexture{ Scene::Size(), TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
	m_trainManager.init(&m_trainNetwork);

	m_roadRenderer.loadStyle(U"assets/roads");

	m_sandboxActive = getData().sandboxMode;

	initScene();
}

GameScene::~GameScene()
{
	// SimThread を先に停止（m_network への参照が無効になる前に）
	m_simThread.stop();

	// バックグラウンドチャンクタスクの完了を待機
	for (auto& task : m_chunkTasks)
	{
		if (task.future.valid())
			task.future.wait();
	}
}

void GameScene::initScene()
{
	// タイトルでセーブ名が指定されていればロード、なければ新規生成
	if (!getData().saveName.isEmpty() && loadGame())
	{
		m_phase = GamePhase::Playing;
		return;
	}
	initNewGame();
}

void GameScene::initNewGame()
{
	// ワールドパラメータ設定 + 地名生成（軽量・即座に完了）
	const auto initResult = MapGenerator::initWorld(
		getData().seed, getData().terrain, m_world);
	m_placeNames = std::move(initResult.placeNames);

	// カメラを原点付近に設定（dispatchChunkTasks が原点中心で投入するため）
	m_camera.setFocus(Vec3{ 512.0, 0.0, 512.0 });

	// 初期チャンク数を計算
	const int side = kInitRange * 2 + 1;
	m_totalInitChunks = side * side;

	// Loading フェーズへ遷移（dispatchChunkTasks が自動的にチャンクを投入する）
	m_loadingTimer.restart();
	m_loadingStatus = U"チャンク生成中";
	m_phase = GamePhase::Loading;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loading フェーズ更新
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::updateLoading()
{
	// 追加生成と同じ仕組みでチャンクを投入・統合する（上限を緩和）
	m_world.update(m_camera.focusPoint());   // 地形チャンクも事前生成
	m_world.popNewChunks();
	dispatchChunkTasks();
	pollChunkTasks(9999);

	const int merged = static_cast<int>(m_chunkStore.size());

	// ステータス更新
	if (!m_chunkTasks.isEmpty())
		m_loadingStatus = U"チャンク生成中 (残り {} タスク)"_fmt(m_chunkTasks.size());
	else
		m_loadingStatus = U"{} / {} チャンク完了"_fmt(merged, m_totalInitChunks);

	// 全チャンク統合済みかつ残タスクなし → ポスト処理へ
	if (m_chunkTasks.isEmpty() && merged >= m_totalInitChunks)
	{
		Logger << U"[Phase] Loading 完了 ({:.1f}秒, {} チャンク)"_fmt(
			m_loadingTimer.sF(), merged);
		m_loadingStatus = U"道路ポスト処理中...";

		// ロード済みとして登録
		for (const auto& [key, cd] : m_chunkStore)
			m_loadedChunkKeys.insert(key);

		// ポスト処理を非同期で実行（メインスレッドをブロックしない）
		m_postProcessFuture = std::async(std::launch::async, [this]()
		{
			const Stopwatch total{ StartImmediately::Yes };
			Stopwatch step{ StartImmediately::Yes };

			Logger << U"[PostProcess] 開始 (nodes={}, edges={})"_fmt(
				m_network.nodes().size(), m_network.edges().size());

			{
				int iter = 0;
				constexpr int kMaxIter = 1000;
				while (iter < kMaxIter && m_network.fixSharpAngles(12.5f))
					++iter;
				Logger << U"[PostProcess] fixSharpAngles: {} 回, {:.0f}ms"_fmt(iter, step.msF());
				step.restart();
			}

			m_network.smoothAllCurves();
			Logger << U"[PostProcess] smoothAllCurves: {:.0f}ms"_fmt(step.msF());
			step.restart();

			m_network.resolveIntersections();
			Logger << U"[PostProcess] resolveIntersections: {:.0f}ms"_fmt(step.msF());
			step.restart();

			m_network.spreadIntersectionTangents();
			Logger << U"[PostProcess] spreadIntersectionTangents: {:.0f}ms"_fmt(step.msF());
			step.restart();

			m_network.removeDuplicateEdges(getData().seed);
			Logger << U"[PostProcess] removeDuplicateEdges: {:.0f}ms"_fmt(step.msF());

			Logger << U"[Phase] PostProcess 完了 ({:.0f}ms)"_fmt(total.msF());
		});

		m_phase = GamePhase::PostProcess;
		return;
	}

	// 進捗描画
	const float progress = (m_totalInitChunks > 0)
		? Clamp(static_cast<float>(merged) / m_totalInitChunks, 0.0f, 1.0f)
		: 0.0f;
	drawLoadingScreen(progress);
}

// ─────────────────────────────────────────────────────────────────────────────
// SimThread 起動
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::startSimThread()
{
	auto simGraph = std::make_shared<const SimGraph>(SimGraph::build(m_network));

	TrafficManager traffic;
	traffic.setSimGraph(simGraph);
	traffic.markNetworkDirty();
	for (int i = 0; i < 20; ++i)
		traffic.spawnVehicle();

	m_simThread.start(std::move(traffic), TrainManager{},
	                  EventSystem{}, m_clock, simGraph);
}

// ─────────────────────────────────────────────────────────────────────────────
// ローディング画面描画
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::drawLoadingScreen(float progress)
{
	Scene::Rect().draw(ColorF{ 0.07, 0.11, 0.16 });

	static const Font titleFont{ FontMethod::MSDF, 48, Typeface::Bold };
	static const Font uiFont{ FontMethod::MSDF, 20 };
	static const Font smallFont{ FontMethod::MSDF, 16 };

	const Vec2 center = Scene::Center();

	// 経過時間
	const int elapsedSec = static_cast<int>(m_loadingTimer.sF());
	const int min = elapsedSec / 60;
	const int sec = elapsedSec % 60;
	uiFont(U"{}:{:0>2}"_fmt(min, sec)).drawAt(
		center.movedBy(0, -110), ColorF{ 0.7 });

	titleFont(U"マップ生成中...").drawAt(center.movedBy(0, -60), ColorF{ 0.9 });

	// プログレスバー
	const RectF barBg{ center.x - 200, center.y, 400, 24 };
	barBg.draw(ColorF{ 0.2 });
	RectF{ barBg.pos, barBg.w * progress, barBg.h }.draw(ColorF{ 0.3, 0.7, 1.0 });

	// パーセント
	uiFont(U"{}%"_fmt(static_cast<int>(progress * 100))).drawAt(barBg.center(), ColorF{ 1.0 });

	// チャンク数
	const int merged = static_cast<int>(m_chunkStore.size());
	uiFont(U"{} / {} チャンク完了"_fmt(merged, m_totalInitChunks)).drawAt(
		center.movedBy(0, 40), ColorF{ 0.7 });

	// 実行中の処理内容
	smallFont(m_loadingStatus).drawAt(
		center.movedBy(0, 70), ColorF{ 0.5 });
}

// ─────────────────────────────────────────────────────────────────────────────
// セーブ（saves/default/ に全チャンクデータを書き出す）
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::saveGame()
{
	// セーブ名が未設定（新規生成からの初回セーブ）なら default を使う
	if (getData().saveName.isEmpty())
		getData().saveName = U"default";

	const String kRoot = U"saves/{}"_fmt(getData().saveName);

	FileSystem::CreateDirectories(U"{}/global"_fmt(kRoot));

	// ---- meta.json ----
	JSON meta;
	meta[U"version"]      = 1;
	meta[U"seed"]         = getData().seed;
	meta[U"terrain"]      = static_cast<int>(getData().terrain);
	meta[U"gameNow"]      = m_clock.now;
	meta[U"timeScale"]    = static_cast<int>(m_clock.speed);
	meta[U"nextNodeId"]   = m_network.nextNodeId();
	meta[U"nextEdgeId"]   = m_network.nextEdgeId();
	meta[U"cameraFocusX"] = m_camera.focusPoint().x;
	meta[U"cameraFocusY"] = m_camera.focusPoint().y;
	meta[U"cameraFocusZ"] = m_camera.focusPoint().z;
	meta.save(U"{}/meta.json"_fmt(kRoot));

	// ---- global/economy.json ----
	JSON eco;
	eco[U"funds"]      = m_economy.funds;
	eco[U"population"] = m_economy.population;
	eco[U"happiness"]  = m_economy.happiness;
	eco.save(U"{}/global/economy.json"_fmt(kRoot));

	// ---- chunks/{cx}_{cy}/ ----
	for (const auto& [key, cd] : m_chunkStore)
	{
		const String chunkDir = U"{}/chunks/{}_{}"_fmt(
			kRoot, cd.chunkCoord.x, cd.chunkCoord.y);
		FileSystem::CreateDirectories(chunkDir);

		// roads.bin（バイナリ）
		RoadBinary::write(U"{}/roads.bin"_fmt(chunkDir),
		                  cd.chunkCoord.x, cd.chunkCoord.y,
		                  cd.nodes, cd.edges);

		// districts.json（地区データ）
		JSON dist;
		dist[U"count"] = static_cast<int>(cd.districts.size());
		for (int i = 0; i < static_cast<int>(cd.districts.size()); ++i)
		{
			const auto& s = cd.districts[i];
			dist[U"type_{}"_fmt(i)] = static_cast<int>(s.type);
			dist[U"cx_{}"_fmt(i)]   = s.center.x;
			dist[U"cy_{}"_fmt(i)]   = s.center.y;
			dist[U"name_{}"_fmt(i)] = s.name;
		}
		dist.save(U"{}/districts.json"_fmt(chunkDir));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ロード（saves/default/ から全チャンクデータを復元する）
// ─────────────────────────────────────────────────────────────────────────────

bool GameScene::loadGame()
{
	const String kRoot = U"saves/{}"_fmt(getData().saveName);

	// ---- meta.json ----
	const JSON meta = JSON::Load(U"{}/meta.json"_fmt(kRoot));
	if (!meta) return false;

	getData().seed    = meta[U"seed"].get<uint64>();
	getData().terrain = static_cast<TerrainType>(meta[U"terrain"].get<int>());
	const double gameNow    = meta[U"gameNow"].get<double>();
	const int    timeScale  = meta[U"timeScale"].get<int>();
	const int    nextNodeId = meta[U"nextNodeId"].get<int>();
	const int    nextEdgeId = meta[U"nextEdgeId"].get<int>();
	const double focusX     = meta[U"cameraFocusX"].get<double>();
	const double focusY     = meta[U"cameraFocusY"].get<double>();
	const double focusZ     = meta[U"cameraFocusZ"].get<double>();

	// ---- 地形パラメータ・地名を初期化 ----
	{
		const auto initResult = MapGenerator::initWorld(
			getData().seed, getData().terrain, m_world);
		m_placeNames = std::move(initResult.placeNames);
	}
	m_chunkStore.clear();
	m_districts.clear();
	m_urbanCenters.clear();

	// ---- economy.json ----
	const JSON eco = JSON::Load(U"{}/global/economy.json"_fmt(kRoot));
	if (eco)
	{
		m_economy.funds      = eco[U"funds"].get<double>();
		m_economy.population = eco[U"population"].get<int>();
		m_economy.happiness  = eco[U"happiness"].get<double>();
	}

	// ---- chunks/{cx}_{cy}/ を列挙してロード ----
	const FilePath chunksDir = U"{}/chunks/"_fmt(kRoot);
	if (!FileSystem::Exists(chunksDir)) return false;

	for (const auto& entry : FileSystem::DirectoryContents(chunksDir, Recursive::No))
	{
		if (!FileSystem::IsDirectory(entry)) continue;

		// ディレクトリ名 "{cx}_{cy}" をパース
		const String dirName = FileSystem::FileName(entry);
		const Array<String> parts = dirName.split(U'_');
		if (parts.size() != 2) continue;
		const auto cxOpt = ParseOpt<int32>(parts[0]);
		const auto cyOpt = ParseOpt<int32>(parts[1]);
		if (!cxOpt || !cyOpt) continue;
		const int32 cx = *cxOpt, cy = *cyOpt;
		const Point chunkCoord{ cx, cy };
		const int64 key = regionKey(chunkCoord);

		// roads.bin を読み込み
		Array<RoadNode> loadedNodes;
		Array<RoadEdge> loadedEdges;
		if (!RoadBinary::read(U"{}/roads.bin"_fmt(entry), loadedNodes, loadedEdges))
			continue;

		// m_network に追加（addNodeRaw/addEdgeRaw は重複 ID をスキップ）
		for (const auto& n : loadedNodes) m_network.addNodeRaw(n);
		for (const auto& e : loadedEdges) m_network.addEdgeRaw(e);

		// ChunkData を構築
		auto& cd        = m_chunkStore[key];
		cd.chunkCoord   = chunkCoord;
		cd.nodes        = loadedNodes;
		cd.edges        = loadedEdges;

		// districts.json を読み込み
		const JSON dist = JSON::Load(U"{}/districts.json"_fmt(entry));
		if (dist)
		{
			const int count = dist[U"count"].get<int>();
			Array<MapGenerator::Settlement> settlements;
			for (int i = 0; i < count; ++i)
			{
				MapGenerator::Settlement s;
				s.type   = static_cast<MapGenerator::SettlementType>(
				               dist[U"type_{}"_fmt(i)].get<int>());
				s.center = Vec2{ dist[U"cx_{}"_fmt(i)].get<double>(),
				                 dist[U"cy_{}"_fmt(i)].get<double>() };
				s.name   = dist[U"name_{}"_fmt(i)].get<String>();
				settlements << s;
			}
			cd.districts = settlements;
			addDistricts(settlements);
		}
	}

	// ---- nextNodeId / nextEdgeId を復元 ----
	m_network.setNextIds(nextNodeId, nextEdgeId);

	// ---- レンダラに通知 ----
	m_roadRenderer.markTopologyChanged();

	// ---- ゲーム時刻を復元 ----
	m_clock.now   = gameNow;
	m_clock.speed = static_cast<TimeSpeed>(timeScale);
	m_clock.syncCalendar();
	m_lastEconYear  = m_clock.year;
	m_lastEconMonth = m_clock.month;

	// ---- カメラを復元 ----
	m_camera.setFocus(Vec3{ focusX, focusY, focusZ });

	// ---- ロード済みチャンクを登録 ----
	for (const auto& [key, cd] : m_chunkStore)
		m_loadedChunkKeys.insert(key);

	// ---- 鉄道初期設定 ----
	MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);

	// ---- ワールド更新 ----
	m_world.update(m_camera.focusPoint());
	m_world.popNewChunks();

	// ---- SimThread 起動 ----
	startSimThread();

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 地区リストを一括更新（m_districts と m_urbanCenters を同時に更新）
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::addDistricts(const Array<MapGenerator::Settlement>& newDistricts)
{
	for (const auto& s : newDistricts)
	{
		m_districts << s;
		if (s.type == MapGenerator::SettlementType::Urban)
			m_urbanCenters << s.center;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3.5 ポスト処理（全チャンク生成後にまとめて適用）
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::applyRoadPostProcess()
{
	int iter = 0;
	constexpr int kMaxIter = 1000;
	while (iter < kMaxIter && m_network.fixSharpAngles(12.5f))
		++iter;
	m_network.smoothAllCurves();
	m_network.resolveIntersections();
	m_network.spreadIntersectionTangents();
	m_network.removeDuplicateEdges(getData().seed);
	snapshotAllChunks();
	m_roadRenderer.markTopologyChanged();
	notifyNetworkChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// ChunkStore スナップショット再構築
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::snapshotAllChunks()
{
	// ロード済みチャンクのノード・エッジのみクリア（アンロード済みチャンクは保持）
	for (const int64 key : m_loadedChunkKeys)
	{
		auto it = m_chunkStore.find(key);
		if (it == m_chunkStore.end()) continue;
		it->second.nodes.clear();
		it->second.edges.clear();
	}

	// ノードをワールド座標で決まるチャンクに割り当てる
	// chunkKey → 登録済み nodeId セット（境界ノード重複管理用）
	HashTable<int64, HashSet<int>> registered;

	for (const auto& node : m_network.nodes())
	{
		if (node.id < 0) continue;
		const int64 key = regionKey(worldPosToChunk(node.position));
		if (!m_chunkStore.contains(key)) continue;
		m_chunkStore[key].nodes << node;
		registered[key].insert(node.id);
	}

	// エッジを nodeA 座標で決まるチャンクに割り当てる
	// クロスチャンクエッジの nodeB は、エッジ所有チャンクに境界ノードとして複製する
	for (const auto& edge : m_network.edges())
	{
		if (edge.id < 0) continue;
		const RoadNode* nA = m_network.getNode(edge.nodeA);
		if (!nA || nA->id < 0) continue;
		const int64 ownerKey = regionKey(worldPosToChunk(nA->position));
		if (!m_chunkStore.contains(ownerKey)) continue;

		m_chunkStore[ownerKey].edges << edge;

		// nodeB がエッジ所有チャンクと異なる場合、境界ノードとして複製する
		const RoadNode* nB = m_network.getNode(edge.nodeB);
		if (!nB || nB->id < 0) continue;
		if (!registered[ownerKey].contains(nB->id))
		{
			m_chunkStore[ownerKey].nodes << *nB;
			registered[ownerKey].insert(nB->id);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 無限ワールド: バックグラウンドチャンク生成
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::checkAndGenerateRegions()
{
	m_world.popNewChunks();
	pollChunkTasks();
	dispatchChunkTasks();
	updateLoadedChunks();
}

void GameScene::pollChunkTasks(int maxMerge)
{
	int merged = 0;
	for (auto it = m_chunkTasks.begin(); it != m_chunkTasks.end(); )
	{
		if (merged >= maxMerge) break;
		if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			auto result = it->future.get();
			m_dispatchedKeys.erase(regionKey(result.chunkCoord));
			mergeChunkResult(std::move(result));
			it = m_chunkTasks.erase(it);
			++merged;
		}
		else
		{
			++it;
		}
	}
}

void GameScene::dispatchChunkTasks()
{
	const Vec3 focus = m_camera.focusPoint();
	auto floorDiv = [](float v, float size) -> int
	{
		const int q = static_cast<int>(v / size);
		return (v < 0.0f && v != static_cast<float>(q) * size) ? q - 1 : q;
	};
	const int cx = floorDiv(static_cast<float>(focus.x), CHUNK_SIZE);
	const int cz = floorDiv(static_cast<float>(focus.z), CHUNK_SIZE);

	// 未生成・未投入のチャンクを収集
	Array<Point> pending;
	for (int dz = -5; dz <= 5; ++dz)
	{
		for (int dx = -5; dx <= 5; ++dx)
		{
			const Point chunk{ cx + dx, cz + dz };
			const int64 key = regionKey(chunk);
			if (!m_chunkStore.contains(key) && !m_dispatchedKeys.contains(key))
				pending << chunk;
		}
	}

	if (pending.isEmpty()) return;

	// カメラに近い順にソート（チェビシェフ距離）
	pending.sort_by([cx, cz](const Point& a, const Point& b)
	{
		return Max(Abs(a.x - cx), Abs(a.y - cz))
		     < Max(Abs(b.x - cx), Abs(b.y - cz));
	});

	// 空きスロット分だけバックグラウンドタスクを投入
	const int slots = kMaxChunkTasks - static_cast<int>(m_chunkTasks.size());
	if (slots <= 0) return;

	const uint64 seed = getData().seed;
	const Array<Vec2> urbanSnap = m_urbanCenters;

	// 既存ノードの位置スナップショット（A* 接続用）
	using NodeSnapshot = MapGenerator::NodeSnapshot;
	Array<NodeSnapshot> nodeSnap;
	for (const auto& node : m_network.nodes())
		if (node.id >= 0)
			nodeSnap << NodeSnapshot{ node.id, node.position };

	for (int i = 0; i < Min(slots, static_cast<int>(pending.size())); ++i)
	{
		const Point coord = pending[i];
		const Vec2 offset = regionToWorldOffset(coord);
		const int64 key   = regionKey(coord);

		m_dispatchedKeys.insert(key);

		ChunkBuildTask task;
		task.chunkCoord = coord;
		task.future = std::async(std::launch::async,
			[offset, seed, &world = std::as_const(m_world), urbanSnap, nodeSnap]()
			{
				return MapGenerator::buildChunk(
					offset, seed, world, urbanSnap, nodeSnap);
			});
		m_chunkTasks << std::move(task);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// バックグラウンド結果をメインスレッドに統合する
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::mergeChunkResult(MapGenerator::ChunkBuildResult&& result)
{
	const int64 key = regionKey(result.chunkCoord);
	const Point cc = result.chunkCoord;

	// 事前計算済み地形 heightMap を World にインストール
	if (result.terrainHeightMap.size() != Size{0, 0})
		m_world.installChunk(cc, std::move(result.terrainHeightMap));

	// ChunkData 作成（二重生成防止）
	m_chunkStore[key].chunkCoord = result.chunkCoord;

	if (result.settlements.isEmpty())
	{
		applyZonesFromGrid(result);
		return;
	}

	// ---- 1. ノードIDリマップ＆追加 ----
	// connectionNodeId は既存ネットワークに存在するため addNode せずリマップのみ
	HashTable<int, int> nodeIdMap;
	for (const auto& localNode : result.localNodes)
	{
		if (localNode.id < 0) continue;
		if (localNode.id == result.connectionNodeId)
		{
			// 既存ノード → リマップテーブルにそのまま登録
			nodeIdMap[localNode.id] = localNode.id;
			continue;
		}
		const int globalId = m_network.addNode(localNode.position, localNode.type);
		nodeIdMap[localNode.id] = globalId;
	}

	// ---- 2. エッジIDリマップ＆追加 + ChunkData 保存 ----
	auto& cd = m_chunkStore[key];
	cd.districts = result.settlements;
	cd.nodes.clear();
	cd.edges.clear();

	// ノード保存（境界ノード connectionNodeId も含む）
	{
		HashSet<int> savedNodeIds;
		for (const auto& localNode : result.localNodes)
		{
			if (localNode.id < 0) continue;
			const auto it = nodeIdMap.find(localNode.id);
			if (it == nodeIdMap.end()) continue;
			const int globalId = it->second;
			if (savedNodeIds.contains(globalId)) continue;
			const RoadNode* netNode = m_network.getNode(globalId);
			if (netNode)
			{
				cd.nodes << *netNode;
				savedNodeIds.insert(globalId);
			}
		}
	}

	// エッジ追加＋グローバルIDで保存
	for (const auto& localEdge : result.localEdges)
	{
		if (localEdge.id < 0) continue;
		const auto itA = nodeIdMap.find(localEdge.nodeA);
		const auto itB = nodeIdMap.find(localEdge.nodeB);
		if (itA == nodeIdMap.end() || itB == nodeIdMap.end()) continue;

		const auto globalEdgeId = m_network.addEdge(
			itA->second, itB->second,
			localEdge.ctrlA, localEdge.ctrlB,
			localEdge.roadType,
			static_cast<int>(localEdge.lanes.size()));

		if (globalEdgeId)
		{
			const RoadEdge* ge = m_network.getEdge(*globalEdgeId);
			if (ge) cd.edges << *ge;
		}
	}

	// ---- 3. ゾーン割当 ----
	applyZonesFromGrid(result);
	m_loadedChunkKeys.insert(key);
	addDistricts(result.settlements);

	// 追加したノード周辺のキャッシュのみ無効化する
	for (const auto& [localId, globalId] : nodeIdMap)
		m_roadRenderer.markTopologyChangedAt(globalId, m_network);
	// SimGraph 再構築は updateLoadedChunks でバッチ実行するため、ここではスキップ
}

// ─────────────────────────────────────────────────────────────────────────────
// キャッシュ済み高さグリッドからゾーンを割り当てる
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::applyZonesFromGrid(const MapGenerator::ChunkBuildResult& result)
{
	auto gridToWorld = [&](int gx, int gz) -> Vec2
	{
		return Vec2{ result.gridOffset.x + (gx + 0.5f) * result.cellSize,
		             result.gridOffset.y + (gz + 0.5f) * result.cellSize };
	};
	auto worldToGrid = [&](float wx, float wz) -> Point
	{
		return Point{
			Clamp(static_cast<int>((wx - result.gridOffset.x) / result.cellSize), 0, result.gridW - 1),
			Clamp(static_cast<int>((wz - result.gridOffset.y) / result.cellSize), 0, result.gridH - 1)
		};
	};
	auto gridHeight = [&](int gx, int gz) -> float
	{
		return result.heightGrid[gz * result.gridW + gx];
	};

	for (const auto& s : result.settlements)
	{
		const float innerDist = (s.type == MapGenerator::SettlementType::Urban)    ? 400.0f
		                      : (s.type == MapGenerator::SettlementType::Suburbs)  ? 200.0f : 100.0f;
		const float midDist   = (s.type == MapGenerator::SettlementType::Urban)    ? 800.0f
		                      : (s.type == MapGenerator::SettlementType::Suburbs)  ? 600.0f : 300.0f;
		const float outerDist = (s.type == MapGenerator::SettlementType::Urban)    ? 1200.0f
		                      : midDist;

		const float innerSq = innerDist * innerDist;
		const float midSq   = midDist   * midDist;
		const float outerSq = outerDist * outerDist;

		const Point g0 = worldToGrid(static_cast<float>(s.center.x) - outerDist,
		                             static_cast<float>(s.center.y) - outerDist);
		const Point g1 = worldToGrid(static_cast<float>(s.center.x) + outerDist,
		                             static_cast<float>(s.center.y) + outerDist);

		for (int gz = g0.y; gz <= g1.y; ++gz)
		{
			for (int gx = g0.x; gx <= g1.x; ++gx)
			{
				if (gx < 0 || gx >= result.gridW || gz < 0 || gz >= result.gridH) continue;

				const Vec2  wp  = gridToWorld(gx, gz);
				const float dSq = static_cast<float>(s.center.distanceFromSq(wp));
				const float h   = gridHeight(gx, gz);

				if (h < 0.0f) continue;

				const Vec3 wp3{ wp.x, h, wp.y };

				if (dSq <= innerSq)
				{
					const ZoneType zt = (s.type == MapGenerator::SettlementType::Urban)
						? ZoneType::Commercial
						: ZoneType::Residential;
					m_zoneManager.paintZone(m_world, wp3, zt, 0);
				}
				else if (dSq <= midSq)
				{
					m_zoneManager.paintZone(m_world, wp3, ZoneType::Residential, 0);
				}
				else if (dSq <= outerSq)
				{
					m_zoneManager.paintZone(m_world, wp3, ZoneType::LowResidential, 0);
				}
			}
		}
	}

	// 農地: 低地かつゾーン未設定のセルを Agriculture に
	for (int gz = 0; gz < result.gridH; ++gz)
	{
		for (int gx = 0; gx < result.gridW; ++gx)
		{
			const float h = gridHeight(gx, gz);
			if (h < 0.0f || h >= 2.0f) continue;

			const Vec2 wp = gridToWorld(gx, gz);
			const Vec3 wp3{ wp.x, h, wp.y };

			const ZoneType existing = m_zoneManager.getZone(m_world, wp3);
			if (existing == ZoneType::Unzoned)
				m_zoneManager.paintZone(m_world, wp3, ZoneType::Agriculture, 0);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 11×11 範囲外チャンクのアンロード / 範囲内チャンクのリロード
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::updateLoadedChunks()
{
	const Vec3 focus = m_camera.focusPoint();
	auto floorDiv = [](float v, float size) -> int
	{
		const int q = static_cast<int>(v / size);
		return (v < 0.0f && v != static_cast<float>(q) * size) ? q - 1 : q;
	};
	const int cx = floorDiv(static_cast<float>(focus.x), CHUNK_SIZE);
	const int cz = floorDiv(static_cast<float>(focus.z), CHUNK_SIZE);

	// 現在の 11×11 範囲のキー集合を構築
	HashSet<int64> wantedKeys;
	for (int dz = -5; dz <= 5; ++dz)
		for (int dx = -5; dx <= 5; ++dx)
			wantedKeys.insert(regionKey(Point{ cx + dx, cz + dz }));

	bool changed = false;
	HashSet<int> affectedNodes;

	// ---- アンロード: ロード済みだが範囲外のチャンク ----
	Array<int64> toUnload;
	for (const int64 key : m_loadedChunkKeys)
	{
		if (!wantedKeys.contains(key))
			toUnload << key;
	}
	for (const int64 key : toUnload)
	{
		// まず最新状態をスナップショットしてから除去
		const auto it = m_chunkStore.find(key);
		if (it == m_chunkStore.end()) continue;

		// エッジを削除
		for (const auto& edge : it->second.edges)
		{
			if (edge.id >= 0)
			{
				affectedNodes.insert(edge.nodeA);
				affectedNodes.insert(edge.nodeB);
				m_network.removeEdge(edge.id);
			}
		}
		m_loadedChunkKeys.erase(key);
		changed = true;
	}

	// ---- リロード: 範囲内だが未ロードのチャンク ----
	for (const int64 key : wantedKeys)
	{
		if (m_loadedChunkKeys.contains(key)) continue;

		const auto it = m_chunkStore.find(key);
		if (it == m_chunkStore.end()) continue;

		for (const auto& node : it->second.nodes)
			m_network.addNodeRaw(node);
		for (const auto& edge : it->second.edges)
		{
			m_network.addEdgeRaw(edge);
			affectedNodes.insert(edge.nodeA);
			affectedNodes.insert(edge.nodeB);
		}

		m_loadedChunkKeys.insert(key);
		changed = true;
	}

	// アンロード・リロード両方完了後に孤立ノードを除去
	// （境界ノードはリロードで再接続されるため、ここでは本当に孤立したものだけ消える）
	for (const int nodeId : affectedNodes)
	{
		const RoadNode* node = m_network.getNode(nodeId);
		if (node && node->edgeIds.isEmpty())
			m_network.removeNode(nodeId);
	}

	if (changed)
	{
		for (const int nodeId : affectedNodes)
			m_roadRenderer.markTopologyChangedAt(nodeId, m_network);
		notifyNetworkChanged();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 毎フレーム更新（ロジック + 描画）
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::update()
{
	if (m_phase == GamePhase::Loading)
	{
		updateLoading();
		return;
	}

	if (m_phase == GamePhase::PostProcess)
	{
		if (m_postProcessFuture.valid())
		{
			// 非同期ポスト処理の完了を待つ
			if (m_postProcessFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			{
				m_postProcessFuture.get();   // future を消費（valid() が false に）
				Logger << U"[Loading] ポスト処理完了 → セットアップは次フレーム";
				m_loadingStatus = U"初期化中...";
			}
			else
			{
				m_loadingStatus = U"道路ポスト処理中...";
			}
		}
		else
		{
			// future 消費済み → メインスレッド側のセットアップ
			const Stopwatch setupTimer{ StartImmediately::Yes };
			Stopwatch step{ StartImmediately::Yes };

			snapshotAllChunks();
			Logger << U"[Setup] snapshotAllChunks: {:.0f}ms"_fmt(step.msF());
			step.restart();

			m_roadRenderer.markTopologyChanged();

			MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts);
			Logger << U"[Setup] setupTrain: {:.0f}ms"_fmt(step.msF());
			step.restart();

			// カメラ初期位置
			Vec3 cameraFocus{ 512.0, 0.0, 512.0 };
			for (const auto& s : m_districts)
			{
				if (s.type == MapGenerator::SettlementType::Urban)
				{
					const float y = m_world.computeHeight(
						static_cast<float>(s.center.x), static_cast<float>(s.center.y));
					cameraFocus = Vec3{ s.center.x, y, s.center.y };
					break;
				}
			}
			if (cameraFocus.y == 0.0 && !m_districts.isEmpty())
			{
				const auto& s = m_districts[0];
				const float y = m_world.computeHeight(
					static_cast<float>(s.center.x), static_cast<float>(s.center.y));
				cameraFocus = Vec3{ s.center.x, y, s.center.y };
			}
			m_camera.setFocus(cameraFocus);

			m_world.update(m_camera.focusPoint());
			Logger << U"[Setup] world.update: {:.0f}ms"_fmt(step.msF());
			step.restart();

			m_world.popNewChunks();
			m_lastEconYear  = m_clock.year;
			m_lastEconMonth = m_clock.month;

			startSimThread();
			Logger << U"[Setup] startSimThread: {:.0f}ms"_fmt(step.msF());

			Logger << U"[Phase] Setup 完了 ({:.0f}ms)"_fmt(setupTimer.msF());
			Logger << U"[Phase] 全体 {:.1f}秒 → Playing へ遷移"_fmt(m_loadingTimer.sF());
			m_phase = GamePhase::Playing;
		}

		// PostProcess 中は常にローディング画面を描画
		drawLoadingScreen(1.0f);
		return;
	}

	const double dt = Scene::DeltaTime();

	// ---- SimThread から時刻を同期 ----
	{
		auto lock = m_simThread.lockForRead();
		m_clock = m_simThread.clock();
	}

	// ---- 通知を取得 ----
	for (const auto& n : m_simThread.popNotifications())
	{
		m_notifications << n;
		if (static_cast<int>(m_notifications.size()) > 5)
			m_notifications.erase(m_notifications.begin());
	}

	// ---- メインスレッドのロジック ----
	const Stopwatch swLogic{ StartImmediately::Yes };
	m_world.update(m_camera.focusPoint());
	checkAndGenerateRegions();
	m_camera.update(dt, m_world);
	m_logicMs = swLogic.msF();

	// フォローカメラ
	if (m_camera.mode() != CameraMode::Overview)
	{
		auto lock = m_simThread.lockForRead();
		const auto& vehicles = m_simThread.vehicles();
		if (!vehicles.isEmpty())
		{
			m_followVehicleIdx = m_followVehicleIdx % static_cast<int>(vehicles.size());
			const Vehicle& v = vehicles[m_followVehicleIdx];
			m_camera.setFollowTarget(v.position, v.heading);
		}
	}

	handleInput();
	updateCursor();
	m_debugRenderer.handleInput();

	// ---- 月次更新 ----
	if (m_clock.year != m_lastEconYear || m_clock.month != m_lastEconMonth)
	{
		m_lastEconYear  = m_clock.year;
		m_lastEconMonth = m_clock.month;
		m_zoneManager.monthlyUpdate(m_world, m_network, m_clock.now, m_economy);
	}

	// ---- ゲーム速度を SimThread に同期 ----
	m_simThread.setSpeed(m_clock.speed);

	// ---- 描画 ----
	renderWorld();
}

// ─────────────────────────────────────────────────────────────────────────────
// 描画
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::renderWorld()
{
	// ---- 描画時間計測 ----
	static double s_sky = 0, s_terrain = 0, s_road = 0, s_zone = 0;
	static double s_vehicle = 0, s_train = 0, s_debug = 0, s_ui = 0, s_total = 0;
	Stopwatch swStep{ StartImmediately::Yes };
	const Stopwatch swTotal{ StartImmediately::Yes };

	auto lap = [&](double& out) { out = swStep.msF(); swStep.restart(); };

	// ---- 太陽・空のパラメータ計算 ----
	const float hour = m_clock.hour;
	const float t    = (hour - 6.0f) * static_cast<float>(Math::Pi / 12.0);
	const float sinT  = static_cast<float>(Math::Sin(t));
	const float dayF  = Clamp(sinT, 0.0f, 1.0f);
	const float dawnF = Clamp(1.0f - Abs(sinT) * 2.5f, 0.0f, 1.0f);
	const double exposure = 0.15 + 0.85 * dayF + 0.30 * dawnF;

	// ---- 3D シーンを深度バッファ付きレンダーテクスチャに描画 ----
	{
		const ScopedRenderTarget3D target{ m_renderTexture.clear(ColorF{ 0.2, 0.3, 0.4 }.removeSRGBCurve()) };
		const ScopedRenderStates3D depthState{ DepthStencilState::DepthTestWrite };

		Graphics3D::SetCameraTransform(m_camera.camera3D());

		const Vec3 sunDir = Vec3{ Math::Cos(t), sinT, 0.3 }.normalized();
		Graphics3D::SetSunDirection(sunDir);
		Graphics3D::SetGlobalAmbientColor(ColorF{ 0.55 + 0.30 * dayF + 0.10 * dawnF });

		const ColorF dayZenith  { 0.10, 0.35, 0.80 };
		const ColorF dawnZenith { 0.22, 0.18, 0.38 };
		const ColorF nightZenith{ 0.01, 0.02, 0.07 };
		m_sky.zenithColor = nightZenith.lerp(dawnZenith, dawnF).lerp(dayZenith, dayF);

		const ColorF dayHorizon  { 0.60, 0.78, 0.95 };
		const ColorF dawnHorizon { 0.85, 0.42, 0.15 };
		const ColorF nightHorizon{ 0.02, 0.03, 0.10 };
		m_sky.horizonColor = nightHorizon.lerp(dawnHorizon, dawnF).lerp(dayHorizon, dayF);

		m_sky.starBrightness = Clamp(1.0 - dayF * 3.0 - dawnF * 2.0, 0.0, 1.0);
		m_sky.cloudTime = Scene::Time() * 0.015;
		m_sky.draw(exposure);
		lap(s_sky);

		const ViewFrustum frustum{ m_camera.camera3D(), 12000.0 };
		m_worldRenderer.render(m_world, m_camera.camera3D());
		lap(s_terrain);

		m_roadRenderer.render(m_network, m_clock.now, m_world, frustum,
		                     m_camera.camera3D().getEyePosition());
		lap(s_road);

		m_zoneManager.renderOverlay(m_world);
		lap(s_zone);

		// 車両の (edgeId, arcPos) → ワールド座標に変換して描画する
		{
			auto lock = m_simThread.lockForRead();
			Array<Vehicle> renderVehicles = m_simThread.vehicles();
			lock.unlock();

			for (auto& v : renderVehicles)
			{
				if (v.currentEdge < 0) continue;
				if (const auto bezier = m_network.getBezier(v.currentEdge))
				{
					const float clampedArc = Clamp(v.arcPos, 0.0f, bezier->totalLength);
					v.position = bezier->positionAt(clampedArc);
					v.position.y = m_world.sampleHeight(
						static_cast<float>(v.position.x),
						static_cast<float>(v.position.z)) + 2.0f;
					const Vec3 tangent = bezier->tangentAt(clampedArc);
					const RoadEdge* edge = m_network.getEdge(v.currentEdge);
					const bool fwd = (!edge || v.currentLane >= static_cast<int>(edge->lanes.size()))
						? true : (edge->lanes[v.currentLane].dir == LaneDir::Forward);
					const float sign = fwd ? 1.0f : -1.0f;
					v.heading = static_cast<float>(Math::Atan2(sign * tangent.x, sign * tangent.z));
				}
			}
			m_vehicleRenderer.render(renderVehicles, m_camera.camera3D().getEyePosition());
		}
		lap(s_vehicle);

		m_trainRenderer.renderTracks(m_trainNetwork);
		m_trainRenderer.renderTrains(m_trainManager.trains());
		lap(s_train);

		if (m_mode == EditMode::TrainDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 6.0f }.draw(ColorF{ 0.9, 0.85, 0.2, 0.8 }.removeSRGBCurve());
		}

		// TODO: バス停描画を SimThread 経由に移行
		// for (const auto& stop : m_simThread.busStops()) { ... }

		if (m_mode == EditMode::BusRouteDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 4.0f }.draw(ColorF{ 0.2, 0.5, 0.9, 0.8 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::RoadDraw && m_cursorGroundPos)
		{
			Sphere{ *m_cursorGroundPos, 5.0f }.draw(ColorF{ 1, 1, 0, 0.8 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::ZonePaint && m_rectStart && m_cursorGroundPos)
		{
			const double minX = Min(m_rectStart->x, m_cursorGroundPos->x);
			const double maxX = Max(m_rectStart->x, m_cursorGroundPos->x);
			const double minZ = Min(m_rectStart->z, m_cursorGroundPos->z);
			const double maxZ = Max(m_rectStart->z, m_cursorGroundPos->z);
			const double cx = (minX + maxX) * 0.5;
			const double cz = (minZ + maxZ) * 0.5;
			const double sx = Max(maxX - minX, 1.0);
			const double sz = Max(maxZ - minZ, 1.0);
			const ColorF zc = zoneColor(m_paintZone);
			Box{ cx, 0.5, cz, sx, 1.0, sz }.draw(ColorF{ zc.r, zc.g, zc.b, 0.3 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::TerrainEdit && m_cursorGroundPos)
		{
			Cylinder{ *m_cursorGroundPos + Vec3{0, -1, 0},
			          *m_cursorGroundPos + Vec3{0, 2, 0},
			          static_cast<double>(m_terrainBrushRadius) }
				.draw(ColorF{ 0.9, 0.6, 0.2, 0.25 }.removeSRGBCurve());
		}

		if (m_mode == EditMode::SandboxEdit)
		{
			const Vec2 cur2D = m_cursorGroundPos
				? Vec2{ m_cursorGroundPos->x, m_cursorGroundPos->z } : Vec2{ 0, 0 };

			// 制御点ハンドルを表示（ノードとの接線ライン + 小球）
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0) continue;
				const RoadNode* nA = m_network.getNode(edge.nodeA);
				const RoadNode* nB = m_network.getNode(edge.nodeB);
				if (!nA || !nB) continue;

				const auto isHovCtrl = [&](Vec3 cp) {
					return m_cursorGroundPos &&
					       Vec2{ cp.x, cp.z }.distanceFrom(cur2D) < 18.0f;
				};
				const bool dragA = m_sandboxDragCtrl &&
				    m_sandboxDragCtrl->edgeId == edge.id && m_sandboxDragCtrl->isA;
				const bool dragB = m_sandboxDragCtrl &&
				    m_sandboxDragCtrl->edgeId == edge.id && !m_sandboxDragCtrl->isA;

				// nodeA ↔ ctrlA のハンドルライン
				const ColorF colA = dragA
				    ? ColorF{ 1.0, 0.5, 0.0, 1.0 }
				    : (isHovCtrl(edge.ctrlA) ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
				                             : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nA->position + Vec3{0,2,0}, edge.ctrlA + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ edge.ctrlA + Vec3{0,2,0}, dragA ? 6.0 : 4.0 }
					.draw(colA.removeSRGBCurve());

				// nodeB ↔ ctrlB のハンドルライン
				const ColorF colB = dragB
				    ? ColorF{ 1.0, 0.5, 0.0, 1.0 }
				    : (isHovCtrl(edge.ctrlB) ? ColorF{ 1.0, 1.0, 0.3, 0.9 }
				                             : ColorF{ 0.2, 0.9, 0.4, 0.7 });
				Line3D{ nB->position + Vec3{0,2,0}, edge.ctrlB + Vec3{0,2,0} }
					.draw(ColorF{ 0.5, 0.5, 0.5, 0.5 }.removeSRGBCurve());
				Sphere{ edge.ctrlB + Vec3{0,2,0}, dragB ? 6.0 : 4.0 }
					.draw(colB.removeSRGBCurve());
			}

			// ノードを球で表示
			for (const auto& node : m_network.nodes())
			{
				if (node.id < 0) continue;
				const bool dragging = m_sandboxDragNode && (*m_sandboxDragNode == node.id);
				const bool hovered  = m_cursorGroundPos &&
				    Vec2{ node.position.x, node.position.z }.distanceFrom(cur2D) < 20.0f;
				const ColorF col = dragging
				    ? ColorF{ 1.0, 0.4, 0.1, 0.95 }
				    : (hovered ? ColorF{ 1.0, 1.0, 0.2, 0.9 }
				               : ColorF{ 0.3, 0.8, 1.0, 0.7 });
				Sphere{ node.position + Vec3{0, 2, 0}, dragging ? 7.0 : 5.0 }
					.draw(col.removeSRGBCurve());
			}
		}

		{
			auto lock = m_simThread.lockForRead();
			m_debugRenderer.render(m_network, m_simThread.vehicles(), m_world, m_camera);
		}
		lap(s_debug);
	}

	Graphics3D::Flush();
	Shader::LinearToScreen(m_renderTexture);

	// ---- UI（2D）----
	m_placeNameRenderer.render(m_districts, m_camera, m_world);
	{
		auto lock = m_simThread.lockForRead();
		m_uiRenderer.render(m_clock, static_cast<int>(m_simThread.vehicles().size()), modeString(), m_economy);
	}
	lap(s_ui);
	s_total = swTotal.msF();

	// ---- 描画時間プロファイル表示（DebugRenderer に委譲） ----
	m_debugRenderer.renderProfiler(s_total, m_logicMs, s_sky, s_terrain,
	                               s_road, s_zone, s_vehicle, s_train,
	                               s_debug, s_ui, m_network);

}

// ─────────────────────────────────────────────────────────────────────────────
// 入力処理
// ─────────────────────────────────────────────────────────────────────────────

void GameScene::handleInput()
{
	// Ctrl+S: セーブ
	if (KeyControl.pressed() && KeyS.down())
	{
		saveGame();
		return;
	}

	// Space: 一時停止 / 直前の速度に復帰
	if (KeySpace.down())
	{
		if (m_clock.speed == TimeSpeed::Paused)
			m_clock.speed = m_prevSpeed;
		else
		{
			m_prevSpeed   = m_clock.speed;
			m_clock.speed = TimeSpeed::Paused;
		}
	}

	if (KeyTab.down())
		m_zoneManager.showOverlay = !m_zoneManager.showOverlay;

	// Esc: 編集モードを抜ける
	if (KeyEscape.down() && m_mode != EditMode::None)
	{
		m_mode               = EditMode::None;
		m_drawStartNode      = none;
		m_rectStart          = none;
		m_trainDrawStartNode = none;
		m_sandboxDragNode    = none;
		m_sandboxDragCtrl    = none;
		m_editingRouteId     = -1;
		m_zoneManager.showOverlay = false;
	}

	if (KeyR.down())
	{
		m_mode = (m_mode == EditMode::RoadDraw) ? EditMode::None : EditMode::RoadDraw;
		m_drawStartNode = none;
		m_rectStart     = none;
	}
	if (KeyZ.down())
	{
		m_mode = (m_mode == EditMode::ZonePaint) ? EditMode::None : EditMode::ZonePaint;
		m_drawStartNode = none;
		m_rectStart     = none;
		m_zoneManager.showOverlay = (m_mode == EditMode::ZonePaint);
	}

	if (m_mode == EditMode::ZonePaint)
	{
		if (Key1.down()) m_paintZone = ZoneType::UrbanControl;
		if (Key2.down()) m_paintZone = ZoneType::LowResidential;
		if (Key3.down()) m_paintZone = ZoneType::Residential;
		if (Key4.down()) m_paintZone = ZoneType::Commercial;
		if (Key5.down()) m_paintZone = ZoneType::Industrial;
		if (Key6.down()) m_paintZone = ZoneType::Agriculture;
		if (Key0.down()) m_paintZone = ZoneType::Unzoned;
	}
	else
	{
		// 1/2/3 キーで速度変更：Space 復帰用に直前速度を更新する
		if (Key1.down()) { m_prevSpeed = TimeSpeed::x1; m_clock.speed = TimeSpeed::x1; }
		if (Key2.down()) { m_prevSpeed = TimeSpeed::x2; m_clock.speed = TimeSpeed::x2; }
		if (Key3.down()) { m_prevSpeed = TimeSpeed::x4; m_clock.speed = TimeSpeed::x4; }
		if (Key0.down()) { m_prevSpeed = m_clock.speed != TimeSpeed::Paused ? m_clock.speed : m_prevSpeed;
		                   m_clock.speed = TimeSpeed::Paused; }
	}

	if (KeyT.down())
		m_simThread.spawnVehicle();

	if (KeyF.down())
		m_camera.cycleMode();

	if (KeyG.down())
	{
		m_mode = (m_mode == EditMode::TerrainEdit) ? EditMode::None : EditMode::TerrainEdit;
		m_drawStartNode = none;
		m_rectStart     = none;
	}

	if (m_mode == EditMode::TerrainEdit && KeyControl.pressed())
	{
		const double wheel = Mouse::Wheel();
		m_terrainBrushRadius = Clamp(
			m_terrainBrushRadius + static_cast<float>(wheel * -20.0),
			20.0f, 400.0f);
	}

	if (KeyX.down())
	{
		m_mode = (m_mode == EditMode::TrainDraw) ? EditMode::None : EditMode::TrainDraw;
		m_trainDrawStartNode = none;
	}

	if (KeyB.down())
	{
		if (m_mode == EditMode::BusRouteDraw)
		{
			m_mode           = EditMode::None;
			m_editingRouteId = -1;
		}
		else
		{
			m_mode = EditMode::BusRouteDraw;
			BusRoute newRoute;
			newRoute.headwaySec  = 120.0f;
			m_editingRouteId = -1; // TODO: SimThread 経由でバス路線追加
		}
	}

	if (m_sandboxActive && KeyV.down())
	{
		m_mode = (m_mode == EditMode::SandboxEdit) ? EditMode::None : EditMode::SandboxEdit;
		m_sandboxDragNode = none;
		m_drawStartNode   = none;
		m_rectStart       = none;
	}

	if      (m_mode == EditMode::RoadDraw)     handleRoadDraw();
	else if (m_mode == EditMode::ZonePaint)    handleZonePaint();
	else if (m_mode == EditMode::BusRouteDraw) handleBusRouteDraw();
	else if (m_mode == EditMode::TerrainEdit)  handleTerrainEdit();
	else if (m_mode == EditMode::TrainDraw)    handleTrainDraw();
	else if (m_mode == EditMode::SandboxEdit)  handleSandboxEdit();
}

void GameScene::handleRoadDraw()
{
	if (MouseL.down() && m_cursorGroundPos)
	{
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		int nodeId;
		if (!nearNode)
			nodeId = m_network.addNode(*m_cursorGroundPos);
		else
			nodeId = *nearNode;

		if (!m_drawStartNode)
		{
			m_drawStartNode = nodeId;
		}
		else
		{
			int from = *m_drawStartNode;
			if (from != nodeId)
			{
				Vec3 pA  = m_network.getNode(from)->position;
				Vec3 pB  = m_network.getNode(nodeId)->position;
				Vec3 mid = (pA + pB) / 2.0;
				m_network.addEdgeWithIntersection(from, nodeId, mid, mid, RoadType::LocalRoad, 2);
				notifyNetworkChanged();
				m_roadRenderer.markTopologyChangedAt(from, m_network);
				m_roadRenderer.markTopologyChangedAt(nodeId, m_network);
			}
			m_drawStartNode = nodeId;
		}
	}
}

void GameScene::handleZonePaint()
{
	if (!m_cursorGroundPos) return;

	if (KeyShift.pressed())
	{
		if (MouseL.down())
			m_rectStart = m_cursorGroundPos;
		if (MouseL.up() && m_rectStart)
		{
			m_zoneManager.paintZoneRect(m_world, *m_rectStart, *m_cursorGroundPos, m_paintZone);
			m_rectStart = none;
		}
	}
	else
	{
		m_rectStart = none;
		if (MouseL.pressed())
			m_zoneManager.paintZone(m_world, *m_cursorGroundPos, m_paintZone, 2);
	}
}

void GameScene::handleBusRouteDraw()
{
	if (!m_cursorGroundPos) return;
	if (m_editingRouteId < 0) return;

	if (MouseL.down())
	{
		BusStop stop;
		stop.position = *m_cursorGroundPos;

		float bestDist = 30.0f;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id < 0) continue;
			if (const auto bez = m_network.getBezier(edge.id))
			{
				const Vec3  mid = bez->positionAt(bez->totalLength * 0.5f);
				const float d   = static_cast<float>(stop.position.distanceFrom(mid));
				if (d < bestDist)
				{
					bestDist    = d;
					stop.edgeId = edge.id;
					stop.arcPos = edge.length * 0.5f;
				}
			}
		}

		// TODO: SimThread 経由でバス停追加
		// const int stopId = m_simThread.addBusStop(stop);
		// m_simThread.addStopToRoute(m_editingRouteId, stopId);
	}
}

void GameScene::handleTerrainEdit()
{
	if (!m_cursorGroundPos) return;

	const float dt    = static_cast<float>(Scene::DeltaTime());
	const float raise = MouseL.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float lower = MouseR.pressed() ? m_terrainBrushStrength * dt : 0.0f;
	const float delta = raise - lower;
	if (delta == 0.0f) return;

	const Vec3 center = *m_cursorGroundPos;

	for (Chunk* chunk : m_world.getActiveChunks())
	{
		if (!chunk) continue;

		constexpr float cellSize = static_cast<float>(CHUNK_SIZE) / HEIGHT_CELLS;
		const Vec3      origin   = chunk->worldOrigin();
		bool            modified = false;

		for (int row = 0; row <= HEIGHT_CELLS; ++row)
		{
			for (int col = 0; col <= HEIGHT_CELLS; ++col)
			{
				const Vec3   vpos = origin + Vec3{ col * cellSize, 0.0, row * cellSize };
				const double dist = Vec2{ vpos.x, vpos.z }.distanceFrom(Vec2{ center.x, center.z });
				if (dist > m_terrainBrushRadius) continue;

				const float t      = static_cast<float>(dist / m_terrainBrushRadius);
				const float weight = static_cast<float>(Math::Cos(t * Math::Pi / 2.0));
				chunk->heightMap[{ col, row }] = Clamp(
					chunk->heightMap[{ col, row }] + delta * weight,
					-200.0f, 350.0f);
				modified = true;
			}
		}

		if (modified)
		{
			chunk->dirty = true;
			chunk->updateHeightBounds();
		}
	}
}

void GameScene::handleTrainDraw()
{
	if (!m_cursorGroundPos) return;

	if (MouseL.down())
	{
		Optional<int> nearNode;
		float         bestDist = 20.0f;
		for (const auto& node : m_trainNetwork.nodes())
		{
			const float d = static_cast<float>(node.position.distanceFrom(*m_cursorGroundPos));
			if (d < bestDist)
			{
				bestDist = d;
				nearNode = node.id;
			}
		}

		int nodeId;
		if (!nearNode)
			nodeId = m_trainNetwork.addStation(*m_cursorGroundPos, U"駅");
		else
			nodeId = *nearNode;

		if (!m_trainDrawStartNode)
		{
			m_trainDrawStartNode = nodeId;
		}
		else
		{
			const int  from = *m_trainDrawStartNode;
			if (from != nodeId)
			{
				const Vec3 pa = m_trainNetwork.getNode(from)->position;
				const Vec3 pb = m_trainNetwork.getNode(nodeId)->position;
				m_trainNetwork.addEdge(from, nodeId,
					pa + (pb - pa) * (1.0 / 3),
					pa + (pb - pa) * (2.0 / 3));
			}
			m_trainDrawStartNode = nodeId;
		}
	}

	if (MouseR.down())
		m_trainDrawStartNode = none;
}

void GameScene::handleSandboxEdit()
{
	if (!m_cursorGroundPos) return;

	const Vec2 cur2D{ m_cursorGroundPos->x, m_cursorGroundPos->z };

	// ---- 左ボタンを押した瞬間：ドラッグ対象を決定 ----
	if (MouseL.down())
	{
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;

		// 1) ノード優先
		m_sandboxDragNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);

		// 2) ノードがなければ制御点を探す
		if (!m_sandboxDragNode)
		{
			float bestDist = 18.0f;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0) continue;
				const float dA = static_cast<float>(
					Vec2{ edge.ctrlA.x, edge.ctrlA.z }.distanceFrom(cur2D));
				const float dB = static_cast<float>(
					Vec2{ edge.ctrlB.x, edge.ctrlB.z }.distanceFrom(cur2D));
				if (dA < bestDist) { bestDist = dA; m_sandboxDragCtrl = CtrlDrag{ edge.id, true  }; }
				if (dB < bestDist) { bestDist = dB; m_sandboxDragCtrl = CtrlDrag{ edge.id, false }; }
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	if (MouseL.up())
	{
		// ドラッグ完了時にネットワーク変更を通知する（ドラッグ中は毎フレーム通知しない）
		if (m_sandboxDragNode || m_sandboxDragCtrl)
			notifyNetworkChanged();
		m_sandboxDragNode = none;
		m_sandboxDragCtrl = none;
	}

	// ---- 左ドラッグ中：ノード or 制御点を移動 ----
	if (MouseL.pressed())
	{
		const Vec3 delta = *m_cursorGroundPos - m_sandboxPrevCursor;

		if (m_sandboxDragNode)
		{
			RoadNode* node = m_network.getNode(*m_sandboxDragNode);
			if (node)
			{
				for (int eid : node->edgeIds)
				{
					RoadEdge* edge = m_network.getEdge(eid);
					if (!edge) continue;
					if (edge->nodeA == node->id) edge->ctrlA += delta;
					if (edge->nodeB == node->id) edge->ctrlB += delta;
					m_roadRenderer.markDirty(eid, edge->nodeA, edge->nodeB);
				}
				node->position += delta;
			}
		}
		else if (m_sandboxDragCtrl)
		{
			RoadEdge* edge = m_network.getEdge(m_sandboxDragCtrl->edgeId);
			if (edge)
			{
				if (m_sandboxDragCtrl->isA) edge->ctrlA += delta;
				else                        edge->ctrlB += delta;
				m_roadRenderer.markDirty(edge->id, edge->nodeA, edge->nodeB);
			}
		}

		m_sandboxPrevCursor = *m_cursorGroundPos;
	}

	// ---- 右クリック：削除 ----
	if (MouseR.down())
	{
		auto nearNode = m_network.findNodeNear(*m_cursorGroundPos, 20.0f);
		if (nearNode)
		{
			// 削除前に影響ノードを収集
			Array<int> neighborNodes;
			if (const RoadNode* node = m_network.getNode(*nearNode))
			{
				for (int eid : node->edgeIds)
				{
					m_roadRenderer.markDirty(eid);
					if (const RoadEdge* e = m_network.getEdge(eid))
					{
						const int other = (e->nodeA == *nearNode) ? e->nodeB : e->nodeA;
						neighborNodes << other;
					}
				}
			}
			m_network.removeNode(*nearNode);
			notifyNetworkChanged();
			for (const int nid : neighborNodes)
				m_roadRenderer.markTopologyChangedAt(nid, m_network);
		}
		else
		{
			int   bestId   = -1;
			float bestDist = 30.0f;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id < 0) continue;
				const auto bez = m_network.getBezier(edge.id);
				if (!bez) continue;
				for (int k = 0; k <= 4; ++k)
				{
					const Vec3  pt   = bez->evaluate(k * 0.25f);
					const float dist = static_cast<float>(
						Vec2{ pt.x, pt.z }.distanceFrom(cur2D));
					if (dist < bestDist) { bestDist = dist; bestId = edge.id; }
				}
			}
			if (bestId >= 0)
			{
				// 削除前に両端ノードを取得
				int nA = -1, nB = -1;
				if (const RoadEdge* e = m_network.getEdge(bestId))
				{ nA = e->nodeA; nB = e->nodeB; }
				m_roadRenderer.markDirty(bestId, nA, nB);
				m_network.removeEdge(bestId);
				notifyNetworkChanged();
				if (nA >= 0) m_roadRenderer.markTopologyChangedAt(nA, m_network);
				if (nB >= 0) m_roadRenderer.markTopologyChangedAt(nB, m_network);
			}
		}
	}
}

void GameScene::updateCursor()
{
	const Ray    ray  = m_camera.screenToRay(Vec2{ Cursor::Pos() });
	const Float3 orig = ray.origin;
	const Float3 dir  = ray.direction;

	if (dir.y >= 0.0f)
	{
		m_cursorGroundPos = none;
		return;
	}

	constexpr float kStep    = 8.0f;
	constexpr float kMaxDist = 8000.0f;

	float tPrev    = 0.0f;
	bool  hitFound = false;

	for (float t = kStep; t < kMaxDist; t += kStep)
	{
		const float px = orig.x + dir.x * t;
		const float pz = orig.z + dir.z * t;
		const float py = orig.y + dir.y * t;
		const float th = m_world.sampleHeight(px, pz);

		if (py <= th)
		{
			float tLo = tPrev, tHi = t;
			for (int i = 0; i < 8; ++i)
			{
				const float tMid = (tLo + tHi) * 0.5f;
				const float mx   = orig.x + dir.x * tMid;
				const float mz   = orig.z + dir.z * tMid;
				const float my   = orig.y + dir.y * tMid;
				if (my <= m_world.sampleHeight(mx, mz))
					tHi = tMid;
				else
					tLo = tMid;
			}
			const float tf = (tLo + tHi) * 0.5f;
			const float fx = orig.x + dir.x * tf;
			const float fz = orig.z + dir.z * tf;
			m_cursorGroundPos = Vec3{ fx, static_cast<double>(m_world.sampleHeight(fx, fz)), fz };
			hitFound = true;
			break;
		}

		tPrev = t;
	}

	if (!hitFound)
		m_cursorGroundPos = m_camera.screenToGround(Vec2{ Cursor::Pos() });
}

String GameScene::modeString() const
{
	switch (m_mode)
	{
	case EditMode::RoadDraw:
		return U"道路描画モード（左クリックで配置）";
	case EditMode::ZonePaint:
		return U"ゾーン塗り [{}] 左:ブラシ Shift+左ドラッグ:矩形  0〜6:種別変更"_fmt(zoneName(m_paintZone));
	case EditMode::BusRouteDraw:
		return U"バス路線描画モード（左クリックでバス停配置・Bキーで確定）";
	case EditMode::TerrainEdit:
		return U"地形編集モード（左:盛土 右:掘削 Ctrl+ホイール:ブラシサイズ）";
	case EditMode::TrainDraw:
		return U"線路描画モード（左クリックで駅配置・連結 右クリックで中断）";
	case EditMode::SandboxEdit:
		return U"サンドボックス編集（左ドラッグ:ノード移動 右クリック:削除）";
	default:
		return U"";
	}
}
