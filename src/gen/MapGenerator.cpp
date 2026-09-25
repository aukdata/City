#include "GenerationSettings.hpp"
#include "MapGenerator.hpp"
#include "RoadNodeIndex.hpp"
#include "SettlementPlacement.hpp"
#include "RoadAlignment.hpp"
#include "RoadConstructionCost.hpp"
#include "RoadDesignLimits.hpp"
#include "RailwayAlignment.hpp"
#include "StreetProfile.hpp"
#include "../debug/DebugLog.hpp"
#include "PlaceNameGenerator.hpp"
#include "DistrictRoads.hpp"
#include "LeveeRoad.hpp"
#include <random>
#include <queue>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// initWorld()
// ─────────────────────────────────────────────────────────────────────────────

MapGenerator::InitResult MapGenerator::initWorld(
	uint64 seed, World& world)
{
	// ワールド生成パラメータと地名供給源だけを先に初期化し、後段フェーズの前提をそろえる。
	world.setGenerationParams(seed,
		WORLD_SIZE,
		WORLD_SIZE);

	PlaceNameGenerator placeGen;
	placeGen.load(U"assets/placenames/placenames.toml");

	InitResult result;
	result.placeNames = placeGen.generate(4096, seed);
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 空間ハッシュ（道路ノード最近傍検索用）
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/// @brief 集落の接続先は道路に接続済みのノードから、進入方向も含めた費用で選ぶ。
	Optional<int> findRoadAccess(const RoadNodeIndex& index, Vec3 position, const RoadNetwork& network,
		float radius, int excludeId = -1, const Optional<Vec2>& preferredDirection = none)
	{
		const Vec2 origin{static_cast<float>(position.x), static_cast<float>(position.z)};
		return index.findBest(position, network, radius, [&](const RoadNode& node, float) -> Optional<double>
		{
			if (node.id == excludeId || node.attachments.isEmpty()) { return none; }
			return SettlementPlacement::roadAccessCost(origin, {node.position.x, node.position.z}, preferredDirection);
		});
	}

	/// @brief 指定ノードの前後ノードから接線方向を返す
	Vec2 roadNodeTangent(int nodeId, const RoadNetwork& net)
	{
		const RoadNode* node = net.getNode(nodeId);
		if (!node || node->attachments.isEmpty()) return Vec2{ 1, 0 };

		// 最初の接続エッジの方向を返す
		const RoadEdge* edge = net.getEdge(node->attachments[0].edgeId);
		if (!edge) return Vec2{ 1, 0 };

		const RoadNode* other = net.getNode(
			(edge->nodeA == nodeId) ? edge->nodeB : edge->nodeA);
		if (!other) return Vec2{ 1, 0 };

		Vec2 dir{
			static_cast<float>(other->position.x - node->position.x),
			static_cast<float>(other->position.z - node->position.z)
		};
		const float len = static_cast<float>(dir.length());
		return (len > 0.01f) ? Vec2{ dir.x / len, dir.y / len } : Vec2{ 1, 0 };
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// MST サブセット（Prim's O(n^2)）
// ─────────────────────────────────────────────────────────────────────────────

Array<std::pair<int,int>> MapGenerator::computeMSTSubset(
	const Array<Settlement>& settlements, const Array<int>& indices)
{
	// 地区集合の骨格接続だけを得るため、完全グラフを明示せず Prim 法で最小木を組む。
	const int n = static_cast<int>(indices.size());
	if (n <= 1) return {};

	Array<float> minCost(n, 1e30f);
	Array<int>   minEdge(n, -1);
	Array<bool>  inTree(n, false);
	Array<std::pair<int,int>> mst;

	minCost[0] = 0.0f;

	for (int iter = 0; iter < n; ++iter)
	{
		int u = -1;
		float best = 1e30f;
		for (int i = 0; i < n; ++i)
		{
			if (!inTree[i] && minCost[i] < best)
			{
				best = minCost[i];
				u = i;
			}
		}
		if (u < 0) break;

		inTree[u] = true;
		if (minEdge[u] >= 0)
			mst << std::make_pair(minEdge[u], u);

		for (int v = 0; v < n; ++v)
		{
			if (inTree[v]) continue;
			const float d = static_cast<float>(
				settlements[indices[u]].center.distanceFrom(
					settlements[indices[v]].center));
			if (d < minCost[v])
			{
				minCost[v] = d;
				minEdge[v] = u;
			}
		}
	}

	return mst;
}

// ─────────────────────────────────────────────────────────────────────────────
// MST diameter（最長パス）
// ─────────────────────────────────────────────────────────────────────────────

Array<int> MapGenerator::findMSTDiameter(
	const Array<std::pair<int,int>>& mst, int nodeCount)
{
	// 最小木の主幹線候補を得るため、2 回 BFS で最長パスを抽出する。
	if (nodeCount <= 1) return { 0 };

	// 隣接リスト
	Array<Array<int>> adj(nodeCount);
	for (const auto& [u, v] : mst)
	{
		adj[u] << v;
		adj[v] << u;
	}

	// BFS で最遠点を見つける
	auto bfs = [&](int start) -> std::pair<int, Array<int>>
	{
		Array<int> dist(nodeCount, -1);
		Array<int> parent(nodeCount, -1);
		std::queue<int> q;
		q.push(start);
		dist[start] = 0;
		int farthest = start;

		while (!q.empty())
		{
			const int u = q.front(); q.pop();
			for (const int v : adj[u])
			{
				if (dist[v] >= 0) continue;
				dist[v] = dist[u] + 1;
				parent[v] = u;
				q.push(v);
				if (dist[v] > dist[farthest])
					farthest = v;
			}
		}

		// パスを復元
		Array<int> path;
		for (int cur = farthest; cur >= 0; cur = parent[cur])
			path << cur;
		path.reverse();
		return { farthest, path };
	};

	// 2回BFS: 任意→最遠u、u→最遠v
	const auto [u, _] = bfs(0);
	const auto [v, path] = bfs(u);
	return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// A* 道路セグメント生成ヘルパー
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// グローバル占有セルのキー（ワールド座標を粗いグリッドに量子化）
	const float kOccupyCellSize = GenerationSettings::get().network_occupiedCell;
	int64 occupyKey(float wx, float wz)
	{
		const int gx = static_cast<int>(Math::Floor(wx / kOccupyCellSize));
		const int gz = static_cast<int>(Math::Floor(wz / kOccupyCellSize));
		return (static_cast<int64>(gx) << 32) | static_cast<uint32>(gz);
	}
}

void MapGenerator::buildRoadSegment(
	const World& world, RoadNetwork& network,
	int startNodeId, Vec3 startPos, int endNodeId, Vec3 endPos,
	RoadType roadType, int lanes,
	HashSet<int64>& globalOccupied,
	const Array<Vec2>& forbiddenStartDirs,
	const Array<Vec2>& forbiddenGoalDirs,
	Array<int>* outEdgeIds)
{
	const double distance=Vec2{endPos.x-startPos.x,endPos.z-startPos.z}.length();
	const float kMargin = static_cast<float>(Max(static_cast<double>(GenerationSettings::get().network_routingMargin),
		distance*(distance>GenerationSettings::get().network_mediumRouteThreshold ? GenerationSettings::get().routing_longDistanceMarginRatio : GenerationSettings::get().routing_distanceMarginRatio)));
	const float sx = static_cast<float>(startPos.x);
	const float sz = static_cast<float>(startPos.z);
	const float ex = static_cast<float>(endPos.x);
	const float ez = static_cast<float>(endPos.z);

	const float minX = Min(sx, ex) - kMargin;
	const float minZ = Min(sz, ez) - kMargin;
	const float maxX = Max(sx, ex) + kMargin;
	const float maxZ = Max(sz, ez) + kMargin;

	const float dist = std::sqrt((ex - sx) * (ex - sx) + (ez - sz) * (ez - sz));
	const float cellSize = (dist > GenerationSettings::get().network_longRouteThreshold) ? GenerationSettings::get().network_longRoutingCell
	                     : (dist > GenerationSettings::get().network_mediumRouteThreshold) ? GenerationSettings::get().network_mediumRoutingCell
	                     : RoadPathfinder::defaultCellSize();

	const int pfW = Max(2, static_cast<int>(Ceil((maxX - minX) / cellSize)));
	const int pfH = Max(2, static_cast<int>(Ceil((maxZ - minZ) / cellSize)));

	RoadPathfinder pf;
	pf.setup(world, Vec2{ minX, minZ }, pfW, pfH, cellSize);

	const Point gs = pf.worldToGrid(sx, sz);
	const Point ge = pf.worldToGrid(ex, ez);

	// グローバル占有セルをローカルグリッドの occupiedCells に変換
	HashSet<int> localOccupied;
	for (const int64 key : globalOccupied)
	{
		const int gx = static_cast<int>(key >> 32);
		const int gz = static_cast<int>(static_cast<uint32>(key));
		const float wx = (gx + 0.5f) * kOccupyCellSize;
		const float wz = (gz + 0.5f) * kOccupyCellSize;
		const Point lp = pf.worldToGrid(wx, wz);
		if (lp.x >= 0 && lp.x < pfW && lp.y >= 0 && lp.y < pfH)
			localOccupied.insert(lp.y * pfW + lp.x);
	}

	const int sampleStep = (cellSize > GenerationSettings::get().network_denseSampleThreshold) ? 2 : 3;

	pf.setRoadType(roadType);
	const Array<Point> path = pf.findPath(gs, ge, forbiddenStartDirs, forbiddenGoalDirs, localOccupied);

	if (path.isEmpty() || path.size()<2)
	{
		DBG_LOG(U"[RoadGeneration] no feasible coarse path from={} to={}"_fmt(startNodeId,endNodeId));
		return;
	}
	Array<Vec3> wps=pf.samplePath(path,sampleStep);
	wps.front()=startPos; wps.back()=endPos;
	Optional<RoadAlignment::Result> selected=RoadAlignment::fitTerrain(world,wps,roadType);
	const double maximumHeight=GenerationSettings::get().roads_maximumGeneratedViaductHeight;
	if (selected && RoadAlignment::maximumClearance(world,selected->curves)>maximumHeight) { selected.reset(); }
	// 距離による検証の省略をしない。成立しない粗い回廊は地形・方位・高さを再探索する。
	if (!selected || selected->cost>distance*RoadConstructionCost::Earthwork())
	{
		if (const auto route=RoadAlignment::find(world,startPos,endPos,roadType,GenerationSettings::get().routing_generatedExpansionLimit,TransportMode::Road,maximumHeight); route && (!selected || route->cost<selected->cost)) { selected=route; }
	}
	if (!selected)
	{
		DBG_LOG(U"[RoadGeneration] no legal alignment from={} to={} distance={}"_fmt(startNodeId,endNodeId,distance));
		return;
	}
	int previous=startNodeId;
	for (size_t i=0; i<selected->curves.size(); ++i)
	{
		const auto& curve=selected->curves[i];
		const int next=i+1==selected->curves.size() ? endNodeId : network.addNode(curve.p3);
		if (const auto id=network.addEdge(previous,next,curve.p1,curve.p2,roadType,lanes))
		{
			network.getEdge(*id)->designGrade=true;
			network.updateEdgeElevation(*id,world);
			if (outEdgeIds) { *outEdgeIds << *id; }
		}
		for (float along=0; along<=curve.totalLength; along+=kOccupyCellSize*.5f)
		{
			const auto point=curve.positionAt(along);
			globalOccupied.insert(occupyKey(static_cast<float>(point.x),static_cast<float>(point.z)));
		}
		previous=next;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// generateGlobalRoads: 3層分岐構造で道路を一括生成する
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::generateGlobalRoads(
	[[maybe_unused]] uint64 seed,
	const Array<Settlement>& settlements,
	const World& world,
	RoadNetwork& network,
	ProgressCallback onProgress)
{
	if (settlements.isEmpty()) return;

	const Stopwatch swTotal{ StartImmediately::Yes };
	Stopwatch swStep{ StartImmediately::Yes };

	// 全地区のノードを作成
	Array<int> nodeIds;
	nodeIds.reserve(settlements.size());
	for (const auto& s : settlements)
	{
		const float y = world.sampleHeight(
			static_cast<float>(s.center.x), static_cast<float>(s.center.y));
		nodeIds << network.addNode(Vec3{ s.center.x, y, s.center.y }, NodeType::Intersection);
	}

	// 種別ごとにインデックスを分類
	Array<int> castleTownIdx, postTownIdx, villageIdx;
	for (int i = 0; i < static_cast<int>(settlements.size()); ++i)
	{
		switch (settlements[i].kind)
		{
		case SettlementKind::RegionalCity:   castleTownIdx << i; break;
		case SettlementKind::LocalTown: postTownIdx << i; break;
		case SettlementKind::RuralSettlement:   villageIdx << i; break;
		}
	}

	Logger << U"[Roads] ノード作成: {} (城下町={}, 宿場町={}, 農村={})"_fmt(
		nodeIds.size(), castleTownIdx.size(), postTownIdx.size(), villageIdx.size());

	RoadNodeIndex roadHash;
	HashSet<int64> globalOccupied;  // 既存道路が通過するワールドセルの集合

	// =====================================================================
	// Layer 1: 幹線街道（城下町間チェーン）
	// plan/22_road_route_spec.md §1 参照: 生成した幹線街道を RoadRoute::NationalRoute として登録
	// =====================================================================
	swStep.restart();

	// 国道指定用の edge ID 収集
	Array<int> chainEdges;   // diameter チェーンの連続 edge 群
	Array<int> frontExtEdges; // diameter.front() 側の map 延伸 (城下町 → map edge 順で格納)
	Array<int> backExtEdges;  // diameter.back() 側の map 延伸
	Array<Array<int>> branchRoutes;  // MST 枝線 (diameter に含まれないもの)

	if (castleTownIdx.size() >= 2)
	{
		// 城下町のみで MST を計算
		auto castleTownMST = computeMSTSubset(settlements, castleTownIdx);

		// MST 上の diameter（最長パス）を求める
		Array<int> diameter = findMSTDiameter(castleTownMST, static_cast<int>(castleTownIdx.size()));

		// diameter 上のノードをセットに登録（枝線判定用）
		HashSet<int> diameterSet;
		for (const int di : diameter)
			diameterSet.insert(di);

		// diameter チェーンを A* で接続
		for (int i = 0; i + 1 < static_cast<int>(diameter.size()); ++i)
		{
			const int si = castleTownIdx[diameter[i]];
			const int ei = castleTownIdx[diameter[i + 1]];
			const RoadNode* sn = network.getNode(nodeIds[si]);
			const RoadNode* en = network.getNode(nodeIds[ei]);
			if (!sn || !en) continue;

			buildRoadSegment(world, network,
				nodeIds[si], sn->position, nodeIds[ei], en->position,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &chainEdges);
		}

		// MST 枝線（diameter に含まれない城下町 → diameter 上の親へ接続）
		for (const auto& [parentLocal, childLocal] : castleTownMST)
		{
			if (diameterSet.contains(parentLocal) && diameterSet.contains(childLocal))
				continue;  // diameter 上の辺は既に処理済み

			const int si = castleTownIdx[parentLocal];
			const int ei = castleTownIdx[childLocal];
			const RoadNode* sn = network.getNode(nodeIds[si]);
			const RoadNode* en = network.getNode(nodeIds[ei]);
			if (!sn || !en) continue;

			Array<int> branchEdges;
			buildRoadSegment(world, network,
				nodeIds[si], sn->position, nodeIds[ei], en->position,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &branchEdges);
			if (!branchEdges.isEmpty())
				branchRoutes << std::move(branchEdges);
		}
		// diameter 両端からマップ外端への幹線延伸
		const float worldSize = WORLD_SIZE;
		const int endIndices[2] = { diameter.front(), diameter.back() };
		for (int ei2 = 0; ei2 < 2; ++ei2)
		{
			const int di = endIndices[ei2];
			const int si = castleTownIdx[di];
			const RoadNode* sn = network.getNode(nodeIds[si]);
			if (!sn) continue;

			const float sx = static_cast<float>(sn->position.x);
			const float sz = static_cast<float>(sn->position.z);

			// 4辺への距離を計算し、最も近い外端の座標を選ぶ
			const float dLeft   = sx;
			const float dRight  = worldSize - sx;
			const float dTop    = sz;
			const float dBottom = worldSize - sz;
			const float minEdgeDist = Min({ dLeft, dRight, dTop, dBottom });

			float edgeX, edgeZ;
			if (minEdgeDist == dLeft)        { edgeX = 0.0f;      edgeZ = sz; }
			else if (minEdgeDist == dRight)  { edgeX = worldSize;  edgeZ = sz; }
			else if (minEdgeDist == dTop)    { edgeX = sx;         edgeZ = 0.0f; }
			else                             { edgeX = sx;         edgeZ = worldSize; }

			// マップ端に少し余裕を持たせる（10m内側）
			edgeX = Clamp(edgeX, GenerationSettings::get().network_borderInset, worldSize - GenerationSettings::get().network_borderInset);
			edgeZ = Clamp(edgeZ, GenerationSettings::get().network_borderInset, worldSize - GenerationSettings::get().network_borderInset);

			const float ey = world.sampleHeight(edgeX, edgeZ);
			const Vec3 edgePos{ edgeX, ey, edgeZ };
			const int edgeNodeId = network.addNode(edgePos, NodeType::Endpoint);

			Array<int>& extOut = (ei2 == 0) ? frontExtEdges : backExtEdges;
			buildRoadSegment(world, network,
				nodeIds[si], sn->position, edgeNodeId, edgePos,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &extOut);
		}
	}
	else if (castleTownIdx.size() == 1)
	{
		// 城下町が1つだけ → マップ外端への接続のみ
		const float worldSize = WORLD_SIZE;
		const int si = castleTownIdx[0];
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (sn)
		{
			const float sx = static_cast<float>(sn->position.x);
			const float sz = static_cast<float>(sn->position.z);
			const float dLeft = sx, dRight = worldSize - sx;
			const float dTop = sz, dBottom = worldSize - sz;
			const float minD = Min({ dLeft, dRight, dTop, dBottom });
			float ex, ez;
			if (minD == dLeft)       { ex = GenerationSettings::get().network_borderInset;           ez = sz; }
			else if (minD == dRight) { ex = worldSize - GenerationSettings::get().network_borderInset; ez = sz; }
			else if (minD == dTop)   { ex = sx;               ez = GenerationSettings::get().network_borderInset; }
			else                     { ex = sx;               ez = worldSize - GenerationSettings::get().network_borderInset; }
			const float ey = world.sampleHeight(ex, ez);
			const Vec3 ep{ ex, ey, ez };
			const int enid = network.addNode(ep, NodeType::Endpoint);
			buildRoadSegment(world, network, nodeIds[si], sn->position, enid, ep,
				RoadType::Arterial, 4, globalOccupied,
				{}, {}, &chainEdges);
		}
	}

	// Layer 1 幹線を国道として登録
	// 主要国道: frontExt (逆順) + chain + backExt を 1 本の route として
	{
		Array<int> mainRoute;
		mainRoute.reserve(frontExtEdges.size() + chainEdges.size() + backExtEdges.size());
		// frontExt は 城下町→map_edge 順で格納されているので、route 先頭に持ってくるには逆順
		for (int i = static_cast<int>(frontExtEdges.size()) - 1; i >= 0; --i)
			mainRoute << frontExtEdges[i];
		for (const int eid : chainEdges)    mainRoute << eid;
		for (const int eid : backExtEdges)  mainRoute << eid;

		if (!mainRoute.isEmpty())
		{
			network.addRoute(RoadRouteKind::NationalRoute, U"", std::move(mainRoute));
		}
	}
	// 支線国道（MST 枝線）
	for (auto& br : branchRoutes)
	{
		if (!br.isEmpty())
			network.addRoute(RoadRouteKind::NationalRoute, U"", std::move(br));
	}
	Logger << U"[Roads] Layer1 国道指定: main={} branches={}"_fmt(
		(chainEdges.size() + frontExtEdges.size() + backExtEdges.size() > 0 ? 1 : 0),
		branchRoutes.size());

	// Layer 1 の全中間ノードを空間ハッシュに登録
	for (const auto& node : network.nodes())
	{
		if (node.id >= 0)
			roadHash.insert(node.position, node.id);
	}

	Logger << U"[Roads] Layer1 幹線街道: {:.0f}ms (nodes={}, edges={})"_fmt(
		swStep.msF(), network.nodes().size(), network.edges().size());
	if (onProgress) onProgress(0.15f);

	// =====================================================================
	// Layer 2: 地方道（宿場町 → 最寄り幹線ノードへの分岐）
	// =====================================================================
	swStep.restart();
	int postTownsConnected = 0;
	Array<int> deferredPostTowns;

	for (const int si : postTownIdx)
	{
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (!sn) continue;

		const int nearId = findRoadAccess(roadHash, sn->position, network, GenerationSettings::get().network_cityJoinRadius, nodeIds[si]).value_or(-1);
		if (nearId < 0)
		{
			deferredPostTowns << si;
			continue;
		}

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) { deferredPostTowns << si; continue; }

		// Y字分岐: 幹線の接線方向を forbiddenGoalDirs に設定
		const Vec2 tangent = roadNodeTangent(nearId, network);
		buildRoadSegment(world, network,
			nearId, nearNode->position, nodeIds[si], sn->position,
			RoadType::Arterial, 2, globalOccupied,
			{}, { tangent });

		++postTownsConnected;
	}

	// deferred: 幹線から5km以上離れた宿場町 → 最寄りの接続済み宿場町へ
	// まず空間ハッシュを最新ノードで一括更新
	{
		const auto& nodes = network.nodes();
		for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
			if (nodes[i].id >= 0)
				roadHash.insert(nodes[i].position, nodes[i].id);
	}

	for (const int si : deferredPostTowns)
	{
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (!sn) continue;

		const int nearId = findRoadAccess(roadHash, sn->position, network, GenerationSettings::get().network_townJoinRadius, nodeIds[si]).value_or(-1);
		if (nearId < 0) continue;

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) continue;

		const int prevNodeCount = static_cast<int>(network.nodes().size());
		buildRoadSegment(world, network,
			nearId, nearNode->position, nodeIds[si], sn->position,
			RoadType::LocalRoad, 1, globalOccupied);

		// 新規ノードだけ空間ハッシュに追加
		const auto& nodes = network.nodes();
		for (int i = prevNodeCount; i < static_cast<int>(nodes.size()); ++i)
			if (nodes[i].id >= 0)
				roadHash.insert(nodes[i].position, nodes[i].id);

		++postTownsConnected;
	}

	Logger << U"[Roads] Layer2 地方道: {:.0f}ms (宿場町接続={}/{})"_fmt(
		swStep.msF(), postTownsConnected, postTownIdx.size());
	if (onProgress) onProgress(0.40f);

	// =====================================================================
	// Layer 3: 集落道（農村 → 最寄り道路ノードへの直線接続）
	// =====================================================================
	swStep.restart();

	// Layer 2 完了時点の全ノードで空間ハッシュを再構築
	roadHash.clear();
	for (const auto& node : network.nodes())
		if (node.id >= 0)
			roadHash.insert(node.position, node.id);

	villageIdx.sort_by([&](int a, int b)
	{
		return settlements[a].accessCost != settlements[b].accessCost
			? settlements[a].accessCost < settlements[b].accessCost : a < b;
	});
	int villagesConnected = 0, villagesRoadFacing = 0;
	const int totalVillages = static_cast<int>(villageIdx.size());

	for (int ri = 0; ri < totalVillages; ++ri)
	{
		const int si = villageIdx[ri];
		const RoadNode* sn = network.getNode(nodeIds[si]);
		if (!sn) continue;

		const int nearId = findRoadAccess(roadHash, sn->position, network, GenerationSettings::get().network_villageJoinRadius, nodeIds[si], settlements[si].accessDirection).value_or(-1);
		if (nearId < 0) continue;

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) continue;

		const float dist = static_cast<float>(sn->position.distanceFrom(nearNode->position));

		if (dist < GenerationSettings::get().network_villageDirectJoinDistance)
		{
			++villagesRoadFacing;
		}
		else
		{
			const Vec2 tangent = roadNodeTangent(nearId, network);
			const int prevNodeCount = static_cast<int>(network.nodes().size());

			buildRoadSegment(world, network,
				nearId, nearNode->position, nodeIds[si], sn->position,
				RoadType::LocalRoad, 1, globalOccupied,
				{}, { tangent });

			// 新規ノードを空間ハッシュに追加（後続の農村が利用）
			const auto& nodes = network.nodes();
			for (int i = prevNodeCount; i < static_cast<int>(nodes.size()); ++i)
				if (nodes[i].id >= 0)
					roadHash.insert(nodes[i].position, nodes[i].id);

			const RoadNode* villageNode = network.getNode(nodeIds[si]);
			if (villageNode && !villageNode->attachments.isEmpty()) { ++villagesConnected; }
		}

		if (onProgress && (ri % 500 == 0))
			onProgress(0.40f + 0.60f * static_cast<float>(ri) / totalVillages);
	}

	Logger << U"[Roads] Layer3 集落道: {:.0f}ms (接続={}, 街道沿い={}, 計={})"_fmt(
		swStep.msF(), villagesConnected, villagesRoadFacing, totalVillages);
	if (onProgress) onProgress(0.90f);

	// =====================================================================
	// 最終パス: 未接続地区の強制接続
	// =====================================================================
	swStep.restart();

	// BFS で接続済みノードを判定
	HashSet<int> reachable;
	{
		// 最初の城下町ノードから BFS
		int startNode = -1;
		for (const int ui : castleTownIdx)
		{
			const RoadNode* n = network.getNode(nodeIds[ui]);
			if (n && !n->attachments.isEmpty()) { startNode = nodeIds[ui]; break; }
		}
		if (startNode < 0)
		{
			// 城下町がなければエッジを持つ任意のノードから
			for (const auto& n : network.nodes())
				if (n.id >= 0 && !n.attachments.isEmpty()) { startNode = n.id; break; }
		}

		if (startNode >= 0)
		{
			std::queue<int> q;
			q.push(startNode);
			reachable.insert(startNode);
			while (!q.empty())
			{
				const int cur = q.front(); q.pop();
				const RoadNode* node = network.getNode(cur);
				if (!node) continue;
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					const RoadEdge* edge = network.getEdge(eid);
					if (!edge) continue;
					const int next = (edge->nodeA == cur) ? edge->nodeB : edge->nodeA;
					if (!reachable.contains(next))
					{
						reachable.insert(next);
						q.push(next);
					}
				}
			}
		}
	}

	// 空間ハッシュを再構築（接続済みノードのみ）
	roadHash.clear();
	for (const auto& node : network.nodes())
		if (node.id >= 0 && reachable.contains(node.id))
			roadHash.insert(node.position, node.id);

	int forceConnected = 0;
	const int nSettlements = static_cast<int>(settlements.size());
	for (int i = 0; i < nSettlements; ++i)
	{
		if (reachable.contains(nodeIds[i])) continue;

		const RoadNode* sn = network.getNode(nodeIds[i]);
		if (!sn) continue;

		const int nearId = findRoadAccess(roadHash, sn->position, network, GenerationSettings::get().network_fallbackJoinRadius, nodeIds[i]).value_or(-1);
		if (nearId < 0) continue;

		const RoadNode* nearNode = network.getNode(nearId);
		if (!nearNode) continue;

		buildRoadSegment(world, network,
			nearId, nearNode->position, nodeIds[i], sn->position,
			RoadType::LocalRoad, 1, globalOccupied);

		// 探索失敗を接続済みと扱わない。上限に達した候補は後続の接続先にも使わない。
		if (network.getNode(nodeIds[i])->attachments.isEmpty()) { continue; }
		// BFS で新たに到達可能になったノードを reachable に追加
		{
			std::queue<int> q;
			q.push(nodeIds[i]);
			reachable.insert(nodeIds[i]);
			while (!q.empty())
			{
				const int cur = q.front(); q.pop();
				const RoadNode* node = network.getNode(cur);
				if (!node) continue;
				for (const auto& att : node->attachments)
				{
					const int eid = att.edgeId;
					const RoadEdge* edge = network.getEdge(eid);
					if (!edge) continue;
					const int next = (edge->nodeA == cur) ? edge->nodeB : edge->nodeA;
					if (!reachable.contains(next))
					{
						reachable.insert(next);
						q.push(next);
						roadHash.insert(network.getNode(next)->position, next);
					}
				}
			}
		}
		++forceConnected;
	}

	Logger << U"[Roads] 未接続強制接続: {:.0f}ms ({}地区)"_fmt(swStep.msF(), forceConnected);
	if (onProgress) onProgress(1.0f);

	Logger << U"[Roads] 合計: nodes={}, edges={} ({:.0f}ms)"_fmt(
		network.nodes().size(), network.edges().size(), swTotal.msF());
}

