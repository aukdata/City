#include "DistrictRoads.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include <algorithm>
#include <random>

namespace DistrictRoads
{
	namespace
	{
		constexpr float kNodeMergeRadius = 25.0f;
		constexpr float kMaxSlope        = 0.10f; // 10%

		struct RoadNodeSpatialHash
		{
			static constexpr float kBucketSize = 200.0f;
			HashTable<int64, Array<int>> buckets;

			static int64 key(float x, float z)
			{
				const int bx = static_cast<int>(Math::Floor(x / kBucketSize));
				const int bz = static_cast<int>(Math::Floor(z / kBucketSize));
				return (static_cast<int64>(bx) << 32) | static_cast<uint32>(bz);
			}

			void insert(Vec3 pos, int nodeId)
			{
				buckets[key(static_cast<float>(pos.x), static_cast<float>(pos.z))] << nodeId;
			}

			int findNearest(Vec3 pos, const RoadNetwork& net, float maxDist) const
			{
				const float px = static_cast<float>(pos.x);
				const float pz = static_cast<float>(pos.z);
				const int range = static_cast<int>(Ceil(maxDist / kBucketSize));
				const int bx0 = static_cast<int>(Math::Floor(px / kBucketSize));
				const int bz0 = static_cast<int>(Math::Floor(pz / kBucketSize));

				float bestDistSq = maxDist * maxDist;
				int bestId = -1;

				for (int dz = -range; dz <= range; ++dz)
				{
					for (int dx = -range; dx <= range; ++dx)
					{
						const int64 k = (static_cast<int64>(bx0 + dx) << 32)
						              | static_cast<uint32>(bz0 + dz);
						const auto it = buckets.find(k);
						if (it == buckets.end()) continue;
						for (const int nid : it->second)
						{
							const RoadNode* node = net.getNode(nid);
							if (!node) continue;
							const float ddx = static_cast<float>(node->position.x) - px;
							const float ddz = static_cast<float>(node->position.z) - pz;
							const float dSq = ddx * ddx + ddz * ddz;
							if (dSq < bestDistSq)
							{
								bestDistSq = dSq;
								bestId = nid;
							}
						}
					}
				}
				return bestId;
			}
		};

		Vec2 safeNormalized(Vec2 v, Vec2 fallback = Vec2{ 1.0, 0.0 })
		{
			if (v.lengthSq() < 1e-6) return fallback;
			v.normalize();
			return v;
		}

		/// @brief Arterial / LocalRoad のみを街道候補エッジとみなす
		bool isKaidoEdge(const RoadNetwork& network, int edgeId)
		{
			const RoadEdge* e = network.getEdge(edgeId);
			if (!e || e->id < 0) return false;
			return e->roadType == RoadType::Arterial
				|| e->roadType == RoadType::LocalRoad;
		}

		bool tryAddLocalRoadEdge(RoadNetwork& network, int nodeIdA, int nodeIdB)
		{
			if (nodeIdA < 0 || nodeIdB < 0) return false;
			if (nodeIdA == nodeIdB) return false;

			const RoadNode* na = network.getNode(nodeIdA);
			const RoadNode* nb = network.getNode(nodeIdB);
			if (!na || !nb) return false;

			const float dh = static_cast<float>(Math::Abs(na->position.y - nb->position.y));
			const float dist = static_cast<float>((na->position - nb->position).length());
			if (dist < 1.0f) return false;
			if (dh / dist > kMaxSlope) return false;

			const Vec3 dir = (nb->position - na->position);
			const Vec3 ctrlA = na->position + dir * (1.0 / 3.0);
			const Vec3 ctrlB = na->position + dir * (2.0 / 3.0);
			return static_cast<bool>(network.addEdge(nodeIdA, nodeIdB, ctrlA, ctrlB,
			                                         RoadType::LocalRoad, 2));
		}

		int ensureNodeWithMerge(
			RoadNodeSpatialHash& nodeHash,
			const World& world,
			RoadNetwork& network,
			Vec2 xz,
			NodeType type = NodeType::Intersection)
		{
			const float y = world.computeHeight(static_cast<float>(xz.x), static_cast<float>(xz.y));
			if (y < 0.5f) return -1;
			const Vec3 pos{ xz.x, y, xz.y };

			const int existing = nodeHash.findNearest(pos, network, kNodeMergeRadius);
			if (existing >= 0) return existing;

			const int nid = network.addNode(pos, type);
			nodeHash.insert(pos, nid);
			return nid;
		}

