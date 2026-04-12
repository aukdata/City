#pragma once
#include "Vehicle.hpp"
#include "TrafficLight.hpp"
#include "TrafficGraph.hpp"
#include "../sim/SimGraph.hpp"

/// @brief 車両シミュレーション定数
namespace TrafficConfig
{
	// ── スポーン ──
	constexpr float kSpawnSpeed        = 5.0f;   ///< 初期速度 [m/s]
	constexpr float kSpawnPosRatio     = 0.8f;   ///< ランダムスポーン位置（エッジ長の割合）
	constexpr float kSpawnPosRatioGoal = 0.5f;   ///< ゴール指定スポーン位置

	// ── 車線変更 ──
	constexpr float kLaneChangeProbability  = 0.01f;  ///< フレームあたり試行確率
	constexpr float kLaneChangeBlendRate    = 2.0f;   ///< ブレンド速度 [1/s]
	constexpr float kRightLaneGapMultiplier = 4.0f;   ///< 右車線変更 gap 閾値倍率

	// ── 速度低減 ──
	constexpr float kConnectionSpeedFactor = 0.9f;  ///< 交差点進入時
	constexpr float kDirectTransitFactor   = 0.8f;  ///< エッジ直接遷移時
	constexpr float kActivationSpeedFactor = 0.8f;  ///< Dormant→Active 時

	// ── 交通制御 ──
	constexpr float kStopLineOffset       = 2.0f;   ///< 停止線オフセット [m]
	constexpr float kStopArrivalThreshold = 0.3f;   ///< 停止判定速度 [m/s]
	constexpr int   kMinEdgesForSignal    = 2;       ///< 信号設置最小エッジ数

	// ── Dormant ──
	constexpr float kDefaultDormantTime   = 1.0f;   ///< フォールバック遷移時間 [s]

	// ── 単位変換 ──
	constexpr float kKmhToMps = 3.6f;  ///< km/h ÷ kKmhToMps = m/s
}

/// @brief 交通シミュレーション共通ユーティリティ
namespace TrafficCommon
{
	// ========== 定数 ==========

	constexpr float kSignalStopDist = 15.0f;  ///< 信号停止検出距離 [m]
	constexpr float kStopSignDist   = 12.0f;  ///< 一時停止検出距離 [m]
	constexpr float kYieldDist      = 20.0f;  ///< 譲れ検出距離 [m]
	constexpr float kStopSignWait   = 1.5f;   ///< 一時停止の待機時間 [game sec]
	constexpr float kLaneChangeMinExitDist = 30.0f;  ///< 気まぐれ車線変更を行う出口までの最小距離 [m]
	constexpr float kLaneChangePerNeedDist = 60.0f;  ///< 経路駆動車線変更: 1 車線変更あたりの余裕距離 [m]
	// 経路駆動車線変更の urgency 閾値（plan/19_vehicle_movement_spec.md §7 参照）
	constexpr float kUrgencyRelaxStart = 0.3f;  ///< これ以上で安全マージンを段階的に縮小開始
	constexpr float kUrgencyAggressive = 0.7f;  ///< これ以上で強引モード（安全チェックスキップ）
	constexpr float kUrgencyForce      = 1.0f;  ///< これを超えたら強制スイッチ（距離不足）
	constexpr float kUrgencyMinScale   = 0.3f;  ///< 段階的緩和の安全マージン縮小下限
	constexpr float kFreeFlowGap   = 500.0f;  ///< 前方車両が遠い場合のフリーフロー閾値 [m]

	// ========== 車線判定 ==========

	/// @brief 指定車線が Forward 方向か判定する
	inline bool isForwardLane(const SimGraph::Edge& edge, int laneIdx)
	{
		if (laneIdx < 0 || laneIdx >= static_cast<int>(edge.lanes.size()))
			return true;
		return edge.lanes[laneIdx].dir == LaneDir::Forward;
	}

	// ========== エッジ選択 ==========

	/// @brief Forward 方向の走行可能な車線を持つエッジ ID を収集する
	inline Array<int> collectDrivableEdges(const SimGraph& simGraph)
	{
		Array<int> candidates;
		for (const auto& [eid, e] : simGraph.edges)
		{
			if (!e.isRoadbedBuilt()) continue;
			for (const auto& lane : e.lanes)
			{
				if ((lane.op == OpState::Open || lane.op == OpState::Provisional)
					&& lane.dir == LaneDir::Forward)
				{
					candidates << e.id;
					break;
				}
			}
		}
		return candidates;
	}

	/// @brief 現在エッジ以外からランダムにゴールエッジを選ぶ（候補なしなら -1）
	inline int selectRandomGoalEdge(const SimGraph& simGraph, int currentEdge)
	{
		const auto ids = simGraph.edgeIds();
		Array<int> candidates;
		for (const int eid : ids)
			if (eid != currentEdge) candidates << eid;
		if (candidates.isEmpty()) return -1;
		return candidates[Random(0, static_cast<int>(candidates.size()) - 1)];
	}

