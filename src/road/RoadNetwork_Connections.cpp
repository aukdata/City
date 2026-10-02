#include "RoadNetwork.hpp"
#include <array>
#include <map>
#include <limits>
#include <stdexcept>
#include "RoadGeometry.hpp"
#include "../debug/DebugLog.hpp"
#include "../world/World.hpp"

namespace
{
	/// @brief カットオフ位置での車線端点（ワールド座標・接線・方向符号）を計算する
	struct LaneEndpoint
	{
		Vec3  worldPos;  ///< 車線中心のワールド座標
		Vec3  tangent;   ///< ベジェ接線ベクトル
		float dirSign;   ///< Forward: +1, Backward: -1
	};

	LaneEndpoint calcLaneEndpoint(
		const CubicBezier& bez, const RoadEdge& edge, const Lane& lane,
		int nodeId)
	{
		const bool isNodeA = (edge.nodeA == nodeId);
		const float cutoff = isNodeA ? edge.cutoffA : edge.cutoffB;
		const float arc = isNodeA
			? cutoff
			: (bez.totalLength - cutoff);

		const Vec3 pos = bez.positionAt(arc);
		const Vec3 tan = bez.tangentAt(arc);

		// 車線オフセットを適用
		const float ft = (bez.totalLength > 0.0f) ? (arc / bez.totalLength) : 0.0f;
		const float offset = lane.centerAt(ft);
		const Vec3 perp = tangentToRight(tan);

		return {
			pos + perp * static_cast<double>(offset),
			tan,
			(lane.dir == LaneDir::Forward) ? 1.0f : -1.0f
		};
	}

}

HashTable<int, Vec2> RoadNetwork::buildEdgeDirs(int nodeId) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return {};

	HashTable<int, Vec2> edgeDirs;
	for (const auto& att : node->attachments)
	{
		const auto bez = getBezier(att.edgeId);
		const RoadEdge* e = getEdge(att.edgeId);
		if (!bez || !e) continue;
		Vec3 tan;
		if (e->nodeA == nodeId)
			tan = -bez->tangent(0.0f);
		else
			tan = bez->tangent(1.0f);
		edgeDirs[att.edgeId] = Vec2{ tan.x, tan.z }.normalized();
	}
	return edgeDirs;
}

Array<Array<int>> RoadNetwork::buildStraightPairs(int nodeId,
                                                   const HashTable<int, Vec2>& edgeDirs) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return {};

	constexpr float kOppositeAngleThreshold = static_cast<float>(Math::QuarterPi);
	HashSet<int> selected;
	Array<Array<int>> pairs;

	for (const auto& att : node->attachments)
	{
		if (selected.contains(att.edgeId)) continue;
		selected.insert(att.edgeId);

		const auto dirIt = edgeDirs.find(att.edgeId);
		if (dirIt == edgeDirs.end()) continue;
		const Vec2 opposite = -dirIt->second;

		int bestEdge = -1;
		float bestAngle = kOppositeAngleThreshold;
		for (const auto& other : node->attachments)
		{
			if (other.edgeId == att.edgeId || selected.contains(other.edgeId)) continue;
			const auto otherIt = edgeDirs.find(other.edgeId);
			if (otherIt == edgeDirs.end()) continue;
			const float angle = static_cast<float>(std::acos(std::clamp(
				opposite.dot(otherIt->second), -1.0, 1.0)));
			if (angle < bestAngle)
			{
				bestAngle = angle;
				bestEdge = other.edgeId;
			}
		}

		Array<int> pair;
		pair << att.edgeId;
		if (bestEdge >= 0)
		{
			pair << bestEdge;
			selected.insert(bestEdge);
		}
		pairs << std::move(pair);
	}
	return pairs;
}