		void rebuildStraightGeometry(int edgeId, const World& world, RoadNetwork& network)
		{
			RoadEdge* edge = network.getEdge(edgeId);
			if (!edge) return;

			const RoadNode* nodeA = network.getNode(edge->nodeA);
			const RoadNode* nodeB = network.getNode(edge->nodeB);
			if (!nodeA || !nodeB) return;

			const Vec3 dir = (nodeB->position - nodeA->position);
			edge->ctrlA = nodeA->position + dir * (1.0 / 3.0);
			edge->ctrlB = nodeA->position + dir * (2.0 / 3.0);

			if (const auto bez = network.getBezier(edgeId))
			{
				edge->length = bez->totalLength;
			}
			network.updateEdgeElevation(edgeId, world);
			network.rebuildNodeConnectivity(edge->nodeA, edge->nodeB);
		}

		bool insideCastleBlock(float lxA, float lzA, float lxB, float lzB)
		{
			const float midX = 0.5f * (lxA + lxB);
			const float midZ = 0.5f * (lzA + lzB);
			return (Math::Abs(midX) <= 30.0f && Math::Abs(midZ) <= 30.0f);
		}

		Array<float> buildCastleGridCoords(float halfExtent)
		{
			HashSet<int32> quantized;
			auto push = [&](float value)
			{
				quantized.insert(static_cast<int32>(Math::Round(value * 10.0f)));
			};

			push(-halfExtent);
			push(halfExtent);
			for (float value = -halfExtent; value <= halfExtent; value += 80.0f) push(value);
			for (float value = -150.0f; value <= 150.0f; value += 50.0f) push(value);

			Array<float> out;
			out.reserve(quantized.size());
			for (const int32 q : quantized) out << (q / 10.0f);
			out.sort();
			return out;
		}

		void pruneCastleGridEdges(uint64 localSeed, const Array<int>& gridEdgeIds, RoadNetwork& network)
		{
			if (gridEdgeIds.isEmpty()) return;

			std::mt19937_64 rng(localSeed ^ 0x5EED1234ULL);
			std::uniform_real_distribution<float> ratioDist(0.05f, 0.15f);
			Array<int> candidates = gridEdgeIds;
			std::shuffle(candidates.begin(), candidates.end(), rng);

			const int target = static_cast<int>(candidates.size() * ratioDist(rng));
			int removed = 0;
			for (const int edgeId : candidates)
			{
				if (removed >= target) break;
				const RoadEdge* edge = network.getEdge(edgeId);
				if (!edge) continue;

				const RoadNode* nodeA = network.getNode(edge->nodeA);
				const RoadNode* nodeB = network.getNode(edge->nodeB);
				if (!nodeA || !nodeB) continue;

				if (static_cast<int>(nodeA->attachments.size()) <= 2) continue;
				if (static_cast<int>(nodeB->attachments.size()) <= 2) continue;

				network.removeEdge(edgeId);
				++removed;
			}
		}

		int findNearestKaidoEdgeToCenter(const KaidoSegment& kaido, const Vec2& center, const RoadNetwork& network)
		{
			int bestEdge = -1;
			double bestDistSq = 1e300;

			for (const int edgeId : kaido.edgeIds)
			{
				const auto bez = network.getBezier(edgeId);
				if (!bez) continue;

				const Vec3 p = bez->positionAt(0.5f * bez->totalLength);
				const double dx = p.x - center.x;
				const double dz = p.z - center.y;
				const double dSq = dx * dx + dz * dz;
				if (dSq < bestDistSq)
				{
					bestDistSq = dSq;
					bestEdge = edgeId;
				}
			}
			return bestEdge;
		}