// ─────────────────────────────────────────────────────────────────────────────
// 地区内道路生成
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::generateDistrictRoads(
	uint64 seed,
	Array<Settlement>& settlements,
	const World& world,
	RoadNetwork& network,
	ProgressCallback onProgress)
{
	const Stopwatch swTotal{ StartImmediately::Yes };
	// Regional roads outside the eventual town footprint are two-way roads with shoulders.
	// Expressways retain their own grade-separated cross section.
	for (const auto& source : network.edges())
	{
		if (source.id<0) { continue; }
		if (source.roadType!=RoadType::Arterial && source.roadType!=RoadType::LocalRoad) { continue; }
		if (auto* edge=network.getEdge(source.id))
		{
			auto role=source.roadType==RoadType::Arterial ? GeneratedStreet::Role::Regional : GeneratedStreet::Role::Village;
			if (role==GeneratedStreet::Role::Regional)
			{
				const auto curve=network.getBezier(source.id);
				int slopes=0;
				if (curve) for (const float fraction : {.2f,.5f,.8f})
				{
					const Vec3 center=curve->positionAt(curve->totalLength*fraction),right=tangentToRight(curve->tangentAt(curve->totalLength*fraction));
					const Vec3 left=center-right*GenerationSettings::get().network_mountainRoadSideSample,other=center+right*GenerationSettings::get().network_mountainRoadSideSample;
					slopes+=Abs(world.sampleHeight(static_cast<float>(left.x),static_cast<float>(left.z))
						-world.sampleHeight(static_cast<float>(other.x),static_cast<float>(other.z)))>GenerationSettings::get().network_mountainRoadRelief;
				}
				if (slopes>=2) { role=GeneratedStreet::Role::Mountain; }
			}
			GeneratedStreet::apply(*edge,GeneratedStreet::describe(role));
		}
	}


	for (int si = 0; si < static_cast<int>(settlements.size()); ++si)
	{
		auto& s = settlements[si];
		const float kaidoSearchRadius = Max(GenerationSettings::get().network_minimumKaidoSearchRadius, s.radius * GenerationSettings::get().network_kaidoSearchRadiusScale);
		const DistrictRoads::KaidoSegment kaido = DistrictRoads::extractKaido(
			s, network, kaidoSearchRadius);

		DistrictRoads::generateSettlement(seed, si, s, kaido, world, network);

		if (onProgress)
			onProgress(static_cast<float>(si + 1) / settlements.size());
	}

	LeveeRoad::generate(seed,world,network);

	Array<int> emptyNodes;
	for (const auto& node : network.nodes())
	{
		if (node.id>=0 && node.attachments.isEmpty()) { emptyNodes << node.id; }
	}
	for (const int id : emptyNodes) { network.removeNode(id); }

	Logger << U"[DistrictRoads] 完了: districts={} ({:.0f}ms)"_fmt(
		settlements.size(), swTotal.msF());
}

// ─────────────────────────────────────────────────────────────────────────────
// 鉄道初期設定
// ─────────────────────────────────────────────────────────────────────────────

void MapGenerator::setupTrain(TrainNetwork& trainNet, World& world,
                              const Array<Settlement>& districts, RoadNetwork* roads)
{
	RailwayAlignment::generate(trainNet,world,districts,roads);
}
