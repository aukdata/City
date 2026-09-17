#include <thread>
#include "GameScene.hpp"
#include "../gen/SettlementDevelopment.hpp"
#include "../gen/WaterCrossings.hpp"
#include "../gen/RoadTerrainFit.hpp"
#include "../gen/RoadDesignLimits.hpp"
#include "../gen/RoadVerticalAlignment.hpp"
#include "../gen/StreetBlocks.hpp"
#include "../gen/DistrictRoads.hpp"
#include "../gen/VillageConnections.hpp"
#include "../save/RoadBinary.hpp"

/// @file
/// @brief 初期生成の手順と生成済みデータの接続。アルゴリズム本体は gen のサービスに委譲する。

namespace
{
	// 生成の各段階が表示上で占める進捗区間。ワーカの順序と同じ場所で管理する。
	constexpr float kProgressTerrain      = 0.00f;
	constexpr float kProgressSettlement   = 0.03f;
	constexpr float kProgressRoads        = 0.25f;
	constexpr float kProgressDistrict     = 0.74f;
	constexpr float kProgressPostProcess  = 0.79f;
	constexpr float kProgressDone         = 0.91f;
}

void GameScene::generateAllTerrain()
{
	setLoadingStatus(U"地形生成中");
	const Stopwatch sw{ StartImmediately::Yes };

	// 全チャンクの高さマップを並列生成し、完成後にワールドへ一括で反映する。
	const int total = WORLD_CHUNKS * WORLD_CHUNKS;
	const int nThreads = Max(1, static_cast<int>(std::thread::hardware_concurrency()));

	Array<HeightMapResult> buffer(total);

	std::atomic<int> counter{ 0 };
	Array<std::thread> threads;
	threads.reserve(nThreads);

	for (int t = 0; t < nThreads; ++t)
	{
		threads.emplace_back([this, &buffer, &counter, total]()
		{
			while (true)
			{
				const int idx = counter.fetch_add(1);
				if (idx >= total) break;
				const int cx = idx % WORLD_CHUNKS;
				const int cy = idx / WORLD_CHUNKS;
				buffer[idx] = m_world.buildHeightMap(Point{ cx, cy });
				m_genProgress.store(kProgressTerrain +
					(kProgressSettlement - kProgressTerrain) * static_cast<float>(idx + 1) / total);
			}
		});
	}

	for (auto& th : threads) th.join();

	for (int idx = 0; idx < total; ++idx)
	{
		const int cx = idx % WORLD_CHUNKS;
		const int cy = idx / WORLD_CHUNKS;
		m_world.installChunkDirect(Point{ cx, cy }, std::move(buffer[idx]));
	}

	m_genProgress.store(kProgressSettlement);
	Logger << U"[Phase1] 地形生成完了: {} チャンク ({:.1f}秒)"_fmt(total, sw.sF());
}

void GameScene::placeAllSettlements()
{
	setLoadingStatus(U"地区配置中");

	auto settlements = MapGenerator::placeAllSettlements(getData().seed, m_world);

	Array<BiomeType> biomes;
	biomes.reserve(settlements.size());
	for (const auto& s : settlements)
		biomes << m_world.getBiome(static_cast<float>(s.center.x), static_cast<float>(s.center.y));

	PlaceNameGenerator placeGen;
	placeGen.load(U"assets/placenames/placenames.toml");
	m_placeNames = placeGen.generateWithBiomes(
		static_cast<int>(settlements.size()), biomes, getData().seed);

	for (int i = 0; i < static_cast<int>(settlements.size()); ++i)
	{
		settlements[i].name    = m_placeNames.settlementName(i);
		settlements[i].reading = m_placeNames.settlementReading(i);
	}

	m_districts = settlements;
	m_castleTownCenters.clear();
	for (const auto& s : m_districts)
	{
		if (s.kind == MapGenerator::SettlementKind::RegionalCity)
			m_castleTownCenters << s.center;
	}

	m_genProgress.store(kProgressRoads);
}

void GameScene::generateAllRoads()
{
	setLoadingStatus(U"道路生成中");

	MapGenerator::generateGlobalRoads(
		getData().seed, m_districts, m_world, m_network,
		[this](float fraction) {
			m_genProgress.store(kProgressRoads +
				(kProgressDistrict - kProgressRoads) * fraction);
		});
}

