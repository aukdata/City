#include <thread>
#include "GameScene.hpp"
#include "../save/RoadBinary.hpp"
#include "../save/GuideSignStorage.hpp"
#include "../save/SaveTransaction.hpp"
#include "../save/DevelopmentSnapshot.hpp"
#include "../save/WorldSnapshotValidation.hpp"

namespace
{
	/// @brief Reject missing or invalid required metadata before current-world restoration.
	bool verifyCurrentMetadata(const FilePath& root, const JSON& meta)
	{
		const JSON economy = JSON::Load(root + U"/global/economy.json");
		const JSON districts = JSON::Load(root + U"/global/districts.json");
		if (!economy || !districts || !meta.contains(U"zoneDevelopment") || !meta.contains(U"railway")
			|| meta[U"worldChunks"].getOr<int>(0) != WORLD_CHUNKS)
		{
			return false;
		}
		for (const String key : { U"funds", U"happiness" })
		{
			const auto value = economy[key].getOpt<double>();
			if (!value || !IsFinite(*value)) { return false; }
		}
		const auto population = economy[U"population"].getOpt<int>();
		const auto count = districts[U"count"].getOpt<int>();
		constexpr int kMaxDistricts = WORLD_CHUNKS * WORLD_CHUNKS;
		if (!population || *population < 0 || !count || *count < 0 || *count > kMaxDistricts) { return false; }
		for (int index = 0; index < *count; ++index)
		{
			const auto kind = districts[U"type_{}"_fmt(index)].getOpt<int>();
			if (!kind || *kind < 0 || *kind > static_cast<int>(MapGenerator::SettlementKind::RuralSettlement)
				|| !districts[U"name_{}"_fmt(index)].getOpt<String>()) { return false; }
			for (const String field : { U"cx", U"cy", U"radius", U"score" })
			{
				const auto value = districts[U"{}_{}"_fmt(field,index)].getOpt<double>();
				if (!value || !IsFinite(*value)) { return false; }
			}
		}
		for (const String key : { U"gameNow", U"cameraFocusX", U"cameraFocusY", U"cameraFocusZ",
			U"cameraDistance", U"cameraYaw", U"cameraPitch" })
		{
			const auto value = meta[key].getOpt<double>();
			if (!value || !IsFinite(*value)) { return false; }
		}
		RoadConstruction::ClearanceLedger clearance;
		return clearance.load(root + U"/global/construction_clearance.json");
	}
	/// @brief Validate required terrain files; staged saves also compare every source sample.
	bool verifyTerrainSnapshot(const FilePath& root, const World* source)
	{
		constexpr int kGridSize = HEIGHT_CELLS + 1;
		constexpr size_t kSampleCount = kGridSize * kGridSize;
		constexpr int64 kTerrainBytes = sizeof(int32) + kSampleCount * sizeof(float);
		Array<float> heights(kSampleCount);
		for (int y=0; y<WORLD_CHUNKS; ++y)
		{
			for (int x=0; x<WORLD_CHUNKS; ++x)
			{
				const FilePath path = U"{}/chunks/{}_{}/terrain.bin"_fmt(root,x,y);
				BinaryReader reader{path};
				int32 gridSize=0;
				if (!reader || reader.size()!=kTerrainBytes || !reader.read(gridSize) || gridSize!=kGridSize
					|| reader.read(heights.data(),kSampleCount*sizeof(float))!=static_cast<int64>(kSampleCount*sizeof(float)))
				{
					DBG_LOG(U"[Save/Load] Missing or corrupt terrain: {}"_fmt(path));
					return false;
				}
				const Chunk* chunk = source ? source->getChunk({x,y}) : nullptr;
				if (source && (!chunk || chunk->heightMap.num_elements()!=kSampleCount)) { return false; }
				for (size_t index=0; index<kSampleCount; ++index)
				{
					if (!IsFinite(heights[index]) || (chunk && heights[index]!=chunk->heightMap.data()[index]))
					{
						DBG_LOG(U"[Save/Load] Invalid or changed terrain sample: {} index={}"_fmt(path,index));
						return false;
					}
				}
			}
		}
		return true;
	}
}

/// @file
/// @brief ゲーム状態の保存と復元。書込み・再読込み検証・公開の順序を保存サービスと協調して守る。