void RoadNetwork::rebuildLaneConnections(int nodeId)
{
	RoadNode* node = getNode(nodeId);
	if (!node) return;
	const bool hadSignalPlacement = node->signalPlacement.has_value();

	// 旧接続の論理キー → ID マッピングを保持（信号フェーズの greenConnectionIds を維持するため）
	const auto makeKey = [](int fromEdge, int fromLane, int toEdge, int toLane)
	{
		return std::array<int,4>{fromEdge,fromLane,toEdge,toLane};
	};
	std::map<std::array<int,4>, int> oldKeyToId;
	HashSet<int> previousConnectionIds, previousEdgeIds;
	for (const auto& conn : node->laneConnections)
	{
		oldKeyToId[makeKey(conn.fromEdgeId, conn.fromLaneIndex, conn.toEdgeId, conn.toLaneIndex)] = conn.id;
		previousConnectionIds.insert(conn.id);
		previousEdgeIds.insert(conn.fromEdgeId);
		previousEdgeIds.insert(conn.toEdgeId);
	}

	node->laneConnections.clear();

	const auto allEdgeIds = node->edgeIds();
	if (allEdgeIds.size() < 2)
	{
		// All movements vanished; retain only intentionally empty all-red phases.
		if (node->signalPlacement)
		{
			node->signalPlacement->phases.remove_if([](const SignalPhaseDef& phase) { return !phase.greenConnectionIds.isEmpty(); });
		}
		return;
	}

	// ── エッジ方向マップ・直進ペア構築 ──
	const auto edgeDirs = buildEdgeDirs(nodeId);
	const auto pairs    = buildStraightPairs(nodeId, edgeDirs);

	// ── 車線参照 ──
	struct LaneRef
	{
		int   edgeId;
		int   laneIndex;
		float driverOffset; // 運転者視点の左右（負=左、正=右）
	};

	// Entry 車線（交差点に進入）を収集、運転者視点で左→右ソート
	auto collectEntryLanes = [&](int edgeId) -> Array<LaneRef>
	{
		const RoadEdge* edge = getEdge(edgeId);
		if (!edge || !edge->isRoadbedBuilt()) return {};
		Array<LaneRef> result;
		for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
		{
			const Lane& lane = edge->lanes[i];
			if (!lane.allows(TransportMode::Road) || (lane.op != OpState::Open && lane.op != OpState::Provisional)) continue;
			const bool exits =
				(lane.dir == LaneDir::Forward  && edge->nodeB == nodeId) ||
				(lane.dir == LaneDir::Backward && edge->nodeA == nodeId);
			if (!exits) continue;
			const bool isAtA = (edge->nodeA == nodeId);
			const float center = lane.centerAt(isAtA ? 0.0f : 1.0f);
			const float drvOff = (lane.dir == LaneDir::Forward) ? center : -center;
			result << LaneRef{ edgeId, i, drvOff };
		}
		result.sort_by([](const LaneRef& a, const LaneRef& b) { return a.driverOffset < b.driverOffset; });
		return result;
	};

	// Exit 車線（交差点から退出）を収集、オフセット昇順ソート
	auto collectExitLanes = [&](int edgeId) -> Array<LaneRef>
	{
		const RoadEdge* edge = getEdge(edgeId);
		if (!edge || !edge->isRoadbedBuilt()) return {};
		Array<LaneRef> result;
		for (int i = 0; i < static_cast<int>(edge->lanes.size()); ++i)
		{
			const Lane& lane = edge->lanes[i];
			if (!lane.allows(TransportMode::Road) || (lane.op != OpState::Open && lane.op != OpState::Provisional)) continue;
			const bool enters =
				(lane.dir == LaneDir::Forward  && edge->nodeA == nodeId) ||
				(lane.dir == LaneDir::Backward && edge->nodeB == nodeId);
			if (!enters) continue;
			const bool isAtA = (edge->nodeA == nodeId);
			const float center = lane.centerAt(isAtA ? 0.0f : 1.0f);
			const float drvOff = (lane.dir == LaneDir::Forward) ? center : -center;
			result << LaneRef{ edgeId, i, drvOff };
		}
		result.sort_by([](const LaneRef& a, const LaneRef& b) { return a.driverOffset < b.driverOffset; });
		return result;
	};

	// LaneConnection を生成して追加
	auto addConn = [&](const LaneRef& from, const LaneRef& to)
	{
		const RoadEdge* fromEdge = getEdge(from.edgeId);
		const RoadEdge* toEdge   = getEdge(to.edgeId);
		if (!fromEdge || !toEdge) return;
		const auto fromBez = getBezier(from.edgeId);
		const auto toBez   = getBezier(to.edgeId);
		if (!fromBez || !toBez) return;

		const auto exitPt  = calcLaneEndpoint(*fromBez, *fromEdge, fromEdge->lanes[from.laneIndex], nodeId);
		const auto entryPt = calcLaneEndpoint(*toBez,   *toEdge,   toEdge->lanes[to.laneIndex],     nodeId);

		const double dist   = (exitPt.worldPos - entryPt.worldPos).length();
		const double handle = Max(dist * 0.33, 5.0);
		const Vec3 p1 = exitPt.worldPos  + (exitPt.tangent  * exitPt.dirSign).normalized()  * handle;
		const Vec3 p2 = entryPt.worldPos - (entryPt.tangent * entryPt.dirSign).normalized() * handle;

		LaneConnection conn;
		const auto key = makeKey(from.edgeId, from.laneIndex, to.edgeId, to.laneIndex);
		if (const auto it = oldKeyToId.find(key); it != oldKeyToId.end())
			conn.id = it->second;
		else
		{
			if (node->nextConnectionId == std::numeric_limits<int>::max())
			{
				throw std::overflow_error("Road lane connection IDs exhausted");
			}
			conn.id = node->nextConnectionId++;
		}
		conn.fromEdgeId    = from.edgeId;
		conn.fromLaneIndex = from.laneIndex;
		conn.toEdgeId      = to.edgeId;
		conn.toLaneIndex   = to.laneIndex;
		conn.path          = CubicBezier{ exitPt.worldPos, p1, p2, entryPt.worldPos };
		node->laneConnections << std::move(conn);
	};

	// N本のEntry車線をM本のExit車線に振り分けて接続を生成
	auto distribute = [&](const Array<LaneRef>& entries, const Array<LaneRef>& exits)
	{
		const int N = static_cast<int>(entries.size());
		const int M = static_cast<int>(exits.size());
		if (N == 0 || M == 0) return;

		if (M >= N)
		{
			// Exit が多い: 各 Entry に floor(M/N) 本、一番右に残り全て
			const int per = M / N;
			int ei = 0;
			for (int i = 0; i < N; ++i)
			{
				const int count = (i == N - 1) ? (M - ei) : per;
				for (int j = 0; j < count; ++j)
					addConn(entries[i], exits[ei + j]);
				ei += count;
			}
		}
		else
		{
			// Entry が多い: 各 Exit に floor(N/M) 本、一番右 Exit に残り全て
			const int per = N / M;
			int ni = 0;
			for (int j = 0; j < M; ++j)
			{
				const int count = (j == M - 1) ? (N - ni) : per;
				for (int i = 0; i < count; ++i)
					addConn(entries[ni + i], exits[j]);
				ni += count;
			}
		}
	};

	// ── 方向種別 ──
	enum class TurnDir { AllDirections, Left, StraightLeft, Straight, StraightRight, Right };

	// ── 各ペアのエッジについて方向別接続を生成 ──
	for (const auto& pair : pairs)
	{
		for (const int entryEdgeId : pair)
		{
			const auto entryDirIt = edgeDirs.find(entryEdgeId);
			if (entryDirIt == edgeDirs.end()) continue;

			// ペア相手
			int pairedEdgeId = -1;
			if (pair.size() == 2)
				pairedEdgeId = (pair[0] == entryEdgeId) ? pair[1] : pair[0];

			// Entry 車線を収集
			auto entryLanes = collectEntryLanes(entryEdgeId);
			if (entryLanes.isEmpty()) continue;
			const int A = static_cast<int>(entryLanes.size());

			// ペア相手の Exit 車線
			auto pairedExits = (pairedEdgeId >= 0) ? collectExitLanes(pairedEdgeId) : Array<LaneRef>{};
			const int B = static_cast<int>(pairedExits.size());

			// 左右エッジを判定し角度でソート（ペア方向に近い順）
			const Vec2 entryDir = entryDirIt->second;
			const Vec2 straightDir = (pairedEdgeId >= 0 && edgeDirs.count(pairedEdgeId))
				? edgeDirs.at(pairedEdgeId) : -entryDir;

			struct EdgeAngle { int edgeId; float angle; };
			Array<EdgeAngle> leftEdges, rightEdges;

			for (const auto& att : node->attachments)
			{
				if (att.edgeId == entryEdgeId || att.edgeId == pairedEdgeId) continue;
				const auto otherIt = edgeDirs.find(att.edgeId);
				if (otherIt == edgeDirs.end()) continue;

				const Vec2 otherDir = otherIt->second;
				// 外積: 正→右、負→左（運転者視点）
				const float cross = static_cast<float>(entryDir.x * otherDir.y - entryDir.y * otherDir.x);
				const float angleDot = static_cast<float>(std::clamp(
					straightDir.dot(otherDir), -1.0, 1.0));
				const float angle = static_cast<float>(std::acos(angleDot));

				if (cross < 0.0f)
					leftEdges << EdgeAngle{ att.edgeId, angle };
				else if (cross > 0.0f)
					rightEdges << EdgeAngle{ att.edgeId, angle };
			}

			leftEdges.sort_by([](const EdgeAngle& a, const EdgeAngle& b) { return a.angle < b.angle; });
			rightEdges.sort_by([](const EdgeAngle& a, const EdgeAngle& b) { return a.angle < b.angle; });

			// 左右 Exit 車線プールを構築（角度順に連結）
			Array<LaneRef> leftExitPool, rightExitPool;
			for (const auto& le : leftEdges)
				leftExitPool.append(collectExitLanes(le.edgeId));
			for (const auto& re : rightEdges)
				rightExitPool.append(collectExitLanes(re.edgeId));

			const int L = static_cast<int>(leftExitPool.size());
			const int R = static_cast<int>(rightExitPool.size());

			// ── Phase 1: 方向割り当て ──
			Array<TurnDir> laneDirs(A, TurnDir::Straight);

			if (A == 1)
			{
				laneDirs[0] = TurnDir::AllDirections;
			}
			else if (A <= B)
			{
				laneDirs[0]     = TurnDir::StraightLeft;
				laneDirs[A - 1] = TurnDir::StraightRight;
			}
			else // A > B
			{
				int numLeft  = Min((A - B) / 2, L);
				int numRight = Min((A - B + 1) / 2, R);

				for (int i = 0; i < numLeft; ++i)
					laneDirs[i] = TurnDir::Left;
				for (int i = 0; i < numRight; ++i)
					laneDirs[A - 1 - i] = TurnDir::Right;

				// 補正: L>0 かつ左折なしなら一番左を左折直進に
				if (L > 0 && numLeft == 0)
					laneDirs[0] = TurnDir::StraightLeft;

				// B=0: 直進先なし → 残りの Straight を Left/Right に再分配
				if (B == 0)
				{
					// 左側から Left に割り当て
					for (int i = 0; i < A; ++i)
					{
						if (laneDirs[i] != TurnDir::Straight) continue;
						int assignedLeft = 0;
						for (int j = 0; j < A; ++j)
							if (laneDirs[j] == TurnDir::Left || laneDirs[j] == TurnDir::StraightLeft) assignedLeft++;
						if (assignedLeft < L)
							laneDirs[i] = TurnDir::Left;
						else
							break;
					}
					// 右側から Right に割り当て
					for (int i = A - 1; i >= 0; --i)
					{
						if (laneDirs[i] != TurnDir::Straight) continue;
						int assignedRight = 0;
						for (int j = 0; j < A; ++j)
							if (laneDirs[j] == TurnDir::Right || laneDirs[j] == TurnDir::StraightRight) assignedRight++;
						if (assignedRight < R)
							laneDirs[i] = TurnDir::Right;
						else
							break;
					}
				}
			}

			// ── Phase 2: 方向別に接続を生成 ──
			Array<LaneRef> straightEntries, leftEntries, rightEntries;

			for (int i = 0; i < A; ++i)
			{
				switch (laneDirs[i])
				{
				case TurnDir::AllDirections:
					for (const auto& ex : pairedExits)    addConn(entryLanes[i], ex);
					for (const auto& ex : leftExitPool)   addConn(entryLanes[i], ex);
					for (const auto& ex : rightExitPool)  addConn(entryLanes[i], ex);
					break;
				case TurnDir::Straight:
					straightEntries << entryLanes[i];
					break;
				case TurnDir::StraightLeft:
					straightEntries << entryLanes[i];
					leftEntries     << entryLanes[i];
					break;
				case TurnDir::StraightRight:
					straightEntries << entryLanes[i];
					rightEntries    << entryLanes[i];
					break;
				case TurnDir::Left:
					leftEntries << entryLanes[i];
					break;
				case TurnDir::Right:
					rightEntries << entryLanes[i];
					break;
				}
			}

			distribute(straightEntries, pairedExits);
			distribute(leftEntries, leftExitPool);

			// 右折: 入力を右端から順にするため反転
			if (!rightEntries.isEmpty() && !rightExitPool.isEmpty())
			{
				rightEntries.reverse();
				distribute(rightEntries, rightExitPool);
			}
		}
	}

	// ── 自動信号設置 ──
	constexpr int kAutoSignalThreshold = 8;
	bool hasMajorApproach=false;
	for(const auto& attachment:node->attachments)
	{
		const RoadEdge* edge=getEdge(attachment.edgeId);
		if(!edge || edge->farmAccess) { continue; }
		hasMajorApproach |= edge->roadType==RoadType::Arterial || edge->roadType==RoadType::Highway
			|| (edge->roadType==RoadType::LocalRoad && edge->lanes.size()>2);
	}
	// 細い生活道路・畦道だけなら、接続数の多い四差路でも無信号とする。
	if (hasMajorApproach && static_cast<int>(node->laneConnections.size()) > kAutoSignalThreshold)
	{
		if (!node->signalPlacement)
		{
			SignalPlacement sp;
			sp.signalDefId = U"signal_3lamp";
			sp.phases = buildDefaultSignalPhases(nodeId);
			node->signalPlacement = sp;
		}
	}

	// Signal newly attached roads; retain authored controls on existing approaches.
	// Both source and destination refs count, including a previously outgoing-only arm.
	if (node->signalPlacement)
	{
		for (auto& attachment : node->attachments)
		{
			const RoadEdge* edge = getEdge(attachment.edgeId);
			if ((!hadSignalPlacement || !previousEdgeIds.contains(attachment.edgeId))
				&& edge && edge->lanes.any([](const Lane& lane) { return lane.allows(TransportMode::Road); }))
			{
				attachment.control = TrafficControl::Signal;
			}
		}

		// Preserve authored timing and surviving movement IDs. New movements receive
		// separate default groups, rather than sharing a potentially conflicting phase.
		auto& phases = node->signalPlacement->phases;
		if (!phases.isEmpty())
		{
			HashSet<int> currentIds, coveredIds;
			for (const auto& connection : node->laneConnections) { currentIds.insert(connection.id); }
			for (size_t index = phases.size(); index > 0; --index)
			{
				auto& phase = phases[index - 1];
				const bool hadMovements = !phase.greenConnectionIds.isEmpty();
				phase.greenConnectionIds.remove_if([&](int id) { return !currentIds.contains(id); });
				if (hadMovements && phase.greenConnectionIds.isEmpty()) { phases.remove_at(index - 1); }
			}
			for (const auto& phase : phases)
			{
				for (const int id : phase.greenConnectionIds) { coveredIds.insert(id); }
			}
			for (auto phase : buildDefaultSignalPhases(nodeId))
			{
				phase.greenConnectionIds.remove_if([&](int id)
				{
					return previousConnectionIds.contains(id) || coveredIds.contains(id);
				});
				if (!phase.greenConnectionIds.isEmpty()) { phases << std::move(phase); }
			}
		}
	}
}