	// ========== IDM ==========

	/// @brief IDM 加速度を計算する [m/s^2]
	/// @param vehicles   車両リスト（前方車両の検索対象）
	/// @param self       対象車両
	/// @param params     IDM パラメータ
	/// @param fwdLane    Forward 車線かどうか
	/// @param activeOnly true のとき Active モード以外の車両をスキップする
	inline float idmAcceleration(const Array<Vehicle>& vehicles,
	                             const Vehicle& self,
	                             const IDMParams& params,
	                             bool fwdLane,
	                             bool activeOnly = false)
	{
		float gap   = 1e9f;
		float vLead = params.v0;

		for (const auto& other : vehicles)
		{
			if (other.id == self.id)               continue;
			if (activeOnly && other.mode != VehicleMode::Active) continue;
			if (other.currentEdge != self.currentEdge) continue;
			if (other.currentLane != self.currentLane) continue;

			const float delta = fwdLane
				? (other.arcPos - self.arcPos)
				: (self.arcPos - other.arcPos);

			if (delta > 0.0f && delta < gap)
			{
				gap   = delta;
				vLead = other.speed;
			}
		}

		const float vRatio = self.speed / Max(0.1f, params.v0);
		const float freeAccel = params.aMax * (1.0f - vRatio * vRatio * vRatio * vRatio);

		if (gap > kFreeFlowGap)
			return freeAccel;

		const float dv    = self.speed - vLead;
		const float sStar = params.s0 + Max(0.0f,
			self.speed * params.T + self.speed * dv / (2.0f * std::sqrtf(params.aMax * params.b)));
		const float safeGap = Max(0.1f, gap);

		return params.aMax * (
			1.0f - vRatio * vRatio * vRatio * vRatio
			- (sStar / safeGap) * (sStar / safeGap));
	}

	/// @brief 停止線手前での IDM 減速加速度を計算する
	/// @param speed      現在速度 [m/s]
	/// @param distToStop 停止線までの距離 [m]（正のとき手前）
	/// @param detectDist 減速開始距離 [m]
	/// @param params     IDM パラメータ
	/// @return 減速加速度。distToStop が範囲外なら NaN（適用不要）
	inline Optional<float> stopLineAccel(float speed, float distToStop,
	                                     float detectDist, const IDMParams& params)
	{
		if (distToStop <= 0.0f || distToStop >= detectDist)
			return none;

		const float dv    = speed;
		const float sStar = params.s0 + Max(0.0f,
			speed * params.T + speed * dv / (2.0f * std::sqrtf(params.aMax * params.b)));
		const float gap   = Max(0.1f, distToStop);
		const float vRatio = speed / Max(0.1f, params.v0);

		return params.aMax * (
			1.0f - vRatio * vRatio * vRatio * vRatio
			- (sStar / gap) * (sStar / gap));
	}

	// ========== 車線変更 ==========

	/// @brief 車線変更に必要な前方・後方ギャップを計測する
	inline void measureGaps(const Array<Vehicle>& vehicles,
	                        int selfId, int edgeId, int targetLane,
	                        float selfArcPos, bool fwdLane,
	                        bool activeOnly,
	                        float& outFrontGap, float& outRearGap)
	{
		outFrontGap = 1e9f;
		outRearGap  = 1e9f;
		for (const auto& other : vehicles)
		{
			if (other.id == selfId) continue;
			if (activeOnly && other.mode != VehicleMode::Active) continue;
			if (other.currentEdge != edgeId || other.currentLane != targetLane) continue;
			const float delta = fwdLane
				? (other.arcPos - selfArcPos)
				: (selfArcPos - other.arcPos);
			if (delta > 0.0f) outFrontGap = Min(outFrontGap, delta);
			else              outRearGap  = Min(outRearGap, -delta);
		}
	}

	/// @brief 車線変更先が安全かどうかを判定する
	inline bool isLaneChangeSafe(const SimGraph::Edge& edge, int targetLane,
	                             bool fwdLane, float frontGap, float rearGap,
	                             const IDMParams& params)
	{
		if (targetLane < 0 || targetLane >= static_cast<int>(edge.lanes.size())) return false;
		const Lane& tgt = edge.lanes[targetLane];
		if (!(edge.isRoadbedBuilt() && (tgt.op == OpState::Open || tgt.op == OpState::Provisional)))
			return false;
		const LaneDir dir = fwdLane ? LaneDir::Forward : LaneDir::Backward;
		if (tgt.dir != dir) return false;

		constexpr float kDeltaVMax  = 15.0f;
		const float safetyFront = params.s0 + 8.0f;
		const float safetyRear  = params.s0 + params.T * kDeltaVMax + 8.0f;
		return (frontGap >= safetyFront) && (rearGap >= safetyRear);
	}