void GameScene::saveGame()
{
	if (getData().saveName.isEmpty())
	{
		getData().saveName = U"default";
	}
	if (getData().saveName.contains(U'/') || getData().saveName.contains(U'\\')
		|| getData().saveName == U"." || getData().saveName == U"..")
	{
		DebugLog::print(U"[Save] Invalid save name: {}"_fmt(getData().saveName));
		m_saveStatusNotice.show(U"セーブできません", U"保存先の名前が不正です。別の保存先を使用してください", false);
		return;
	}

	const FilePath saveRoot = U"saves/{}"_fmt(getData().saveName);
	const SaveResult result = SaveTransaction::commit(saveRoot,
		[this](const FilePath& temporaryDirectory)
		{
			return writeGameSnapshot(temporaryDirectory);
		},
		[this](const FilePath& temporaryDirectory)
		{
			return verifyGameSnapshot(temporaryDirectory);
		});

	if (result)
	{
		DebugLog::print(U"[Save] Saved atomically to {}"_fmt(result.path));
		m_saveStatusNotice.show(U"セーブしました", U"現在の街を保存しました", true);
	}
	else
	{
		DebugLog::print(U"[Save] Failed: {} ({})"_fmt(result.message, result.path));
		String message = result.message;
		if (result.error == SaveError::WriteFailed || result.error == SaveError::VerificationFailed)
		{
			message += U"\n既存のセーブは変更していません";
		}
		m_saveStatusNotice.show(U"セーブできません", message, false);
	}
}