		void applyCastleKaidoCrank(
			uint64 seed,
			int settlementIndex,
			const MapGenerator::Settlement& settlement,
			const KaidoSegment& kaido,
			const World& world,
			RoadNetwork& network)
		{
			if (!kaido.passesThrough || kaido.edgeIds.isEmpty()) return;

			const int targetEdgeId = findNearestKaidoEdgeToCenter(kaido, settlement.center, network);
			if (targetEdgeId < 0) return;

			const RoadEdge* targetEdge = network.getEdge(targetEdgeId);
			if (!targetEdge) return;
			const RoadNode* nodeA = network.getNode(targetEdge->nodeA);
			const RoadNode* nodeB = network.getNode(targetEdge->nodeB);
			const auto bez = network.getBezier(targetEdgeId);
			if (!nodeA || !nodeB || !bez || bez->totalLength < 40.0f) return;

			Vec2 dirAB{
				static_cast<float>(nodeB->position.x - nodeA->position.x),
				static_cast<float>(nodeB->position.z - nodeA->position.z)
			};
			dirAB = safeNormalized(dirAB, Vec2{ 1.0, 0.0 });

			const int nodeN1 = network.splitEdgeAt(targetEdgeId, bez->totalLength * 0.35f);
			if (nodeN1 < 0) return;

			const RoadNode* n1 = network.getNode(nodeN1);
			if (!n1) return;

			int forwardEdgeId = -1;
			double bestDot = -2.0;
			for (const auto& att : n1->attachments)
			{
				const RoadEdge* e = network.getEdge(att.edgeId);
				if (!e || !isKaidoEdge(network, e->id)) continue;
				const int otherId = (e->nodeA == nodeN1) ? e->nodeB : e->nodeA;
				const RoadNode* other = network.getNode(otherId);
				if (!other) continue;

				Vec2 d{
					static_cast<float>(other->position.x - n1->position.x),
					static_cast<float>(other->position.z - n1->position.z)
				};
				d = safeNormalized(d, dirAB);
				const double dot = d.dot(dirAB);
				if (dot > bestDot)
				{
					bestDot = dot;
					forwardEdgeId = att.edgeId;
				}
			}
			if (forwardEdgeId < 0) return;

			const auto bez2 = network.getBezier(forwardEdgeId);
			if (!bez2 || bez2->totalLength < 20.0f) return;

			const int nodeN2 = network.splitEdgeAt(forwardEdgeId, bez2->totalLength * 0.55f);
			if (nodeN2 < 0) return;

			const Vec2 kaidoDir = safeNormalized(kaido.dirAtCenter, dirAB);
			Vec2 normal{ -kaidoDir.y, kaidoDir.x };
			normal = safeNormalized(normal, Vec2{ 0.0, 1.0 });

			std::mt19937_64 rng(seed ^ (0xC1A0D0ULL + static_cast<uint64>(settlementIndex) * 7919ULL));
			std::uniform_real_distribution<float> offDist(25.0f, 40.0f);
			const float offset = offDist(rng);

			auto moveNode = [&](int nodeId, float sign)
			{
				RoadNode* node = network.getNode(nodeId);
				if (!node) return;

				const float nx = static_cast<float>(node->position.x)
				               + static_cast<float>(normal.x) * offset * sign;
				const float nz = static_cast<float>(node->position.z)
				               + static_cast<float>(normal.y) * offset * sign;
				const float ny = world.computeHeight(nx, nz);
				if (ny < 0.5f) return;

				node->position.x = nx;
				node->position.z = nz;
				node->position.y = ny;
			};

			moveNode(nodeN1, +1.0f);
			moveNode(nodeN2, -1.0f);

			HashSet<int> affected;
			if (const RoadNode* node = network.getNode(nodeN1))
			{
				for (const auto& att : node->attachments) affected.insert(att.edgeId);
			}
			if (const RoadNode* node = network.getNode(nodeN2))
			{
				for (const auto& att : node->attachments) affected.insert(att.edgeId);
			}

			for (const int edgeId : affected)
			{
				rebuildStraightGeometry(edgeId, world, network);
			}
		}