void RoadNetwork::updateLaneConnectionPaths(int nodeId)
{
	RoadNode* node = getNode(nodeId);
	if (!node) return;

	for (auto& conn : node->laneConnections)
	{
		const RoadEdge* fromEdge = getEdge(conn.fromEdgeId);
		const RoadEdge* toEdge   = getEdge(conn.toEdgeId);
		if (!fromEdge || !toEdge) continue;

		const auto fromBez = getBezier(conn.fromEdgeId);
		const auto toBez   = getBezier(conn.toEdgeId);
		if (!fromBez || !toBez) continue;

		if (conn.fromLaneIndex >= static_cast<int>(fromEdge->lanes.size())) continue;
		if (conn.toLaneIndex   >= static_cast<int>(toEdge->lanes.size()))   continue;

		const auto exitPt  = calcLaneEndpoint(*fromBez, *fromEdge, fromEdge->lanes[conn.fromLaneIndex], nodeId);
		const auto entryPt = calcLaneEndpoint(*toBez,   *toEdge,   toEdge->lanes[conn.toLaneIndex],     nodeId);

		const double dist   = (exitPt.worldPos - entryPt.worldPos).length();
		const double handle = Max(dist * 0.33, 5.0);
		const Vec3 p1 = exitPt.worldPos  + (exitPt.tangent  * exitPt.dirSign).normalized()  * handle;
		const Vec3 p2 = entryPt.worldPos - (entryPt.tangent * entryPt.dirSign).normalized() * handle;

		conn.path = CubicBezier{ exitPt.worldPos, p1, p2, entryPt.worldPos };
	}
}

Array<SignalPhaseDef> RoadNetwork::buildDefaultSignalPhases(int nodeId) const
{
	const RoadNode* node = getNode(nodeId);
	if (!node) return {};

	constexpr float kMinPhaseDuration = 5.0f;       // 実時間秒
	constexpr float kDurationPerConnection = 2.0f;  // 実時間秒

	// エッジ方向マップ・直進ペア構築
	const auto edgeDirs = buildEdgeDirs(nodeId);
	const auto pairs    = buildStraightPairs(nodeId, edgeDirs);

	// フェーズの生成
	Array<SignalPhaseDef> phases;
	for (const auto& pair : pairs)
	{
		SignalPhaseDef phase;
		for (const auto& conn : node->laneConnections)
		{
			for (const int eid : pair)
			{
				if (conn.fromEdgeId == eid)
				{
					phase.greenConnectionIds << conn.id;
					break;
				}
			}
		}

		if (phase.greenConnectionIds.isEmpty()) continue;

		phase.duration = Max(
			static_cast<float>(phase.greenConnectionIds.size()) * kDurationPerConnection,
			kMinPhaseDuration);
		phases << std::move(phase);
	}

	return phases;
}
