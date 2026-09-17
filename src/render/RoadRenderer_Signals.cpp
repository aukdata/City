#include "RoadRenderer.hpp"
#include "../road/RoadGeometry.hpp"
#include "../traffic/TrafficCommon.hpp"

/// @file
/// @brief 信号の配置キャッシュ・現示・選択輪郭の描画。道路の路面メッシュ生成とは別に更新する。

const Mesh* RoadRenderer::getSignalMesh(const String& defId, const String& meshName, int lod)
{
	auto& cache = m_signalMeshCache[defId];
	const String key = lod == 0 ? meshName : meshName + U"_lod{}"_fmt(lod);
	if (auto it = cache.meshes.find(key); it != cache.meshes.end())
		return &it->second;

	const SignalModel* model = m_signalRegistry.getModel(defId);
	if (!model) return nullptr;

	const auto& meshes = lod == 0 ? model->meshes : model->lodMeshes[lod-1];
	auto mit = meshes.find(meshName);
	if (mit == meshes.end()) return nullptr;

	const auto& partModelData = mit->second;
	if (partModelData.isEmpty()) return nullptr;

	cache.meshes[key] = Mesh{ MeshData{ partModelData.vertices, partModelData.indices } };
	return &cache.meshes[key];
}

HashTable<int, RoadRenderer::EdgeSignalSummary>
RoadRenderer::buildEdgeSignalSummaries(const RoadNode& node,
                                        const SimGraph& simGraph,
                                        const TrafficLight* tl)
{
	HashTable<int, EdgeSignalSummary> summaries;
	for (const auto& conn : node.laneConnections)
	{
		const TurnType turn = TrafficCommon::classifyTurn(simGraph, conn);
		if (turn == TurnType::UTurn) continue;  // 描画上は無視

		const bool connGreen = tl ? tl->isGreen(conn.id) : true;
		EdgeSignalSummary& sum = summaries[conn.fromEdgeId];
		switch (turn)
		{
		case TurnType::Straight: sum.hasStraight = true; if (connGreen) sum.straightGreen = true; break;
		case TurnType::Left:     sum.hasLeft     = true; if (connGreen) sum.leftGreen     = true; break;
		case TurnType::Right:    sum.hasRight    = true; if (connGreen) sum.rightGreen    = true; break;
		default: break;
		}
	}
	return summaries;
}

void RoadRenderer::ensureSignalAttachGeomCache(const RoadNode& node, const RoadNetwork& network,
                                               const World& world, bool elevated,
                                               Array<SignalAttachGeomCache>& cacheArr) const
{
	// サイズが一致していればキャッシュ済み。道路変更時は呼び出し側で erase される
	if (cacheArr.size() == node.attachments.size()) return;

	cacheArr.assign(node.attachments.size(), SignalAttachGeomCache{});

	for (size_t ai = 0; ai < node.attachments.size(); ++ai)
	{
		const auto& att = node.attachments[ai];
		if (att.control != TrafficControl::Signal) continue;

		const RoadEdge* edge = network.getEdge(att.edgeId);
		if (!edge) continue;
		const auto bez = network.getBezier(att.edgeId);
		if (!bez) continue;

		const auto anchor = RoadGeometry::signalAnchor(*edge, *bez, node.id);
		if (!anchor) { continue; }
		const Vec3 cutPos = anchor->roadPosition;
		const double signalY = elevated
			? cutPos.y + kRoadSurfaceLift
			: static_cast<double>(world.sampleHeight(
				static_cast<float>(cutPos.x), static_cast<float>(cutPos.z))) + kRoadSurfaceLift;

		const Vec3 signalPos{anchor->position.x, signalY, anchor->position.z};
		cacheArr[ai].baseMat = Mat4x4::RotateY(anchor->yaw)
			* Mat4x4::Translate(Float3{
				static_cast<float>(signalPos.x),
				static_cast<float>(signalPos.y),
				static_cast<float>(signalPos.z) });
		cacheArr[ai].valid = true;
	}
}