void GameScene::generateDistrictRoads()
{
	setLoadingStatus(U"地区内道路生成中");
	m_genProgress.store(kProgressDistrict);

	MapGenerator::generateDistrictRoads(
		getData().seed, m_districts, m_world, m_network,
		[this](float fraction) {
			m_genProgress.store(kProgressDistrict +
				(kProgressPostProcess - kProgressDistrict) * fraction);
		});
	setLoadingStatus(U"村どうしの遠回りを改善中");
	VillageConnections::improve(getData().seed, m_districts, m_world, m_network);
}

void GameScene::postProcessRoads()
{
	setLoadingStatus(U"道路ポスト処理中");
	m_genProgress.store(kProgressPostProcess);

	// 道路生成後の形状補正、交差点整理、標識登録までを段階的に実行して走行可能なネットワークへ整える。
	const Stopwatch total{ StartImmediately::Yes };
	Stopwatch step{ StartImmediately::Yes };

	Logger << U"[PostProcess] 開始 (nodes={}, edges={})"_fmt(
		m_network.nodes().size(), m_network.edges().size());

	{
		int iter = 0;
		constexpr int kMaxIter = 1000;
		while (iter < kMaxIter && m_network.fixSharpAngles(45.0f))
			++iter;
		Logger << U"[PostProcess] fixSharpAngles: {} 回, {:.0f}ms"_fmt(iter, step.msF());
		if (getData().auditRoadIntegrity) RoadBinary::writeGlobal(U"integrity_fixSharpAngles.bin",m_network);
		step.restart();
	}

	m_network.smoothAllCurves();
	Logger << U"[PostProcess] smoothAllCurves: {:.0f}ms"_fmt(step.msF());
	if (getData().auditRoadIntegrity) RoadBinary::writeGlobal(U"integrity_smoothAllCurves.bin",m_network);
	step.restart();

	DistrictRoads::straightenCastleTownRoads(m_districts, m_world, m_network);
	DBG_LOG(U"[RoadDesignChains] repaired={}"_fmt(RoadDesignLimits::smoothThroughChains(m_network)));
	for (const auto& edge : m_network.edges())
	{
		if (edge.id >= 0) { RoadDesignLimits::constrainCurve(m_network, edge.id); }
	}
	for (const auto& edge : m_network.edges())
	{
		if (edge.id >= 0) m_network.getEdge(edge.id)->edgeState = EdgeState::Open;
	}
	m_network.resolveIntersections();
	m_network.consolidateOverlappingRoads();
	m_network.resolveIntersections();
	m_network.consolidateOverlappingRoads();
	m_network.removeDuplicateEdges(getData().seed);
	Logger << U"[PostProcess] reconcileRoadGeometry: {:.0f}ms"_fmt(step.msF());
	if (getData().auditRoadIntegrity) RoadBinary::writeGlobal(U"integrity_reconciled.bin",m_network);
	if (getData().auditRoadIntegrity)
	{
		for (const auto& district : m_districts)
		{
			if (!district.plan.ready || district.plan.frontageRoads) { continue; }
			const Vec2 interior = district.plan.halfExtent-Vec2{80,80};
			int tested=0, misaligned=0;
			double largestDrift=0;
			for (const auto& edge : m_network.edges())
			{
				if (edge.id<0) { continue; }
				const Vec3 a=m_network.getNode(edge.nodeA)->position,b=m_network.getNode(edge.nodeB)->position;
				const Vec2 localA{a.x-district.center.x,a.z-district.center.y},localB{b.x-district.center.x,b.z-district.center.y};
				if (Abs(localA.dot(district.gridAxisX))>interior.x || Abs(localA.dot(district.gridAxisZ))>interior.y
					|| Abs(localB.dot(district.gridAxisX))>interior.x || Abs(localB.dot(district.gridAxisZ))>interior.y) { continue; }
				++tested;
				const Vec2 direction=localB-localA;
				const double drift=Min(Abs(direction.dot(district.gridAxisX)),Abs(direction.dot(district.gridAxisZ)));
				largestDrift=Max(largestDrift,drift);
				misaligned+=(drift>0.1);
			}
			DBG_LOG(U"[TownGridIntegrity] center=({}, {}) edges={} misaligned={} maxDriftM={}"_fmt(district.center.x,district.center.y,tested,misaligned,largestDrift));
		}
	}
	step.restart();

	const auto auditDryStreets = [&](StringView stage)
	{
		double highest = 0; int raised = 0;
		for (const auto& node : m_network.nodes())
		{
			if (node.id < 0) { continue; }
			const double ground = m_world.sampleHeight(static_cast<float>(node.position.x), static_cast<float>(node.position.z));
			if (ground < m_world.waterSurfaceHeight(node.position.x, node.position.z) + 3.2) { continue; }
			const double gap = node.position.y - ground; highest = Max(highest, gap); raised += gap > 8;
		}
		DBG_LOG(U"[RoadHeightStage] stage={} raisedDryNodes={} maximumGap={}"_fmt(stage, raised, highest));
	};
	auditDryStreets(U"terrain");
	WaterCrossings::splitWaterSpans(m_network,
		[&](double x, double z) { return m_world.sampleHeight(static_cast<float>(x), static_cast<float>(z)); },
		[&](double x, double z) { return m_world.waterSurfaceHeight(x, z); });
	RoadVerticalAlignment::apply(m_network,m_world);
	auditDryStreets(U"vertical");
	const auto roadsBefore = RoadDesignLimits::measure(m_network, m_world);
	// Resolve infeasible hills before bridge approach heights can spread into adjacent towns.
	RoadDesignLimits::apply(m_network, m_world);
	auditDryStreets(U"alignment");
	const auto water=WaterCrossings::repair(m_network,[&](double x,double z) { return m_world.sampleHeight(static_cast<float>(x),static_cast<float>(z)); },[&](double x,double z) { return m_world.waterSurfaceHeight(x,z); });
	auditDryStreets(U"crossings");
	RoadDesignLimits::fitGrades(m_network, m_world);
	auditDryStreets(U"finalGrades");
	RoadTerrainFit::apply(m_network,m_world);
	const auto beforeJunctions=RoadDesignLimits::measure(m_network,m_world);
	const bool joinedTunnels=m_network.resolveIntersections();
	const auto afterJunctions=RoadDesignLimits::measure(m_network,m_world);
	if (joinedTunnels && (afterJunctions.gradeViolations>0 || afterJunctions.radiusViolations>0))
	{
		// A shared junction changes curve endpoints; re-establish their grade planes afterwards.
		RoadDesignLimits::fitGrades(m_network,m_world);
		RoadTerrainFit::apply(m_network,m_world);
	}
	DBG_LOG(U"[TunnelTopology] intersectionsAfterVerticalAlignment={} gradeBeforeJoin={} gradeAfterJoin={}"_fmt(joinedTunnels,beforeJunctions.gradeViolations,afterJunctions.gradeViolations));
	const auto roadsAfter = RoadDesignLimits::measure(m_network, m_world);
	DBG_LOG(U"[RoadDesignAudit] edges={} gradeBefore={} gradeAfter={} radiusBefore={} radiusAfter={} maxGrade={:.5f}"_fmt(
		roadsAfter.edges, roadsBefore.gradeViolations, roadsAfter.gradeViolations,
		roadsBefore.radiusViolations, roadsAfter.radiusViolations, roadsAfter.maximumGrade));
	JSON groundReport; int townIndex=0;
	for (const auto& district : m_districts)
	{
		if (district.kind!=MapGenerator::SettlementKind::RegionalCity) { continue; }
		int streets=0,elevated=0,tunnels=0; double largestClearance=0;
		const double radius=district.plan.ready ? Min(district.plan.halfExtent.x,district.plan.halfExtent.y)*.65 : 400;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0) { continue; }
			const auto curve=m_network.getBezier(edge.id); const Vec3 point=curve->evaluate(.5f);
			if (Vec2{point.x,point.z}.distanceFrom(district.center)>radius) { continue; }
			++streets; elevated+=edge.useElevation && !edge.tunnel; tunnels+=edge.tunnel;
			largestClearance=Max(largestClearance,point.y-m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)));
		}
		JSON item; item[U"center"]=Array<double>{district.center.x,district.center.y}; item[U"streets"]=streets;
		item[U"elevated"]=elevated; item[U"tunnels"]=tunnels; item[U"largestClearanceM"]=largestClearance;
		groundReport[U"towns"][townIndex++]=item;
		DBG_LOG(U"[TownRoadGround] center=({}, {}) streets={} elevated={} tunnels={} maxClearanceM={:.2f}"_fmt(district.center.x,district.center.y,streets,elevated,tunnels,largestClearance));
	}
	groundReport[U"gradeViolations"]=roadsAfter.gradeViolations; groundReport[U"radiusViolations"]=roadsAfter.radiusViolations;
	groundReport.save(U"road_ground_audit.json");
	DBG_LOG(U"[NarrowBlockMerge] removedShortcuts={}"_fmt(StreetBlocks::mergeNarrowFaces(m_network)));
	for (const auto& edge : m_network.edges())
	{
		if (edge.id>=0 && edge.useElevation && !edge.tunnel) { m_network.generatePiersForEdge(edge.id,m_world); }
	}
	DBG_LOG(U"[WaterCrossings] wetBefore={} elevatedEdges={}"_fmt(water.wetEdges,water.elevatedEdges));
	{
		double wetLength=0,parallelLength=0,maxBridgeGrade=0;int bridges=0,submerged=0;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0 || edge.tunnel) { continue; }
			const auto curve=m_network.getBezier(edge.id);bool wet=false;
			for (float arc=0;arc<curve->totalLength;arc+=6)
			{
				const Vec3 p=curve->positionAt(arc),tangent=curve->tangentAt(arc);
				const double ground=m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)),waterLevel=m_world.waterSurfaceHeight(p.x,p.z);
				if (ground>=waterLevel+1) { continue; }
				wet=true;wetLength+=6;submerged+=(edge.usesDesignHeight() ? p.y : ground)<waterLevel;
				maxBridgeGrade=Max(maxBridgeGrade,Abs(tangent.y)/Max(.001,Vec2{tangent.x,tangent.z}.length()));
				const auto river=m_world.rivers().nearest({p.x,p.z});
				if (river.reach>=0 && river.distance<river.halfWidth)
				{
					const auto& reach=m_world.rivers().reaches[river.reach];const Vec2 flow{reach.end.x-reach.start.x,reach.end.z-reach.start.z};
					if (Abs(flow.normalized().dot(Vec2{tangent.x,tangent.z}.normalized()))>.8) { parallelLength+=6; }
				}
			}
			bridges+=wet;
		}
		DBG_LOG(U"[BridgeAlignmentAudit] wetEdges={} waterLengthM={} parallelWaterLengthM={} maxWaterGrade={} submergedSamples={}"_fmt(bridges,wetLength,parallelLength,maxBridgeGrade,submerged));
	}
	if (getData().auditRoadIntegrity)
	{
		int wetRoads=0;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0) { continue; } const auto curve=m_network.getBezier(edge.id);bool wet=false;
			for (float arc=0;arc<=curve->totalLength;arc+=4)
			{
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,0,1})
				{
					const Vec3 p=point+right*(edge.totalWidth()*.5*side);const double ground=m_world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)),surface=m_world.waterSurfaceHeight(p.x,p.z);
					const double pavement=edge.usesDesignHeight() ? point.y : ground;
					wet|=ground<surface && pavement<surface-.03 && !(edge.tunnel && pavement<ground-6);
				}
			}
			wetRoads+=wet;
		}
		DBG_LOG(U"[RoadWaterAudit] submergedRoads={}"_fmt(wetRoads));
	}

	{
		int elevated=0,low=0,buried=0;
		for (const auto& edge : m_network.edges())
		{
			if (edge.id<0 || !edge.useElevation) { continue; }
			++elevated; double maximum=-1e9,minimum=1e9;
			const auto curve=m_network.getBezier(edge.id);
			for (int i=0;i<=20;++i) { const auto point=curve->evaluate(i/20.0f); const double gap=point.y-m_world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)); maximum=Max(maximum,gap); minimum=Min(minimum,gap); }
			low+=maximum<2; buried+=minimum<-.05;
		}
		DBG_LOG(U"[RoadGroundAudit] elevated={} belowTwoMeters={} buried={}"_fmt(elevated,low,buried));
	}
	registerGuideDestinations();
	Logger << U"[PostProcess] registerGuideDestinations: {:.0f}ms"_fmt(step.msF());
	step.restart();

	// 初期生成された道路はすべて建設済み（供用中）とする
	for (const auto& edge : m_network.edges())
	{
		if (edge.id < 0) continue;
		if (RoadEdge* e = m_network.getEdge(edge.id)) e->edgeState = EdgeState::Open;
	}

	m_genProgress.store(kProgressDone);
	Logger << U"[Phase4] PostProcess 完了 ({:.0f}ms)"_fmt(total.msF());
}