SaveResult GameScene::writeGameSnapshot(const FilePath& saveRoot) const
{
	constexpr int kSaveVersion = 4;
	if (!WorldSnapshotValidation::references(m_world,m_network))
	{
		return SaveResult::failed(SaveError::VerificationFailed,U"建物の道路・敷地参照が不正です。既存セーブは保持されます",saveRoot);
	}
	const FilePath globalDirectory = saveRoot + U"/global";
	if (!FileSystem::CreateDirectories(globalDirectory))
	{
		return SaveResult::failed(SaveError::WriteFailed,
			U"global ディレクトリを作成できません", globalDirectory);
	}

	JSON meta;
	meta[U"version"] = kSaveVersion;
	meta[U"roadGeometryVersion"] = 1;
	meta[U"zoneDevelopment"] = m_zoneManager.saveState(m_world);
	meta[U"railway"] = m_trainNetwork.saveState();
	meta[U"seed"] = getData().seed;
	meta[U"generation"] = getData().generation.save();
	meta[U"worldChunks"] = WORLD_CHUNKS;
	meta[U"gameNow"] = m_clock.now;
	meta[U"timeScale"] = static_cast<int>(m_clock.speed);
	meta[U"nextNodeId"] = m_network.nextNodeId();
	meta[U"nextEdgeId"] = m_network.nextEdgeId();
	meta[U"cameraFocusX"] = m_camera.focusPoint().x;
	meta[U"cameraFocusY"] = m_camera.focusPoint().y;
	meta[U"cameraFocusZ"] = m_camera.focusPoint().z;
	meta[U"cameraDistance"] = m_camera.distance();
	meta[U"cameraYaw"] = m_camera.yaw();
	meta[U"cameraPitch"] = m_camera.pitch();
	if (!meta.save(saveRoot + U"/meta.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"meta.json を保存できません", saveRoot);
	}

	JSON economy;
	economy[U"funds"] = m_economy.funds;
	economy[U"population"] = m_economy.population;
	economy[U"happiness"] = m_economy.happiness;
	if (!economy.save(globalDirectory + U"/economy.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"economy.json を保存できません", saveRoot);
	}
	String roadError;
	if (!RoadBinary::writeGlobal(globalDirectory + U"/roads.bin", m_network, &roadError))
	{
		return SaveResult::failed(SaveError::WriteFailed, roadError, saveRoot);
	}
	if (!m_clearanceLedger.save(globalDirectory + U"/construction_clearance.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed,U"撤去した区画を保存できません",globalDirectory);
	}
	if (!GuideSignStorage::writeJson(globalDirectory + U"/guide_signs.json", m_network))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"guide_signs.json を保存できません", saveRoot);
	}

	JSON districts;
	districts[U"count"] = static_cast<int>(m_districts.size());
	districts[U"morphologyVersion"] = 2;
	for (int index = 0; index < static_cast<int>(m_districts.size()); ++index)
	{
		const auto& settlement = m_districts[index];
		districts[U"type_{}"_fmt(index)] = static_cast<int>(settlement.kind);
		districts[U"cx_{}"_fmt(index)] = settlement.center.x;
		districts[U"cy_{}"_fmt(index)] = settlement.center.y;
		districts[U"radius_{}"_fmt(index)] = settlement.radius;
		districts[U"score_{}"_fmt(index)] = settlement.score;
		districts[U"name_{}"_fmt(index)] = settlement.name;
		districts[U"reading_{}"_fmt(index)] = settlement.reading;
		const auto& plan=settlement.plan;
		districts[U"structure_{}"_fmt(index)]=UrbanStructure::saveLayout(plan);
		districts[U"origin_{}"_fmt(index)]=static_cast<int>(plan.origin);
		districts[U"ruralForm_{}"_fmt(index)]=static_cast<int>(plan.ruralForm);
		districts[U"halfX_{}"_fmt(index)]=plan.halfExtent.x;
		districts[U"halfZ_{}"_fmt(index)]=plan.halfExtent.y;
		districts[U"axisX_{}"_fmt(index)]=settlement.gridAxisX.x;
		districts[U"axisZ_{}"_fmt(index)]=settlement.gridAxisX.y;
		districts[U"railway_{}"_fmt(index)]=plan.station.has_value();
		districts[U"frontage_{}"_fmt(index)]=plan.frontageRoads;
		districts[U"oldX_{}"_fmt(index)]=plan.oldCore.x;
		districts[U"oldZ_{}"_fmt(index)]=plan.oldCore.y;
		if (plan.station)
		{
			districts[U"stationX_{}"_fmt(index)]=plan.station->x;
			districts[U"stationZ_{}"_fmt(index)]=plan.station->y;
		}
		districts[U"civic_{}"_fmt(index)]=plan.civic.has_value();
		const RectF civic=plan.civic.value_or(RectF{0,0,0,0});
		districts[U"civicX_{}"_fmt(index)]=civic.x; districts[U"civicZ_{}"_fmt(index)]=civic.y;
		districts[U"civicW_{}"_fmt(index)]=civic.w; districts[U"civicH_{}"_fmt(index)]=civic.h;
		districts[U"industryX_{}"_fmt(index)]=plan.industry.x; districts[U"industryZ_{}"_fmt(index)]=plan.industry.y;
		districts[U"industryW_{}"_fmt(index)]=plan.industry.w; districts[U"industryH_{}"_fmt(index)]=plan.industry.h;
		districts[U"fringe_{}"_fmt(index)]=static_cast<int>(plan.fringeStreets.size());
		for (size_t street=0;street<plan.fringeStreets.size();++street)
		{
			const auto& line=plan.fringeStreets[street];
			districts[U"fringeAX_{}_{}"_fmt(index,street)]=line.begin.x;
			districts[U"fringeAZ_{}_{}"_fmt(index,street)]=line.begin.y;
			districts[U"fringeBX_{}_{}"_fmt(index,street)]=line.end.x;
			districts[U"fringeBZ_{}_{}"_fmt(index,street)]=line.end.y;
		}
		districts[U"homes_{}"_fmt(index)]=static_cast<int>(plan.ruralHomes.size());
		for (size_t home=0;home<plan.ruralHomes.size();++home)
		{
			districts[U"homeX_{}_{}"_fmt(index,home)]=plan.ruralHomes[home].x;
			districts[U"homeZ_{}_{}"_fmt(index,home)]=plan.ruralHomes[home].y;
		}
	}
	if (!districts.save(globalDirectory + U"/districts.json"))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"districts.json を保存できません", saveRoot);
	}

	const FilePath chunksDirectory = saveRoot + U"/chunks";
	for (int chunkY = 0; chunkY < WORLD_CHUNKS; ++chunkY)
	{
		for (int chunkX = 0; chunkX < WORLD_CHUNKS; ++chunkX)
		{
			const Chunk* chunk = m_world.getChunk(Point{ chunkX, chunkY });
			if (!chunk || chunk->heightMap.isEmpty())
			{
				continue;
			}

			const FilePath chunkDirectory = U"{}/{}_{}"_fmt(chunksDirectory, chunkX, chunkY);
			if (!FileSystem::CreateDirectories(chunkDirectory))
			{
				return SaveResult::failed(SaveError::WriteFailed,
					U"チャンクディレクトリを作成できません", chunkDirectory);
			}
			BinaryWriter writer{ chunkDirectory + U"/terrain.bin" };
			if (!writer)
			{
				return SaveResult::failed(SaveError::WriteFailed,
					U"terrain.bin を作成できません", chunkDirectory);
			}

			const int gridSize = HEIGHT_CELLS + 1;
			writer.write(static_cast<int32>(gridSize));
			for (int row = 0; row < gridSize; ++row)
			{
				for (int col = 0; col < gridSize; ++col)
				{
					writer.write(chunk->heightMap[{ col, row }]);
				}
			}
			BinaryWriter landWriter{ chunkDirectory + U"/land_patches.bin" };
			if (!landWriter)
			{
				return SaveResult::failed(SaveError::WriteFailed,
					U"land_patches.bin を作成できません", chunkDirectory);
			}
			landWriter.write(static_cast<uint32>(chunk->landPatches.size()));
			for (const LandPatch& patch : chunk->landPatches)
			{
				landWriter.write(static_cast<int32>(patch.id));
				landWriter.write(static_cast<uint8>(patch.type));
				landWriter.write(patch.elevationOffset);
				landWriter.write(patch.materialVariant);
				landWriter.write(static_cast<int64>(patch.sourceParcelKey));
				landWriter.write(static_cast<uint32>(patch.polygon.size()));
				for (const Vec2& point : patch.polygon)
				{
					landWriter.write(static_cast<float>(point.x));
					landWriter.write(static_cast<float>(point.y));
				}
			}
		}
	}
	if (!DevelopmentSnapshot::write(globalDirectory + U"/development.bin", m_world))
	{
		return SaveResult::failed(SaveError::WriteFailed, U"都市状態を保存できません", globalDirectory);
	}
	return SaveResult::succeeded(saveRoot);
}