void RoadRenderer::drawSignals(const RoadNetwork& network, const SimGraph& simGraph,
                               const World& world,
                               const HashTable<int, TrafficLight>& trafficLights,
                               GameTime gameNow, Vec3 cameraPos)
{
	constexpr double kSignalDrawMaxDistSq = 500.0 * 500.0;

	// 交差点ごとの進入方向サマリーと取付行列を再利用しながら、信号灯器をまとめて描画する。
	for (const auto& node : network.nodes())
	{
		if (node.id < 0 || !node.signalPlacement)
		{
			continue;
		}

		// 描画範囲外の信号では、定義・モデルの照会も行わない。
		const double distanceSq = node.position.distanceFromSq(cameraPos);
		const int lod = distanceSq < 80*80 ? 0 : (distanceSq < 200*200 ? 1 : 2);
		if (distanceSq > kSignalDrawMaxDistSq)
		{
			continue;
		}

		const auto& sigPlacement = *node.signalPlacement;
		const SignalDef* def = m_signalRegistry.getDef(sigPlacement.signalDefId);
		const SignalModel* model = def ? m_signalRegistry.getModel(sigPlacement.signalDefId) : nullptr;
		if (!def || !model || !model->texture)
		{
			continue;
		}

		const bool elevated = network.nodeUsesDesignHeight(node.id);

		// この交差点の LaneConnection を進入エッジ別 × 旋回別に集計する。
		// attachments ループの中で繰り返し計算しないようここで一度だけ作る。
		const auto tlIt = trafficLights.find(node.id);
		const TrafficLight* tl = (tlIt != trafficLights.end()) ? &tlIt->second : nullptr;

		// フェーズが変化したときのみ再構築（毎フレームのアロケーションを回避）
		const int phaseIdx = tl ? tl->currentPhaseIndex() : -1;
		auto& sCache = m_signalSummaryCache[node.id];
		if (sCache.lastPhaseIdx != phaseIdx)
		{
			sCache.summaries    = buildEdgeSignalSummaries(node, simGraph, tl);
			sCache.lastPhaseIdx = phaseIdx;
		}
		const auto& edgeSummaries = sCache.summaries;

		// アタッチメントの変換行列キャッシュを構築（未構築の場合のみ）
		auto& geomCacheArr = m_signalAttachGeomCache[node.id];
		ensureSignalAttachGeomCache(node, network, world, elevated, geomCacheArr);

		for (size_t ai = 0; ai < node.attachments.size(); ++ai)
		{
			const auto& att = node.attachments[ai];
			if (att.control != TrafficControl::Signal) continue;
			if (!geomCacheArr[ai].valid) continue;

			const Mat4x4& baseMat = geomCacheArr[ai].baseMat;

			// 筐体メッシュ描画
			if (const Mesh* bodyMesh = getSignalMesh(sigPlacement.signalDefId, def->bodyMeshName, lod))
			{
				PhongMaterial bodyMat;
				bodyMat.ambientColor = ColorF{ 0.5 };
				bodyMat.diffuseColor = ColorF{ 1.0 };
				bodyMat.hasDiffuseTexture = true;
				bodyMesh->draw(baseMat, *model->texture, bodyMat);
			}

			// 進入エッジ単位の集計サマリーを引き当てる（loop 外で計算済み）
			const auto sumIt = edgeSummaries.find(att.edgeId);
			const EdgeSignalSummary sum = (sumIt != edgeSummaries.end())
				? sumIt->second
				: EdgeSignalSummary{};

			// メインランプ: 直進 LaneConnection が青なら緑。直進が無ければ全方向の論理和
			const bool isGreen = sum.hasStraight
				? sum.straightGreen
				: (sum.leftGreen || sum.rightGreen);

			bool isYellow = false;
			if (tl)
			{
				const float elapsed = tl->phaseElapsed(gameNow);
				const float duration = tl->currentPhaseDuration();
				if (isGreen && duration > 0.0f && (duration - elapsed) < kYellowDuration)
				{
					isYellow = true;
				}
			}

			// メインランプ描画
			for (size_t li = 0; li < def->lamps.size(); ++li)
			{
				const auto& lampDef = def->lamps[li];
				const Mesh* lampMesh = getSignalMesh(sigPlacement.signalDefId, lampDef.meshName, lod);
				if (!lampMesh)
				{
					continue;
				}

				// ランプの状態を決定
				String stateId = U"off";
				if (lampDef.stateIds.contains(U"green") && isGreen && !isYellow)
				{
					stateId = U"green";
				}
				else if (lampDef.stateIds.contains(U"yellow") && isYellow)
				{
					stateId = U"yellow";
				}
				else if (lampDef.stateIds.contains(U"red") && !isGreen && !isYellow)
				{
					stateId = U"red";
				}

				auto stIt = def->states.find(stateId);
				if (stIt == def->states.end())
				{
					stIt = def->states.find(U"off");
				}
				if (stIt == def->states.end())
				{
					continue;
				}

				const auto& state = stIt->second;
				const TextureRegion texRegion = (*model->texture)(
					static_cast<int>(state.uvRect.x),
					static_cast<int>(state.uvRect.y),
					static_cast<int>(state.uvRect.z),
					static_cast<int>(state.uvRect.w));

				lampMesh->draw(baseMat, texRegion);
			}

			// sub_lamp（矢印信号）描画 — フェーズから自動導出
			// 進入エッジに属する LaneConnection を旋回別に分類し、
			// メインランプが赤のときに該当方向が青の場合のみ矢印を点灯する。
			// 配置列: arrow_left=0, arrow_straight=1, arrow_right=2
			if (def->subLamp)
			{
				const auto& subLampDef = *def->subLamp;
				const Mesh* subLampMesh = getSignalMesh(sigPlacement.signalDefId, subLampDef.meshName, lod);
				const Mesh* subBodyMesh = getSignalMesh(sigPlacement.signalDefId, subLampDef.bodyMeshName, lod);

				// メインランプが青/黄のときは矢印は点灯しない（重複を避ける）
				const bool mainLit = (isGreen || isYellow);

				// 各方向の点灯判定: 該当 LaneConnection が青 かつ メインが赤
				struct ArrowSlot { int col; String stateId; bool lit; };
				Array<ArrowSlot> slots;
				if (sum.hasLeft)
					slots << ArrowSlot{ 0, U"arrow_left",     sum.leftGreen     && !mainLit };
				if (sum.hasStraight)
					slots << ArrowSlot{ 1, U"arrow_straight", sum.straightGreen && !mainLit };
				if (sum.hasRight)
					slots << ArrowSlot{ 2, U"arrow_right",    sum.rightGreen    && !mainLit };

				for (const auto& s : slots)
				{
					const int row = s.col / subLampDef.cols;
					const int c   = s.col % subLampDef.cols;
					const Float3 offset{
						subLampDef.colStride.x * c + subLampDef.rowStride.x * row,
						subLampDef.colStride.y * c + subLampDef.rowStride.y * row,
						subLampDef.colStride.z * c + subLampDef.rowStride.z * row
					};

					const Mat4x4 subLampMat = Mat4x4::Translate(offset) * baseMat;

					if (subBodyMesh)
					{
						PhongMaterial subBodyMat;
						subBodyMat.ambientColor = ColorF{ 0.5 };
						subBodyMat.diffuseColor = ColorF{ 1.0 };
						subBodyMat.hasDiffuseTexture = true;
						subBodyMesh->draw(subLampMat, *model->texture, subBodyMat);
					}

					if (subLampMesh)
					{
						const String renderStateId = s.lit ? s.stateId : U"off";
						auto renderStIt = def->states.find(renderStateId);
						if (renderStIt == def->states.end())
						{
							continue;
						}
						const auto& renderState = renderStIt->second;

						const TextureRegion texRegion = (*model->texture)(
							static_cast<int>(renderState.uvRect.x),
							static_cast<int>(renderState.uvRect.y),
							static_cast<int>(renderState.uvRect.z),
							static_cast<int>(renderState.uvRect.w));

						subLampMesh->draw(subLampMat, texRegion);
					}
				}
			}
		}
	}

}