void GameScene::registerGuideDestinations()
{
	// 地区データを道路ノード上の目的地辞書へ写し替え、自動案内標識の再生成まで一気に行う。
	m_network.clearNamedDestinations();

	const auto tierOf = [](MapGenerator::SettlementKind t) -> uint8 {
		switch (t)
		{
		case MapGenerator::SettlementKind::RegionalCity:   return 0;
		case MapGenerator::SettlementKind::LocalTown: return 1;
		case MapGenerator::SettlementKind::RuralSettlement:   return 2;
		}
		return 2;
	};

	int registered = 0;
	for (const auto& s : m_districts)
	{
		if (s.name.isEmpty()) continue;

		// 地区中心から最寄り RoadNode を線形検索
		const Vec3 target{ s.center.x, 0.0, s.center.y };
		int   bestNode = -1;
		float bestDistSq = std::numeric_limits<float>::max();
		for (const auto& n : m_network.nodes())
		{
			if (n.id < 0 || n.attachments.isEmpty()) { continue; }
			const float dx = static_cast<float>(n.position.x - target.x);
			const float dz = static_cast<float>(n.position.z - target.z);
			const float d2 = dx * dx + dz * dz;
			if (d2 < bestDistSq)
			{
				bestDistSq = d2;
				bestNode = n.id;
			}
		}
		if (bestNode < 0) continue;

		m_network.addNamedDestination(bestNode, s.name, s.reading, tierOf(s.kind));
		++registered;
	}

	m_network.recomputeAllAutoGuideSigns();
	Logger << U"[GuideSign] {} 地区を登録, 案内標識自動生成完了"_fmt(registered);
}