SaveResult GameScene::verifyGameSnapshot(const FilePath& saveRoot) const
{
	constexpr int kSaveVersion = 4;
	const FilePath metaPath = saveRoot + U"/meta.json";
	const FilePath economyPath = saveRoot + U"/global/economy.json";
	const FilePath roadsPath = saveRoot + U"/global/roads.bin";
	const JSON meta = JSON::Load(metaPath);
	if (!meta)
	{
		return SaveResult::failed(SaveError::MissingData, U"meta.json がありません", metaPath);
	}
	const int version = meta[U"version"].getOr<int>(0);
	if (version <= 0 || version > kSaveVersion)
	{
		return SaveResult::failed(SaveError::UnsupportedVersion,
			U"未対応のセーブバージョンです: {}"_fmt(version), metaPath);
	}
	if (!JSON::Load(economyPath))
	{
		return SaveResult::failed(SaveError::CorruptData,
			U"economy.json が欠損または破損しています", economyPath);
	}
	const FilePath clearancePath = saveRoot + U"/global/construction_clearance.json";
	if (FileSystem::IsFile(clearancePath))
	{
		RoadConstruction::ClearanceLedger verification;
		if (!verification.load(clearancePath))
			return SaveResult::failed(SaveError::CorruptData,U"撤去区画の記録が破損しています",clearancePath);
	}
	if (!FileSystem::IsFile(roadsPath) || FileSystem::FileSize(roadsPath) <= 0)
	{
		return SaveResult::failed(SaveError::MissingData,
			U"roads.bin が欠損しています", roadsPath);
	}
	if (version >= 4 && (!verifyCurrentMetadata(saveRoot, meta) || !verifyTerrainSnapshot(saveRoot, &m_world)
		|| !DevelopmentSnapshot::matches(saveRoot + U"/global/development.bin", m_world)))
	{
		return SaveResult::failed(SaveError::VerificationFailed, U"都市状態の再読込み検証に失敗しました", saveRoot);
	}
	return SaveResult::succeeded(saveRoot);
}