		void generateLegacyGrid(
			uint64 seed, int settlementIndex,
			const MapGenerator::Settlement& settlement,
			float extent, float interval, float noiseAmp,
			const World& world,
			RoadNetwork& network)
		{
			RoadNodeSpatialHash nodeHash;
			for (const auto& n : network.nodes())
			{
				if (n.id < 0) continue;
				nodeHash.insert(n.position, n.id);
			}

			const float halfExt = extent * 0.5f;
			const float clipRadiusSq = (settlement.radius * 1.1f) * (settlement.radius * 1.1f);
			const uint64 localSeed = seed ^ (static_cast<uint64>(settlementIndex) * 2654435761ULL);
			const PerlinNoise noise{ localSeed };
			const float gridAngle = static_cast<float>((localSeed % 1000) / 1000.0 * Math::Pi);
			const float cosA = Math::Cos(gridAngle);
			const float sinA = Math::Sin(gridAngle);
			const float cx = static_cast<float>(settlement.center.x);
			const float cz = static_cast<float>(settlement.center.y);
			const int nLines = static_cast<int>(Ceil(extent / interval)) + 1;

			auto localToWorld = [&](float lx, float lz) -> Vec3
			{
				const float dx = static_cast<float>(
					(noise.noise2D(lx * 0.015, lz * 0.015 + 100.0) * 2.0 - 1.0) * noiseAmp);
				const float dz = static_cast<float>(
					(noise.noise2D(lx * 0.015 + 200.0, lz * 0.015) * 2.0 - 1.0) * noiseAmp);
				const float wx = cx + (lx + dx) * cosA - (lz + dz) * sinA;
				const float wz = cz + (lx + dx) * sinA + (lz + dz) * cosA;
				const float wy = world.computeHeight(wx, wz);
				return Vec3{ wx, wy, wz };
			};

			Grid<int> gridNodeIds(nLines, nLines, -1);
			for (int row = 0; row < nLines; ++row)
			{
				for (int col = 0; col < nLines; ++col)
				{
					const float lx = -halfExt + col * interval;
					const float lz = -halfExt + row * interval;
					const Vec3 pos = localToWorld(lx, lz);

					const float ddx = static_cast<float>(pos.x) - cx;
					const float ddz = static_cast<float>(pos.z) - cz;
					if (ddx * ddx + ddz * ddz > clipRadiusSq) continue;
					if (pos.y < 0.5f) continue;

					const int existing = nodeHash.findNearest(pos, network, kNodeMergeRadius);
					if (existing >= 0)
					{
						gridNodeIds[{ col, row }] = existing;
					}
					else
					{
						const int nid = network.addNode(pos, NodeType::Intersection);
						gridNodeIds[{ col, row }] = nid;
						nodeHash.insert(pos, nid);
					}
				}
			}

			for (int row = 0; row < nLines; ++row)
			{
				for (int col = 0; col + 1 < nLines; ++col)
				{
					tryAddLocalRoadEdge(network, gridNodeIds[{ col, row }], gridNodeIds[{ col + 1, row }]);
				}
			}

			for (int col = 0; col < nLines; ++col)
			{
				for (int row = 0; row + 1 < nLines; ++row)
				{
					tryAddLocalRoadEdge(network, gridNodeIds[{ col, row }], gridNodeIds[{ col, row + 1 }]);
				}
			}
		}

		/// @brief ノードがワールド XZ 平面で集落中心の searchRadius 以内か
		bool nodeInRange(const RoadNetwork& network, int nodeId,
		                 const Vec2& center, float searchRadius)
		{
			const RoadNode* n = network.getNode(nodeId);
			if (!n) return false;
			const double dx = n->position.x - center.x;
			const double dz = n->position.z - center.y;
			return dx * dx + dz * dz <= double(searchRadius) * double(searchRadius);
		}

		/// @brief prevNode → startEdge 経由で歩き続け、直進的に街道を辿ったエッジ列を返す
		void walkKaido(const RoadNetwork& network, int startNode, int startEdge,
		               const Vec2& center, float searchRadius,
		               Array<int>& outEdges, Array<int>& outNodes)
		{
			int prevNode = startNode;
			int currentEdge = startEdge;

			while (true)
			{
				const RoadEdge* e = network.getEdge(currentEdge);
				if (!e) break;
				const int nextNode = (e->nodeA == prevNode) ? e->nodeB : e->nodeA;

				outEdges << currentEdge;
				outNodes << nextNode;

				if (!nodeInRange(network, nextNode, center, searchRadius)) break;

				const RoadNode* nn = network.getNode(nextNode);
				const RoadNode* pn = network.getNode(prevNode);
				if (!nn || !pn) break;

				Vec2 curDir{ nn->position.x - pn->position.x,
				             nn->position.z - pn->position.z };
				if (curDir.lengthSq() < 1e-6) break;
				curDir.normalize();

				int bestNext = -1;
				double bestAlign = 0.3;
				for (const auto& att : nn->attachments)
				{
					if (att.edgeId == currentEdge) continue;
					if (!isKaidoEdge(network, att.edgeId)) continue;
					const RoadEdge* ne = network.getEdge(att.edgeId);
					if (!ne) continue;
					const int other = (ne->nodeA == nextNode) ? ne->nodeB : ne->nodeA;
					const RoadNode* on = network.getNode(other);
					if (!on) continue;

					Vec2 d{ on->position.x - nn->position.x,
					        on->position.z - nn->position.z };
					if (d.lengthSq() < 1e-6) continue;
					d.normalize();
					const double align = curDir.dot(d);
					if (align > bestAlign)
					{
						bestAlign = align;
						bestNext = att.edgeId;
					}
				}

				if (bestNext < 0) break;
				prevNode = nextNode;
				currentEdge = bestNext;
			}
		}
	} // namespace