void GameScene::addDistricts(const Array<MapGenerator::Settlement>& newDistricts)
{
	for (auto s : newDistricts)
	{
		if (s.name.isEmpty())
		{
			const int idx = static_cast<int>(m_districts.size());
			s.name    = m_placeNames.settlementName(idx);
			s.reading = m_placeNames.settlementReading(idx);
		}
		m_districts << s;
		if (s.kind == MapGenerator::SettlementKind::RegionalCity)
			m_castleTownCenters << s.center;
	}
}

SettlementDevelopment GameScene::settlementDevelopment()
{
	return {m_world, m_network, m_trainNetwork, m_districts, getData().seed, getData().generation};
}

void GameScene::applyZonesGlobal()
{
	settlementDevelopment().applyZonesGlobal();
}

void GameScene::placeInitialBuildings(bool preserveLandPatches)
{
	const auto result = settlementDevelopment().placeInitialBuildings(preserveLandPatches);
	m_cityConstraintValidationPassed = result.passed;
	m_cityConstraintValidationSummary = result.summary;
}

void GameScene::generateLandPatches(bool preserveExisting)
{
	settlementDevelopment().generateLandPatches(preserveExisting);
}

void GameScene::migrateLegacyBuildingFrontageReferences()
{
	settlementDevelopment().migrateLegacyBuildingFrontageReferences();
}

bool GameScene::validateGeneratedCityConstraints()
{
	const auto result = settlementDevelopment().validateGeneratedCityConstraints();
	m_cityConstraintValidationSummary = result.summary;
	return result.passed;
}

void GameScene::refreshBuildingAnglesFromEdges()
{
	settlementDevelopment().refreshBuildingAnglesFromEdges();
}