void RoadRenderer::drawSignalSilhouette(int nodeId, const RoadNetwork& network, const World& world,
                                         const ColorF& color)
{
	const RoadNode* node = network.getNode(nodeId);
	if (!node || !node->signalPlacement) return;

	const auto& sigPlacement = *node->signalPlacement;
	const SignalDef* def = m_signalRegistry.getDef(sigPlacement.signalDefId);
	if (!def) return;

	const bool elevated = network.nodeUsesDesignHeight(nodeId);
	auto& geomCacheArr = m_signalAttachGeomCache[nodeId];
	ensureSignalAttachGeomCache(*node, network, world, elevated, geomCacheArr);

	for (size_t ai = 0; ai < node->attachments.size(); ++ai)
	{
		const auto& att = node->attachments[ai];
		if (att.control != TrafficControl::Signal) continue;
		if (ai >= geomCacheArr.size() || !geomCacheArr[ai].valid) continue;

		const Mat4x4& baseMat = geomCacheArr[ai].baseMat;
		if (const Mesh* bodyMesh = getSignalMesh(sigPlacement.signalDefId, def->bodyMeshName))
			bodyMesh->draw(baseMat, color);
		for (const auto& lampDef : def->lamps)
		{
			if (const Mesh* lampMesh = getSignalMesh(sigPlacement.signalDefId, lampDef.meshName))
				lampMesh->draw(baseMat, color);
		}
		if (def->subLamp)
		{
			if (const Mesh* subMesh = getSignalMesh(sigPlacement.signalDefId, def->subLamp->meshName))
				subMesh->draw(baseMat, color);
		}
	}
}