	KaidoSegment extractKaido(
		const MapGenerator::Settlement& settlement,
		const RoadNetwork& network,
		float searchRadius)
	{
		KaidoSegment out;

		const Vec3 cp{ settlement.center.x, 0.0, settlement.center.y };
		const auto nearOpt = network.findNodeNear(cp, searchRadius);
		if (!nearOpt) return out;

		const int centerId = *nearOpt;
		out.centerNodeId = centerId;

		const RoadNode* centerNode = network.getNode(centerId);
		if (!centerNode) return out;

		Array<int> kaidoAtts;
		for (const auto& att : centerNode->attachments)
		{
			if (isKaidoEdge(network, att.edgeId)) kaidoAtts << att.edgeId;
		}
		if (kaidoAtts.isEmpty()) return out;

		int edgeA = kaidoAtts[0];
		int edgeB = -1;
		if (kaidoAtts.size() >= 2)
		{
			auto dirFromCenter = [&](int edgeId) -> Vec2
			{
				const RoadEdge* e = network.getEdge(edgeId);
				if (!e) return Vec2{ 0, 0 };
				const int other = (e->nodeA == centerId) ? e->nodeB : e->nodeA;
				const RoadNode* on = network.getNode(other);
				if (!on) return Vec2{ 0, 0 };
				Vec2 d{ on->position.x - centerNode->position.x,
				        on->position.z - centerNode->position.z };
				if (d.lengthSq() > 1e-6) d.normalize();
				return d;
			};

			double bestOpp = 1.0;
			for (size_t i = 0; i < kaidoAtts.size(); ++i)
			{
				for (size_t j = i + 1; j < kaidoAtts.size(); ++j)
				{
					const double opp = dirFromCenter(kaidoAtts[i]).dot(dirFromCenter(kaidoAtts[j]));
					if (opp < bestOpp)
					{
						bestOpp = opp;
						edgeA = kaidoAtts[i];
						edgeB = kaidoAtts[j];
					}
				}
			}
		}

		Array<int> aEdges, aNodes;
		walkKaido(network, centerId, edgeA, settlement.center, searchRadius, aEdges, aNodes);

		Array<int> bEdges, bNodes;
		if (edgeB >= 0)
		{
			walkKaido(network, centerId, edgeB, settlement.center, searchRadius, bEdges, bNodes);
		}

		out.edgeIds.reserve(aEdges.size() + bEdges.size());
		out.nodeIds.reserve(aEdges.size() + bEdges.size() + 1);

		for (int i = static_cast<int>(aEdges.size()) - 1; i >= 0; --i) out.edgeIds << aEdges[i];
		for (int i = static_cast<int>(aNodes.size()) - 1; i >= 0; --i) out.nodeIds << aNodes[i];
		out.nodeIds << centerId;
		for (size_t i = 0; i < bEdges.size(); ++i) out.edgeIds << bEdges[i];
		for (size_t i = 0; i < bNodes.size(); ++i) out.nodeIds << bNodes[i];

		int centerIdx = -1;
		for (int i = 0; i < static_cast<int>(out.nodeIds.size()); ++i)
		{
			if (out.nodeIds[i] == centerId) { centerIdx = i; break; }
		}
		const int lastIdx = static_cast<int>(out.nodeIds.size()) - 1;
		if (centerIdx >= 0 && lastIdx >= 1)
		{
			const int idxA = Max(0, centerIdx - 1);
			const int idxB = Min(lastIdx, centerIdx + 1);
			const RoadNode* a = network.getNode(out.nodeIds[idxA]);
			const RoadNode* b = network.getNode(out.nodeIds[idxB]);
			if (a && b)
			{
				Vec2 d{ b->position.x - a->position.x,
				        b->position.z - a->position.z };
				if (d.lengthSq() > 1e-6)
				{
					d.normalize();
					out.dirAtCenter = d;
				}
			}
		}

		out.passesThrough = (out.edgeIds.size() >= 1);
		return out;
	}