	// ========== 旋回分類 (45° ルール) ==========

	/// @brief 進入・退出方向角から旋回種別を分類する（45° ルール、プリミティブ版）
	/// @details
	///   |θ| ≤ 45°            → Straight
	///   45° < θ < 135°       → Left
	///   -135° < θ < -45°     → Right
	///   |θ| ≥ 135°           → UTurn
	///
	///   経路探索・信号フェーズ自動生成・矢印ランプ描画の 3 箇所で共通使用する。
	///   詳細は plan/08_pathfinding_spec.md §3 参照。
	/// @param inAngle  ノードに到達する直前の進行方向 [rad] (atan2(tx, tz))
	/// @param outAngle ノードを出た直後の進行方向 [rad]
	inline TurnType classifyTurnByAngles(float inAngle, float outAngle)
	{
		const float cosIn  = std::cos(inAngle),  sinIn  = std::sin(inAngle);
		const float cosOut = std::cos(outAngle), sinOut = std::sin(outAngle);

		const float dot   = cosIn * cosOut + sinIn * sinOut;
		const float cross = cosIn * sinOut - sinIn * cosOut;

		// 45° ルール: cos 45° = √2/2 ≈ 0.7071
		constexpr float kCos45 = 0.70710678f;
		if (dot >=  kCos45) return TurnType::Straight;
		if (dot <= -kCos45) return TurnType::UTurn;
		return (cross > 0.0f) ? TurnType::Left : TurnType::Right;
	}

	/// @brief LaneConnection を旋回種別に分類する（SimGraph 版）
	inline TurnType classifyTurn(const SimGraph& graph, const LaneConnection& conn)
	{
		const SimGraph::Edge* fromE = graph.getEdge(conn.fromEdgeId);
		const SimGraph::Edge* toE   = graph.getEdge(conn.toEdgeId);
		if (!fromE || !toE) return TurnType::Straight;
		if (conn.fromLaneIndex < 0 || conn.fromLaneIndex >= static_cast<int>(fromE->lanes.size())) return TurnType::Straight;
		if (conn.toLaneIndex   < 0 || conn.toLaneIndex   >= static_cast<int>(toE->lanes.size()))   return TurnType::Straight;

		const LaneDir fromDir = fromE->lanes[conn.fromLaneIndex].dir;
		const LaneDir toDir   = toE->lanes[conn.toLaneIndex].dir;

		// 進入: Forward なら nodeB 端の接線、Backward なら nodeA 端の接線を反転
		const float inAngle = (fromDir == LaneDir::Forward)
			? fromE->tangentAngleB
			: (fromE->tangentAngleA + static_cast<float>(Math::Pi));

		// 退出: Forward なら nodeA 端の接線、Backward なら nodeB 端の接線を反転
		const float outAngle = (toDir == LaneDir::Forward)
			? toE->tangentAngleA
			: (toE->tangentAngleB + static_cast<float>(Math::Pi));

		return classifyTurnByAngles(inAngle, outAngle);
	}

	/// @brief TurnType に対応する Transition コスト [ゲーム秒]
	/// @details LaneConnection 自体にはコストを持たせず、ターン種別のみで決定する。
	///   厳密な通過時間ではなく「極端な遠回りを避ける」目安。
	inline float costTransition(TurnType turn)
	{
		switch (turn)
		{
		case TurnType::Straight: return 2.0f;
		case TurnType::Left:     return 5.0f;
		case TurnType::Right:    return 8.0f;
		case TurnType::UTurn:    return 15.0f;
		}
		return 2.0f;
	}

	// ========== 信号フェーズ変換 ==========

	/// @brief SignalPhaseDef（永続化型）の配列を SignalPhase（実行時型）の配列に変換する
	inline Array<SignalPhase> convertPhaseDefs(const Array<SignalPhaseDef>& defs)
	{
		Array<SignalPhase> phases;
		phases.reserve(defs.size());
		for (const auto& pd : defs)
		{
			SignalPhase sp;
			sp.duration = pd.duration;
			sp.greenConnectionIds = pd.greenConnectionIds;
			phases << std::move(sp);
		}
		return phases;
	}

	// ========== 信号フェーズ生成 ==========

	/// @brief 全 LaneConnection を 1 フェーズで常時青にするフェーズを構築する
	/// @details RoadNetwork::buildDefaultSignalPhases() の直進ペア分割とは異なり、
	///   全接続を単一フェーズに収める最小実装。RoadNetwork が利用不可の場合のフォールバック。
	inline Array<SignalPhase> buildAllGreenPhase(const Array<LaneConnection>& laneConnections,
	                                             float duration = 30.0f)
	{
		SignalPhase phase;
		phase.duration = duration;
		for (const auto& conn : laneConnections)
		{
			phase.greenConnectionIds << conn.id;
		}

		Array<SignalPhase> phases;
		phases << std::move(phase);
		return phases;
	}
}
