#pragma once
#include "Vehicle.hpp"
#include "TrafficLight.hpp"
#include "../sim/SimGraph.hpp"

/// @brief VehicleManager / TrafficManager 間で共通する交通ユーティリティ
namespace TrafficCommon
{
	// ========== 定数 ==========

	constexpr float kSignalStopDist = 15.0f;  ///< 信号停止検出距離 [m]
	constexpr float kStopSignDist   = 12.0f;  ///< 一時停止検出距離 [m]
	constexpr float kYieldDist      = 20.0f;  ///< 譲れ検出距離 [m]
	constexpr float kStopSignWait   = 1.5f;   ///< 一時停止の待機時間 [game sec]
	constexpr float kLaneChangeMinExitDist = 30.0f;  ///< 車線変更を行う出口までの最小距離 [m]
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

	// ========== 信号フェーズ生成 ==========

	/// @brief エッジリストを2グループに分けて信号フェーズを構築する
	inline Array<SignalPhase> buildTwoGroupPhases(const Array<int>& edgeIds, float duration = 30.0f)
	{
		const int half = static_cast<int>(edgeIds.size()) / 2;
		SignalPhase phaseA; phaseA.duration = duration;
		SignalPhase phaseB; phaseB.duration = duration;
		for (int k = 0; k < static_cast<int>(edgeIds.size()); ++k)
		{
			if (k < half)
				phaseA.greenEdgeIds << edgeIds[k];
			else
				phaseB.greenEdgeIds << edgeIds[k];
		}
		Array<SignalPhase> phases;
		phases << std::move(phaseA) << std::move(phaseB);
		return phases;
	}
}