	void generateCastleTown(
		uint64 seed,
		int settlementIndex,
		const MapGenerator::Settlement& settlement,
		const KaidoSegment& kaido,
		const World& world,
		RoadNetwork& network)
	{
		if (!kaido.passesThrough || kaido.edgeIds.isEmpty())
		{
			generateLegacyGrid(seed, settlementIndex, settlement, 500.0f, 60.0f, 15.0f, world, network);
			return;
		}

		const uint64 localSeed = seed ^ (0xCA57A11ULL + static_cast<uint64>(settlementIndex) * 2654435761ULL);
		const PerlinNoise noise{ localSeed };

		Vec2 axisX = safeNormalized(kaido.dirAtCenter, Vec2{ 1.0, 0.0 });
		Vec2 axisZ{ -axisX.y, axisX.x };
		axisZ = safeNormalized(axisZ, Vec2{ 0.0, 1.0 });

		const float halfExtent = Max(260.0f, Min(420.0f, settlement.radius * 0.6f));
		const float clipRadiusSq = (settlement.radius * 1.1f) * (settlement.radius * 1.1f);
		const Array<float> coords = buildCastleGridCoords(halfExtent);
		const int n = static_cast<int>(coords.size());

		RoadNodeSpatialHash nodeHash;
		for (const auto& node : network.nodes())
		{
			if (node.id < 0) continue;
			nodeHash.insert(node.position, node.id);
		}

		Grid<int> nodeIds(n, n, -1);
		Array<int> gridEdgeIds;

		auto localToWorld = [&](float lx, float lz) -> Vec3
		{
			const float dx = static_cast<float>(
				(noise.noise2D(lx * 0.02, lz * 0.02 + 33.0) * 2.0 - 1.0) * 8.0f);
			const float dz = static_cast<float>(
				(noise.noise2D(lx * 0.02 + 88.0, lz * 0.02) * 2.0 - 1.0) * 8.0f);
			const Vec2 xz = settlement.center
				+ axisX * (lx + dx)
				+ axisZ * (lz + dz);
			const float y = world.computeHeight(static_cast<float>(xz.x), static_cast<float>(xz.y));
			return Vec3{ xz.x, y, xz.y };
		};

		for (int row = 0; row < n; ++row)
		{
			for (int col = 0; col < n; ++col)
			{
				const float lx = coords[col];
				const float lz = coords[row];
				const Vec3 pos = localToWorld(lx, lz);

				const float dx = static_cast<float>(pos.x - settlement.center.x);
				const float dz = static_cast<float>(pos.z - settlement.center.y);
				if (dx * dx + dz * dz > clipRadiusSq) continue;
				if (pos.y < 0.5f) continue;

				const int existing = nodeHash.findNearest(pos, network, kNodeMergeRadius);
				if (existing >= 0)
				{
					nodeIds[{ col, row }] = existing;
				}
				else
				{
					const int nid = network.addNode(pos, NodeType::Intersection);
					nodeIds[{ col, row }] = nid;
					nodeHash.insert(pos, nid);
				}
			}
		}

		auto tryAddGridEdge = [&](int colA, int rowA, int colB, int rowB)
		{
			if (insideCastleBlock(coords[colA], coords[rowA], coords[colB], coords[rowB])) return;
			const int nodeA = nodeIds[{ colA, rowA }];
			const int nodeB = nodeIds[{ colB, rowB }];
			if (!tryAddLocalRoadEdge(network, nodeA, nodeB)) return;

			const RoadNode* na = network.getNode(nodeA);
			if (!na) return;
			for (const auto& att : na->attachments)
			{
				const RoadEdge* e = network.getEdge(att.edgeId);
				if (!e) continue;
				if ((e->nodeA == nodeA && e->nodeB == nodeB) ||
				    (e->nodeA == nodeB && e->nodeB == nodeA))
				{
					gridEdgeIds << e->id;
					break;
				}
			}
		};

		for (int row = 0; row < n; ++row)
		{
			for (int col = 0; col + 1 < n; ++col)
			{
				tryAddGridEdge(col, row, col + 1, row);
			}
		}
		for (int col = 0; col < n; ++col)
		{
			for (int row = 0; row + 1 < n; ++row)
			{
				tryAddGridEdge(col, row, col, row + 1);
			}
		}

		pruneCastleGridEdges(localSeed, gridEdgeIds, network);
		applyCastleKaidoCrank(seed, settlementIndex, settlement, kaido, world, network);
	}