void GameScene::loadTerrainChunks(const String& saveRoot, Stopwatch& step)
{
	// 地形チャンクは並列に読み戻し、欠損しているものだけ再生成してロード失敗を吸収する。
	// 地形データ（チャンクごとに並列読み込み + バルク I/O）
	const int totalChunks = WORLD_CHUNKS * WORLD_CHUNKS;
	setLoadingStatus(U"地形データ読み込み中...");
	{
		Array<HeightMapResult> buffer(totalChunks);
		std::atomic<int> counter{ 0 };
		std::atomic<int> loadedAtomic{ 0 };
		const int nThreads = Max(1, static_cast<int>(std::thread::hardware_concurrency()));
		Array<std::thread> threads;
		threads.reserve(nThreads);

		for (int t = 0; t < nThreads; ++t)
		{
			threads.emplace_back([this, &buffer, &counter, &loadedAtomic, totalChunks, &saveRoot]()
			{
				while (true)
				{
					const int idx = counter.fetch_add(1);
					if (idx >= totalChunks) break;
					const int cx = idx % WORLD_CHUNKS;
					const int cy = idx / WORLD_CHUNKS;

					const String terrainPath = U"{}/chunks/{}_{}/terrain.bin"_fmt(saveRoot, cx, cy);
					bool loaded = false;

					if (FileSystem::Exists(terrainPath))
					{
						BinaryReader r{ terrainPath };
						if (r)
						{
							int32 gridSize = 0;
							if (r.read(gridSize) && gridSize == HEIGHT_CELLS + 1)
							{
								HeightMapResult hmr;
								hmr.heightMap = Grid<float>(gridSize, gridSize);
								// バルク読み込み: gridSize² 個の float を1 回で取得
								const size_t cellCount = static_cast<size_t>(gridSize) * gridSize;
								const int64 expectedBytes = static_cast<int64>(cellCount * sizeof(float));
								if (r.read(hmr.heightMap.data(), static_cast<size_t>(expectedBytes)) == expectedBytes)
								{
									// min/max は読み込み完了後にまとめて算出
									float mn =  1e30f, mx = -1e30f;
									const float* p = hmr.heightMap.data();
									for (size_t i = 0; i < cellCount; ++i) { mn = Min(mn, p[i]); mx = Max(mx, p[i]); }
									hmr.heightMin = mn;
									hmr.heightMax = mx;
									buffer[idx] = std::move(hmr);
									loaded = true;
								}
							}
						}
					}

					if (!loaded)
					{
						buffer[idx] = m_world.buildHeightMap(Point{ cx, cy });
					}
					const int done = loadedAtomic.fetch_add(1) + 1;
					m_genProgress.store(static_cast<float>(done) / static_cast<float>(totalChunks) * 0.7f);
				}
			});
		}
		for (auto& th : threads) th.join();

		// installChunkDirect は m_world 内部状態を変更するためメインスレッドで直列に
		for (int idx = 0; idx < totalChunks; ++idx)
		{
			const int cx = idx % WORLD_CHUNKS;
			const int cy = idx / WORLD_CHUNKS;
			m_world.installChunkDirect(Point{ cx, cy }, std::move(buffer[idx]));
			Chunk* chunk = m_world.getChunk(Point{ cx, cy });
			if (!chunk) continue;
			const String landPath = U"{}/chunks/{}_{}/land_patches.bin"_fmt(saveRoot, cx, cy);
			if (!FileSystem::Exists(landPath)) continue;
			BinaryReader landReader{ landPath };
			if (!landReader) continue;
			uint32 patchCount = 0;
			if (!landReader.read(patchCount) || patchCount > 4096) continue;
			chunk->landPatches.clear();
			for (uint32 p = 0; p < patchCount; ++p)
			{
				LandPatch patch;
				int32 id = -1;
				uint8 type = 0;
				int64 sourceParcelKey = -1;
				uint32 pointCount = 0;
				if (!landReader.read(id) || !landReader.read(type) || !landReader.read(patch.elevationOffset)
					|| !landReader.read(patch.materialVariant) || !landReader.read(sourceParcelKey)
					|| !landReader.read(pointCount) || pointCount > 64)
				{
					chunk->landPatches.clear();
					break;
				}
				patch.id = static_cast<int>(id);
				patch.type = static_cast<LandPatchType>(type);
				patch.sourceParcelKey = sourceParcelKey;
				for (uint32 pointIndex = 0; pointIndex < pointCount; ++pointIndex)
				{
					float px = 0.0f, py = 0.0f;
					if (!landReader.read(px) || !landReader.read(py))
					{
						patch.polygon.clear();
						break;
					}
					patch.polygon << Vec2{ px, py };
				}
				if (patch.polygon.size() >= 3)
				{
					chunk->landPatches << patch;
				}
			}
		}
	}

	Console << U"[Load] terrain: {:.0f}ms"_fmt(step.msF());
	step.restart();
}