	void generatePostTown(
		uint64 seed,
		int settlementIndex,
		const MapGenerator::Settlement& settlement,
		const KaidoSegment& kaido,
		const World& world,
		RoadNetwork& network)
	{
		if (!kaido.passesThrough || kaido.nodeIds.size() < 3)
		{
			generateLegacyGrid(seed, settlementIndex, settlement, 200.0f, 70.0f, 12.0f, world, network);
			return;
		}

		RoadNodeSpatialHash nodeHash;
		for (const auto& node : network.nodes())
		{
			if (node.id < 0) continue;
			nodeHash.insert(node.position, node.id);
		}

		std::mt19937_64 rng(seed ^ (0xB057A11ULL + static_cast<uint64>(settlementIndex) * 6364136223846793005ULL));
		std::uniform_int_distribution<int> rungCountDist(3, 5);
		std::uniform_int_distribution<int> sideCountDist(1, 2);
		std::uniform_real_distribution<float> offsetDist(40.0f, 60.0f);
		std::uniform_real_distribution<float> alongDist(-10.0f, 10.0f);

		Vec2 kaidoDir = safeNormalized(kaido.dirAtCenter, Vec2{ 1.0, 0.0 });
		Vec2 normal{ -kaidoDir.y, kaidoDir.x };
		normal = safeNormalized(normal, Vec2{ 0.0, 1.0 });

		const int desiredRungs = rungCountDist(rng);
		Array<std::pair<double, int>> rungAnchors;

		const int usableInternal = Max(0, static_cast<int>(kaido.nodeIds.size()) - 2);
		const int fromNodes = Min(desiredRungs, usableInternal);
		for (int i = 1; i <= fromNodes; ++i)
		{
			const int idx = (i * (static_cast<int>(kaido.nodeIds.size()) - 1)) / (fromNodes + 1);
			rungAnchors << std::make_pair(static_cast<double>(idx), kaido.nodeIds[idx]);
		}

		if (rungAnchors.size() < 3)
		{
			for (int i = 0; i < static_cast<int>(kaido.edgeIds.size()) && rungAnchors.size() < 3; ++i)
			{
				const auto bez = network.getBezier(kaido.edgeIds[i]);
				if (!bez || bez->totalLength < 30.0f) continue;
				const int nid = network.splitEdgeAt(kaido.edgeIds[i], 0.5f * bez->totalLength);
				if (nid >= 0) rungAnchors << std::make_pair(i + 0.5, nid);
			}
		}
		if (rungAnchors.size() < 2)
		{
			generateLegacyGrid(seed, settlementIndex, settlement, 200.0f, 70.0f, 12.0f, world, network);
			return;
		}

		rungAnchors.sort_by([](const auto& a, const auto& b) { return a.first < b.first; });

		Array<int> sides;
		if (sideCountDist(rng) == 1)
		{
			sides << ((rng() & 1ULL) ? +1 : -1);
		}
		else
		{
			sides << -1;
			sides << +1;
		}

		const int kaidoStartId = kaido.nodeIds.front();
		const int kaidoEndId = kaido.nodeIds.back();

		for (const int side : sides)
		{
			Array<int> backNodes;
			backNodes.reserve(rungAnchors.size());

			for (const auto& anchor : rungAnchors)
			{
				const int anchorId = anchor.second;
				const RoadNode* anchorNode = network.getNode(anchorId);
				if (!anchorNode)
				{
					backNodes << -1;
					continue;
				}

				const float offset = offsetDist(rng) * static_cast<float>(side);
				const float along = alongDist(rng);
				const Vec2 xz{
					static_cast<float>(anchorNode->position.x) + normal.x * offset + kaidoDir.x * along,
					static_cast<float>(anchorNode->position.z) + normal.y * offset + kaidoDir.y * along
				};

				const int backNodeId = ensureNodeWithMerge(nodeHash, world, network, xz, NodeType::Joint);
				backNodes << backNodeId;
				if (backNodeId >= 0)
				{
					tryAddLocalRoadEdge(network, anchorId, backNodeId);
				}
			}

			for (int i = 0; i + 1 < static_cast<int>(backNodes.size()); ++i)
			{
				if (backNodes[i] < 0 || backNodes[i + 1] < 0) continue;
				tryAddLocalRoadEdge(network, backNodes[i], backNodes[i + 1]);
			}

			int first = -1;
			int last = -1;
			for (const int nodeId : backNodes)
			{
				if (nodeId < 0) continue;
				if (first < 0) first = nodeId;
				last = nodeId;
			}

			if (first >= 0 && last >= 0)
			{
				tryAddLocalRoadEdge(network, kaidoStartId, first);
				tryAddLocalRoadEdge(network, last, kaidoEndId);
			}
		}
	}

	void generateVillage(
		uint64 seed,
		int settlementIndex,
		const MapGenerator::Settlement& settlement,
		const KaidoSegment& kaido,
		const World& world,
		RoadNetwork& network)
	{
		if (!kaido.passesThrough || kaido.edgeIds.isEmpty())
		{
			generateLegacyGrid(seed, settlementIndex, settlement, 100.0f, 80.0f, 8.0f, world, network);
			return;
		}

		std::mt19937_64 rng(seed ^ (0x9E3779B97F4A7C15ULL + static_cast<uint64>(settlementIndex)));
		std::uniform_int_distribution<int> branchCountDist(1, 2);
		std::uniform_real_distribution<float> branchLenDist(60.0f, 120.0f);
		std::uniform_real_distribution<float> jitterDist(-0.35f, 0.35f);
		std::uniform_int_distribution<int> sideDist(0, 1);

		Array<int> candidates;
		const int center = static_cast<int>(kaido.edgeIds.size()) / 2;
		for (int d = 0; d <= 2; ++d)
		{
			const int i1 = center - d;
			const int i2 = center + d;
			if (0 <= i1 && i1 < static_cast<int>(kaido.edgeIds.size())) candidates << i1;
			if (d != 0 && 0 <= i2 && i2 < static_cast<int>(kaido.edgeIds.size())) candidates << i2;
		}
		if (candidates.isEmpty()) return;

		std::shuffle(candidates.begin(), candidates.end(), rng);
		const int targetBranches = Min(branchCountDist(rng), static_cast<int>(candidates.size()));

		Vec2 kaidoDir = kaido.dirAtCenter;
		if (kaidoDir.lengthSq() < 1e-6) kaidoDir = Vec2{ 1.0f, 0.0f };
		else kaidoDir.normalize();

		int added = 0;
		for (int pick = 0; pick < targetBranches; ++pick)
		{
			const int edgeId = kaido.edgeIds[candidates[pick]];
			const RoadEdge* edge = network.getEdge(edgeId);
			if (!edge || edge->id < 0) continue;

			const auto bezOpt = network.getBezier(edgeId);
			if (!bezOpt || bezOpt->totalLength < 20.0f) continue;

			const float splitArc = bezOpt->totalLength * 0.5f;
			const int branchNodeId = network.splitEdgeAt(edgeId, splitArc);
			if (branchNodeId < 0) continue;

			const RoadNode* branchNode = network.getNode(branchNodeId);
			if (!branchNode) continue;

			const float sign = (sideDist(rng) == 0) ? -1.0f : 1.0f;
			const float angle = jitterDist(rng);
			Vec2 baseNormal{ -kaidoDir.y, kaidoDir.x };
			if (baseNormal.lengthSq() < 1e-6) continue;
			baseNormal.normalize();
			Vec2 branchDir{
				baseNormal.x * Math::Cos(angle) - baseNormal.y * Math::Sin(angle),
				baseNormal.x * Math::Sin(angle) + baseNormal.y * Math::Cos(angle)
			};
			branchDir *= sign;
			branchDir.normalize();

			const float branchLen = branchLenDist(rng);
			const float ex = static_cast<float>(branchNode->position.x)
			               + static_cast<float>(branchDir.x) * branchLen;
			const float ez = static_cast<float>(branchNode->position.z)
			               + static_cast<float>(branchDir.y) * branchLen;
			const float ey = world.computeHeight(ex, ez);
			if (ey < 0.5f) continue;

			const float startY = static_cast<float>(branchNode->position.y);
			const float slope = Math::Abs(ey - startY) / Max(1.0f, branchLen);
			if (slope > kMaxSlope) continue;

			const int endNodeId = network.addNode(Vec3{ ex, ey, ez }, NodeType::Endpoint);
			if (!tryAddLocalRoadEdge(network, branchNodeId, endNodeId))
			{
				network.removeNode(endNodeId);
				continue;
			}
			++added;
		}

		if (added == 0)
		{
			generateLegacyGrid(seed, settlementIndex, settlement, 100.0f, 80.0f, 8.0f, world, network);
		}
	}
}