bool GameScene::loadGame()
{
	const Stopwatch loadTotal{ StartImmediately::Yes };
	Stopwatch step{ StartImmediately::Yes };

	// セーブ一式を読み戻し、道路・地区・建物・時計・カメラまでプレイ直前の状態に復元する。
	const String saveRoot = U"saves/{}"_fmt(getData().saveName);

	// meta.json
	const JSON meta = JSON::Load(U"{}/meta.json"_fmt(saveRoot));
	if (!meta) { return false; }
	const int saveVersion = meta[U"version"].getOr<int>(0);
	if (saveVersion < 1 || saveVersion > 4)
	{
		DBG_LOG(U"[Load] Unsupported save version: {}"_fmt(saveVersion));
		return false;
	}
	const bool restoreSnapshot = saveVersion >= 4;
	if (restoreSnapshot)
	{
		if (!verifyCurrentMetadata(saveRoot, meta))
		{
			DBG_LOG(U"[Load] Missing required current-save metadata");
			return false;
		}
		if (!verifyTerrainSnapshot(saveRoot, nullptr)) { return false; }
	}

	getData().seed    = meta[U"seed"].get<uint64>();
	getData().generation = GenerationOptions::load(meta[U"generation"]);
	m_worldRenderer.setWoodlandEnabled(getData().generation.enabled(GenerationOptions::Element::Trees));
	const double gameNow    = meta[U"gameNow"].get<double>();
	const int    timeScale  = meta[U"timeScale"].get<int>();
	const int    nextNodeId = meta[U"nextNodeId"].get<int>();
	const int    nextEdgeId = meta[U"nextEdgeId"].get<int>();
	const double focusX     = meta[U"cameraFocusX"].get<double>();
	const double focusY     = meta[U"cameraFocusY"].get<double>();
	const double focusZ     = meta[U"cameraFocusZ"].get<double>();
	const float  camDist    = meta[U"cameraDistance"].getOr<float>(600.0f);
	const float  camYaw     = meta[U"cameraYaw"].getOr<float>(0.0f);
	const float  camPitch   = meta[U"cameraPitch"].getOr<float>(static_cast<float>(40.0_deg));

	m_world.setGenerationParams(getData().seed,
		WORLD_SIZE,
		WORLD_SIZE);
	if (getData().generation.enabled(GenerationOptions::Element::Rivers))
	{
		m_world.generateRivers();
	}
	m_world.reserveChunks();
	Console << U"[Load] meta+init: {:.0f}ms"_fmt(step.msF());
	step.restart();

	loadTerrainChunks(saveRoot, step);

	// 経済
	if (const JSON eco = JSON::Load(U"{}/global/economy.json"_fmt(saveRoot)))
	{
		m_economy.funds      = eco[U"funds"].get<double>();
		m_economy.population = eco[U"population"].get<int>();
		m_economy.happiness  = eco[U"happiness"].get<double>();
	}

	// 道路ネットワーク
	{
		const String roadPath = U"{}/global/roads.bin"_fmt(saveRoot);
		const bool roadOk = RoadBinary::readGlobal(roadPath, m_network, restoreSnapshot);
		if (!roadOk)
		{
			DBG_LOG(U"[Load] Invalid road snapshot: {}"_fmt(roadPath));
			return false;
		}
		// 案内標識（独立 JSON。テクスチャは render 時に prepareGuideSignTextures で合成）
		m_network.clearGuideSigns();
		GuideSignStorage::readJson(U"{}/global/guide_signs.json"_fmt(saveRoot), m_network);
		Console << U"[Load] roads: " << (roadOk ? U"OK" : U"FAILED (format mismatch? re-save needed)")
		        << U" nodes=" << m_network.nodes().size()
		        << U" edges=" << m_network.edges().size();
	}
	m_network.setNextIds(nextNodeId, nextEdgeId);

	m_genProgress.store(0.8f);
	setLoadingStatus(U"道路・経済データ復元完了");
	Console << U"[Load] economy+roads: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 集落
	m_districts.clear();
	m_castleTownCenters.clear();
	if (const JSON dist = JSON::Load(U"{}/global/districts.json"_fmt(saveRoot)))
	{
		const int count = dist[U"count"].get<int>();
		Array<MapGenerator::Settlement> settlements;
		for (int i = 0; i < count; ++i)
		{
			MapGenerator::Settlement s;
			s.kind   = static_cast<MapGenerator::SettlementKind>(dist[U"type_{}"_fmt(i)].get<int>());
			s.center = Vec2{ dist[U"cx_{}"_fmt(i)].get<double>(), dist[U"cy_{}"_fmt(i)].get<double>() };
			s.name   = dist[U"name_{}"_fmt(i)].get<String>();
			const String radiusKey = U"radius_{}"_fmt(i);
			if (dist.hasElement(radiusKey))
				s.radius = dist[radiusKey].get<float>();
			else
			{
				// 旧セーブとの互換: 種別からデフォルト半径を復元
				s.radius = (s.kind == MapGenerator::SettlementKind::RegionalCity)   ? 700.0f
				         : (s.kind == MapGenerator::SettlementKind::LocalTown) ? 300.0f
				                                                             : 150.0f;
			}
			const String scoreKey = U"score_{}"_fmt(i);
			if (dist.hasElement(scoreKey))
				s.score = dist[scoreKey].get<float>();
			const String readingKey = U"reading_{}"_fmt(i);
			if (dist.hasElement(readingKey))
				s.reading = PlaceNameFormat::capitalizeReading(dist[readingKey].get<String>());
			const auto origin=static_cast<UrbanMorphology::Origin>(dist[U"origin_{}"_fmt(i)].getOr<int>(7));
			UrbanMorphology::Site site;
			const double halfZ=dist[U"halfZ_{}"_fmt(i)].getOr<double>(170);
			site.shoreDistance=halfZ+45;
			s.plan=UrbanMorphology::makePlan(origin,static_cast<uint8>(s.kind),site,
				UrbanMorphology::mix(getData().seed ^ (static_cast<uint64>(i)*0x9e3779b97f4a7c15ULL)),dist[U"railway_{}"_fmt(i)].getOr<bool>(false));
			s.plan.ruralForm=static_cast<UrbanMorphology::RuralForm>(dist[U"ruralForm_{}"_fmt(i)].getOr<int>(0));
			s.plan.halfExtent={dist[U"halfX_{}"_fmt(i)].getOr<double>(230),halfZ};
			s.plan.frontageRoads=dist[U"frontage_{}"_fmt(i)].getOr<bool>(false);
			s.plan.oldCore={dist[U"oldX_{}"_fmt(i)].getOr<double>(0),dist[U"oldZ_{}"_fmt(i)].getOr<double>(0)};
			if (s.plan.station) { s.plan.station=Vec2{dist[U"stationX_{}"_fmt(i)].getOr<double>(0),dist[U"stationZ_{}"_fmt(i)].getOr<double>(0)}; }
			if (dist[U"civic_{}"_fmt(i)].getOr<bool>(false))
			{
				s.plan.civic=RectF{dist[U"civicX_{}"_fmt(i)].get<double>(),dist[U"civicZ_{}"_fmt(i)].get<double>(),dist[U"civicW_{}"_fmt(i)].get<double>(),dist[U"civicH_{}"_fmt(i)].get<double>()};
			}
			s.plan.industry=RectF{dist[U"industryX_{}"_fmt(i)].getOr<double>(0),dist[U"industryZ_{}"_fmt(i)].getOr<double>(0),dist[U"industryW_{}"_fmt(i)].getOr<double>(0),dist[U"industryH_{}"_fmt(i)].getOr<double>(0)};
			for (int home=0;home<dist[U"homes_{}"_fmt(i)].getOr<int>(0);++home)
			{
				s.plan.ruralHomes << Vec2{dist[U"homeX_{}_{}"_fmt(i,home)].get<double>(),dist[U"homeZ_{}_{}"_fmt(i,home)].get<double>()};
			}
			for (int street=0;street<dist[U"fringe_{}"_fmt(i)].getOr<int>(0);++street)
			{
				s.plan.fringeStreets << Line{
					Vec2{dist[U"fringeAX_{}_{}"_fmt(i,street)].get<double>(),dist[U"fringeAZ_{}_{}"_fmt(i,street)].get<double>()},
					Vec2{dist[U"fringeBX_{}_{}"_fmt(i,street)].get<double>(),dist[U"fringeBZ_{}_{}"_fmt(i,street)].get<double>()}};
			}
			UrbanStructure::restoreLayout(s.plan,dist[U"structure_{}"_fmt(i)]);
			s.gridAxisX={dist[U"axisX_{}"_fmt(i)].getOr<double>(1),dist[U"axisZ_{}"_fmt(i)].getOr<double>(0)};
			s.gridAxisZ={-s.gridAxisX.y,s.gridAxisX.x};
			settlements << s;
		}
		addDistricts(settlements);
	}

	// Upgrade only the loaded in-memory graph; saving remains an explicit user operation.
	if (!meta.hasElement(U"roadGeometryVersion") || meta[U"roadGeometryVersion"].getOr<int>(0) < 1)
	{
		setLoadingStatus(U"重複する道路の接続を修復中...");
		m_network.consolidateOverlappingRoads();
	}
	m_genProgress.store(0.9f);
	setLoadingStatus(U"ゾーン・建物を復元中...");

	if (meta.hasElement(U"railway"))
	{
		m_trainNetwork.bind(&m_network);
		if (!m_trainNetwork.restoreState(meta[U"railway"]))
		{
			DBG_LOG(U"[Load] railway snapshot is invalid");
			return false;
		}
	}
	else if (getData().generation.enabled(GenerationOptions::Element::Railway))
	{
		MapGenerator::setupTrain(m_trainNetwork, m_world, m_districts, &m_network);
	}
	m_districtHierarchy.generate(m_world,m_districts);
	if (!restoreSnapshot) { m_network.recomputeAllAutoSigns(); }
	m_roadRenderer.setMunicipalityLookup([this](Vec2 point)
	{
		const int id=m_districtHierarchy.at(point,0);
		return id>=0 ? m_districtHierarchy.areas[id].name : U"";
	});
	if (restoreSnapshot)
	{
		if (!DevelopmentSnapshot::read(saveRoot + U"/global/development.bin", m_world))
		{
			DBG_LOG(U"[Load] Missing or corrupt required development snapshot");
			return false;
		}
		if (!WorldSnapshotValidation::references(m_world,m_network))
		{
			DBG_LOG(U"[Load] Invalid building/road/parcel references");
			return false;
		}
	}
	else
	{
		applyZonesGlobal();
		placeInitialBuildings(true);
	}
	const FilePath clearancePath = saveRoot + U"/global/construction_clearance.json";
	if (FileSystem::IsFile(clearancePath))
	{
		if (!m_clearanceLedger.load(clearancePath)) { return false; }
		if (!restoreSnapshot) { m_clearanceLedger.apply(m_world); }
	}
	else if (restoreSnapshot) { return false; }
	// Current snapshots already contain cleared cells and fitted terrain.
	m_restoreConstructionSites = !restoreSnapshot;
	registerGuideDestinations();

	m_roadRenderer.invalidateAllCaches();

	m_clock.now   = gameNow;
	m_clock.speed = static_cast<TimeSpeed>(timeScale);
	m_clock.syncCalendar();

	m_camera.setState(Vec3{ focusX, focusY, focusZ }, camDist, camYaw, camPitch);


	m_world.update(m_camera.focusPoint());

	Console << U"[Load] finish: {:.0f}ms"_fmt(step.msF());
	m_genProgress.store(1.0f);
	if (!restoreSnapshot)
	{
		generateLandPatches(true);
		m_clearanceLedger.apply(m_world);
		migrateLegacyBuildingFrontageReferences();
		refreshBuildingAnglesFromEdges();
	}
	if (meta.contains(U"zoneDevelopment")) { m_zoneManager.restoreState(meta[U"zoneDevelopment"],m_world,restoreSnapshot); }
	m_cityConstraintValidationPassed = validateGeneratedCityConstraints();
	startSimThread();
	Console << U"[Load] TOTAL: {:.0f}ms from {}"_fmt(loadTotal.msF(), saveRoot);
	return true;
}
