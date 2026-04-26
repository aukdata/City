#include "DistrictRoads.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <random>

namespace DistrictRoads
{
	namespace
	{
		constexpr float kNodeMergeRadius = 25.0f;
		constexpr float kMaxSlope        = 0.10f; // 10%
		constexpr float kCastleHalfMin   = 1000.0f; // 一辺 2km
		constexpr float kCastleHalfMax   = 4000.0f; // 一辺 8km

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
			// 地区内道路として成立する勾配だけを通し、直線ベースの生活道路エッジを追加する。
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
			// 新規候補点は既存近傍ノードへ吸着し、十分離れているときだけ新設する。
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
			// 既存エッジを直線基準の制御点へ戻し、長さ・高低差・接続情報をまとめて更新する。
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
			// 中央城郭は最低 500m 四方を確保（中心 ±250m）
			return (Math::Abs(midX) <= 250.0f && Math::Abs(midZ) <= 250.0f);
		}

		Array<float> buildCastleGridCoords(float halfExtent)
		{
			// 城下町グリッドの分割数を、街区幅制約を守りつつ 300m 近傍になるよう選ぶ。
			Array<float> out;
			const float fullExtent = halfExtent * 2.0f;

			// セル幅制約 [200m, 500m] を満たす分割数を探索し、300m 近傍を優先する
			const int minCells = Max(1, static_cast<int>(Ceil(fullExtent / 500.0f)));
			const int maxCells = Max(minCells, static_cast<int>(Floor(fullExtent / 200.0f)));

			int bestCells = minCells;
			float bestScore = 1e30f;
			for (int cells = minCells; cells <= maxCells; ++cells)
			{
				const float cellW = fullExtent / cells;
				const float score = Math::Abs(cellW - 300.0f);
				if (score < bestScore)
				{
					bestScore = score;
					bestCells = cells;
				}
			}

			const float cellW = fullExtent / bestCells;
			out.reserve(bestCells + 1);
			for (int i = 0; i <= bestCells; ++i)
			{
				out << (-halfExtent + cellW * i);
			}
			return out;
		}

		int findEdgeBetweenNodes(const RoadNetwork& network, int nodeIdA, int nodeIdB)
		{
			if (nodeIdA < 0 || nodeIdB < 0) return -1;
			const RoadNode* nodeA = network.getNode(nodeIdA);
			if (!nodeA) return -1;
			for (const auto& att : nodeA->attachments)
			{
				const RoadEdge* e = network.getEdge(att.edgeId);
				if (!e || e->id < 0) continue;
				if ((e->nodeA == nodeIdA && e->nodeB == nodeIdB)
				 || (e->nodeA == nodeIdB && e->nodeB == nodeIdA))
				{
					return e->id;
				}
			}
			return -1;
		}

		double distanceXZ(const RoadNode* node, const Vec2& center)
		{
			if (!node) return 1e300;
			const double dx = node->position.x - center.x;
			const double dz = node->position.z - center.y;
			return std::sqrt(dx * dx + dz * dz);
		}

		Array<int> collectArterialNodesAroundRadius(
			const Vec2& center,
			float targetRadius,
			float tolerance,
			const RoadNetwork& network)
		{
			HashSet<int> nodeSet;
			for (const auto& edge : network.edges())
			{
				if (edge.id < 0 || edge.roadType != RoadType::Arterial) continue;
				const int ends[2] = { edge.nodeA, edge.nodeB };
				for (const int nodeId : ends)
				{
					const RoadNode* node = network.getNode(nodeId);
					if (!node) continue;
					const double d = distanceXZ(node, center);
					if (Math::Abs(d - targetRadius) <= tolerance)
					{
						nodeSet.insert(nodeId);
					}
				}
			}

			// リング帯で足りないときは半径内の幹線ノードへフォールバック
			if (nodeSet.size() < 2)
			{
				for (const auto& edge : network.edges())
				{
					if (edge.id < 0 || edge.roadType != RoadType::Arterial) continue;
					const int ends[2] = { edge.nodeA, edge.nodeB };
					for (const int nodeId : ends)
					{
						const RoadNode* node = network.getNode(nodeId);
						if (!node) continue;
						if (distanceXZ(node, center) <= targetRadius)
						{
							nodeSet.insert(nodeId);
						}
					}
				}
			}

			Array<int> out;
			out.reserve(nodeSet.size());
			for (const int nodeId : nodeSet) out << nodeId;
			return out;
		}

		void removeInnerArterials(const Vec2& center, float radius, RoadNetwork& network)
		{
			Array<int> removeIds;
			for (const auto& edge : network.edges())
			{
				if (edge.id < 0 || edge.roadType != RoadType::Arterial) continue;
				const RoadNode* nodeA = network.getNode(edge.nodeA);
				const RoadNode* nodeB = network.getNode(edge.nodeB);
				if (!nodeA || !nodeB) continue;

				const double da = distanceXZ(nodeA, center);
				const double db = distanceXZ(nodeB, center);
				if (da < radius && db < radius)
				{
					removeIds << edge.id;
				}
			}

			for (const int edgeId : removeIds)
			{
				network.removeEdge(edgeId);
			}
		}

		Optional<std::pair<int, int>> pickMostOppositeNodes(const Array<int>& nodeIds, const Vec2& center, const RoadNetwork& network)
		{
			if (nodeIds.size() < 2) return none;

			std::pair<int, int> best = { -1, -1 };
			double bestDot = 1e300;
			for (size_t i = 0; i < nodeIds.size(); ++i)
			{
				const RoadNode* a = network.getNode(nodeIds[i]);
				if (!a) continue;
				Vec2 va{ static_cast<float>(a->position.x - center.x), static_cast<float>(a->position.z - center.y) };
				va = safeNormalized(va, Vec2{ 1.0f, 0.0f });

				for (size_t j = i + 1; j < nodeIds.size(); ++j)
				{
					const RoadNode* b = network.getNode(nodeIds[j]);
					if (!b) continue;
					Vec2 vb{ static_cast<float>(b->position.x - center.x), static_cast<float>(b->position.z - center.y) };
					vb = safeNormalized(vb, Vec2{ 1.0f, 0.0f });

					const double dot = va.dot(vb);
					if (dot < bestDot)
					{
						bestDot = dot;
						best = { nodeIds[i], nodeIds[j] };
					}
				}
			}

			if (best.first < 0 || best.second < 0) return none;
			return best;
		}

		float castleHalfExtent(const MapGenerator::Settlement& settlement)
		{
			return Max(kCastleHalfMin, Min(kCastleHalfMax, settlement.radius * 0.6f));
		}

		bool findNearestArterialDirection(
			const Vec2& center,
			const RoadNetwork& network,
			Vec2& outDir)
		{
			double bestDistSq = 1e300;
			Vec2 bestDir{ 1.0f, 0.0f };

			for (const auto& edge : network.edges())
			{
				if (edge.id < 0 || edge.roadType != RoadType::Arterial) continue;
				const RoadNode* nodeA = network.getNode(edge.nodeA);
				const RoadNode* nodeB = network.getNode(edge.nodeB);
				if (!nodeA || !nodeB) continue;

				const Vec2 mid{
					static_cast<float>((nodeA->position.x + nodeB->position.x) * 0.5),
					static_cast<float>((nodeA->position.z + nodeB->position.z) * 0.5)
				};
				const double dx = mid.x - center.x;
				const double dz = mid.y - center.y;
				const double dSq = dx * dx + dz * dz;
				if (dSq >= bestDistSq) continue;

				Vec2 dir{
					static_cast<float>(nodeB->position.x - nodeA->position.x),
					static_cast<float>(nodeB->position.z - nodeA->position.z)
				};
				if (dir.lengthSq() < 1e-8) continue;
				bestDistSq = dSq;
				bestDir = safeNormalized(dir, Vec2{ 1.0f, 0.0f });
			}

			if (bestDistSq >= 1e299) return false;
			outDir = bestDir;
			return true;
		}

		bool computeArterialAxisTangent(
			const Vec2& center,
			float searchRadius,
			const RoadNetwork& network,
			Vec2& outAxisX)
		{
			double bestDistSq = 1e300;
			Vec2 bestAxis{ 1.0f, 0.0f };
			const double radiusSq = static_cast<double>(searchRadius) * searchRadius;

			for (const auto& edge : network.edges())
			{
				if (edge.id < 0 || edge.roadType != RoadType::Arterial) continue;
				const auto bez = network.getBezier(edge.id);
				if (!bez || bez->totalLength <= 1.0f) continue;

				constexpr int kSamples = 16;
				for (int i = 0; i <= kSamples; ++i)
				{
					const float s = bez->totalLength * (static_cast<float>(i) / kSamples);
					const Vec3 p = bez->positionAt(s);
					const double dx = p.x - center.x;
					const double dz = p.z - center.y;
					const double dSq = dx * dx + dz * dz;
					if (dSq > radiusSq || dSq >= bestDistSq) continue;

					Vec2 tan{ bez->tangentAt(s).x, bez->tangentAt(s).z };
					if (tan.lengthSq() < 1e-8) continue;
					tan.normalize();

					bestDistSq = dSq;
					bestAxis = tan;
				}
			}

			if (bestDistSq >= 1e299) return false;
			outAxisX = bestAxis;
			return true;
		}

		void computeCastleGridAxes(
			const Vec2& center,
			float searchRadius,
			const Array<int>& arterialNodes,
			const RoadNetwork& network,
			Vec2& outAxisX, Vec2& outAxisZ)
		{
			// 城下町グリッドの向きは、近傍幹線の接線を優先し、取れなければノード配置から推定する。
			Vec2 axisX{ 1.0f, 0.0f };

			// 第一候補: 幹線（Arterial）上の中心最寄り点での接線方向
			if (computeArterialAxisTangent(center, searchRadius, network, axisX))
			{
				// keep
			}
			// 第二候補: ノード2点（中心を挟んで最反対）から推定
			else if (const auto axisPair = pickMostOppositeNodes(arterialNodes, center, network))
			{
				const RoadNode* axisNodeA = network.getNode(axisPair->first);
				const RoadNode* axisNodeB = network.getNode(axisPair->second);
				if (axisNodeA && axisNodeB)
				{
					axisX = safeNormalized(
						Vec2{
							static_cast<float>(axisNodeB->position.x - axisNodeA->position.x),
							static_cast<float>(axisNodeB->position.z - axisNodeA->position.z)
						},
							Vec2{ 1.0f, 0.0f });
				}
			}
			// 第三候補: 単独幹線ノードに接続する幹線エッジ方向
			else if (!arterialNodes.isEmpty())
			{
				const int nid = arterialNodes.front();
				const RoadNode* node = network.getNode(nid);
				if (node)
				{
					for (const auto& att : node->attachments)
					{
						const RoadEdge* edge = network.getEdge(att.edgeId);
						if (!edge || edge->roadType != RoadType::Arterial) continue;
						const int otherId = (edge->nodeA == nid) ? edge->nodeB : edge->nodeA;
						const RoadNode* other = network.getNode(otherId);
						if (!other) continue;
						axisX = safeNormalized(
							Vec2{
								static_cast<float>(other->position.x - node->position.x),
								static_cast<float>(other->position.z - node->position.z)
							},
							Vec2{ 1.0f, 0.0f });
						break;
					}
				}
			}
			// 最終候補: 全体で最寄りの幹線エッジ方向
			else
			{
				Vec2 nearestDir;
				if (findNearestArterialDirection(center, network, nearestDir))
				{
					axisX = nearestDir;
				}
			}

			axisX = safeNormalized(axisX, Vec2{ 1.0f, 0.0f });

			Vec2 axisZ{ -axisX.y, axisX.x };
			axisZ = safeNormalized(axisZ, Vec2{ 0.0f, 1.0f });

			outAxisX = axisX;
			outAxisZ = axisZ;
		}

		bool addArterialWithPathfinder(
			const World& world,
			RoadNetwork& network,
			int startNodeId,
			int endNodeId,
			Array<int>* outEdgeIds = nullptr)
		{
			if (startNodeId < 0 || endNodeId < 0 || startNodeId == endNodeId) return false;
			const RoadNode* startNode = network.getNode(startNodeId);
			const RoadNode* endNode   = network.getNode(endNodeId);
			if (!startNode || !endNode) return false;

			const float sx = static_cast<float>(startNode->position.x);
			const float sz = static_cast<float>(startNode->position.z);
			const float ex = static_cast<float>(endNode->position.x);
			const float ez = static_cast<float>(endNode->position.z);

			const float dist = std::sqrt((ex - sx) * (ex - sx) + (ez - sz) * (ez - sz));
			const float cellSize = (dist > 5000.0f) ? 120.0f
			                     : (dist > 2000.0f) ? 80.0f
			                     : RoadPathfinder::kDefaultCellSize;
			const float margin = 300.0f;
			const float minX = Min(sx, ex) - margin;
			const float minZ = Min(sz, ez) - margin;
			const float maxX = Max(sx, ex) + margin;
			const float maxZ = Max(sz, ez) + margin;

			const int pfW = Max(2, static_cast<int>(Ceil((maxX - minX) / cellSize)));
			const int pfH = Max(2, static_cast<int>(Ceil((maxZ - minZ) / cellSize)));

			RoadPathfinder pf;
			pf.setup(world, Vec2{ minX, minZ }, pfW, pfH, cellSize);

			const Point gs = pf.worldToGrid(sx, sz);
			const Point ge = pf.worldToGrid(ex, ez);
			const Array<Point> path = pf.findPath(gs, ge);

			if (path.isEmpty() || path.size() < 2)
			{
				if (auto eid = network.addEdge(startNodeId, endNodeId,
				                               startNode->position + (endNode->position - startNode->position) * (1.0 / 3.0),
				                               startNode->position + (endNode->position - startNode->position) * (2.0 / 3.0),
				                               RoadType::Arterial, 4))
				{
					if (outEdgeIds) *outEdgeIds << *eid;
					return true;
				}
				return false;
			}

			const int sampleStep = (cellSize > 60.0f) ? 2 : 3;
			Array<Vec3> wps = pf.samplePath(path, sampleStep);
			wps.front() = startNode->position;
			wps.back()  = endNode->position;
			const int before = static_cast<int>(network.edges().size());
			pf.pathToRoadEdges(wps, network, RoadType::Arterial, 4, startNodeId, endNodeId, outEdgeIds);
			return static_cast<int>(network.edges().size()) > before;
		}

		void reattachRoutesFromNeighbors(RoadNetwork& network, int edgeId)
		{
			RoadEdge* edge = network.getEdge(edgeId);
			if (!edge || edge->id < 0) return;

			HashTable<int, int> routeCounts;
			const int ends[2] = { edge->nodeA, edge->nodeB };
			for (const int nodeId : ends)
			{
				const RoadNode* node = network.getNode(nodeId);
				if (!node) continue;
				for (const auto& att : node->attachments)
				{
					if (att.edgeId == edgeId) continue;
					const RoadEdge* nbr = network.getEdge(att.edgeId);
					if (!nbr || nbr->id < 0) continue;
					for (const int rid : nbr->routeIds)
					{
						++routeCounts[rid];
					}
				}
			}
			if (routeCounts.empty()) return;

			int bestCount = 0;
			for (const auto& [rid, count] : routeCounts) bestCount = Max(bestCount, count);

			for (const auto& [rid, count] : routeCounts)
			{
				if (count < bestCount) continue;
				if (!edge->routeIds.contains(rid))
				{
					edge->routeIds << rid;
				}
				if (RoadRoute* route = network.getRoute(rid))
				{
					if (!route->edgeIds.contains(edgeId))
					{
						route->edgeIds << edgeId;
					}
				}
			}
		}

		struct GridAdjEdge
		{
			int   toNodeId = -1;
			int   edgeId   = -1;
			float length   = 0.0f;
		};

		Array<int> findGridPathTurnPenalty(
			const RoadNetwork& network,
			int startNodeId,
			int goalNodeId,
			const HashTable<int, Array<GridAdjEdge>>& graph,
			float turnPenalty)
		{
			if (startNodeId < 0 || goalNodeId < 0 || startNodeId == goalNodeId) return {};
			if (!graph.contains(startNodeId) || !graph.contains(goalNodeId)) return {};

			auto encode = [](int prev, int cur) -> int64
			{
				return (static_cast<int64>(prev + 2) << 32) | static_cast<uint32>(cur);
			};
			auto decodePrev = [](int64 k) -> int
			{
				return static_cast<int>((k >> 32) - 2);
			};
			auto decodeCur = [](int64 k) -> int
			{
				return static_cast<int>(static_cast<uint32>(k));
			};

			struct ParentInfo
			{
				int64 parentKey = -1;
				int edgeId = -1;
			};

			using QueueItem = std::pair<double, int64>;
			std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;
			HashTable<int64, double> dist;
			HashTable<int64, ParentInfo> parent;

			const int64 startKey = encode(-1, startNodeId);
			dist[startKey] = 0.0;
			parent[startKey] = ParentInfo{};
			pq.push({ 0.0, startKey });

			int64 bestGoalKey = -1;
			double bestGoalCost = 1e300;

			while (!pq.empty())
			{
				const auto [cost, stateKey] = pq.top();
				pq.pop();
				const auto itDist = dist.find(stateKey);
				if (itDist == dist.end() || cost > itDist->second + 1e-9) continue;
				if (cost >= bestGoalCost) continue;

				const int prevNodeId = decodePrev(stateKey);
				const int curNodeId = decodeCur(stateKey);
				if (curNodeId == goalNodeId)
				{
					bestGoalCost = cost;
					bestGoalKey = stateKey;
					continue;
				}

				const auto itAdj = graph.find(curNodeId);
				if (itAdj == graph.end()) continue;

				const RoadNode* prevNode = (prevNodeId >= 0) ? network.getNode(prevNodeId) : nullptr;
				const RoadNode* curNode = network.getNode(curNodeId);
				if (!curNode) continue;

				for (const auto& adj : itAdj->second)
				{
					const RoadNode* nextNode = network.getNode(adj.toNodeId);
					if (!nextNode) continue;

					double penalty = 0.0;
					if (prevNode)
					{
						Vec2 dirIn{
							static_cast<float>(curNode->position.x - prevNode->position.x),
							static_cast<float>(curNode->position.z - prevNode->position.z)
						};
						Vec2 dirOut{
							static_cast<float>(nextNode->position.x - curNode->position.x),
							static_cast<float>(nextNode->position.z - curNode->position.z)
						};
						dirIn = safeNormalized(dirIn, Vec2{ 1.0f, 0.0f });
						dirOut = safeNormalized(dirOut, Vec2{ 1.0f, 0.0f });
						const double dot = dirIn.dot(dirOut);
						if (dot < 0.999) penalty += turnPenalty;
						if (dot < -0.5)  penalty += turnPenalty; // Uターン抑制
					}

					const double nextCost = cost + adj.length + penalty;
					const int64 nextKey = encode(curNodeId, adj.toNodeId);
					const auto itNext = dist.find(nextKey);
					if (itNext == dist.end() || nextCost < itNext->second)
					{
						dist[nextKey] = nextCost;
						parent[nextKey] = ParentInfo{ stateKey, adj.edgeId };
						pq.push({ nextCost, nextKey });
					}
				}
			}

			if (bestGoalKey < 0) return {};

			Array<int> edgePath;
			for (int64 curKey = bestGoalKey; curKey != startKey; )
			{
				const auto itParent = parent.find(curKey);
				if (itParent == parent.end() || itParent->second.parentKey < 0 || itParent->second.edgeId < 0)
				{
					edgePath.clear();
					break;
				}
				edgePath << itParent->second.edgeId;
				curKey = itParent->second.parentKey;
			}
			edgePath.reverse();
			return edgePath;
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

		/// @brief 街道の始端→終端を結ぶ全体方向を返す（クランク後も変化しない）
		Vec2 computeKaidoAxisOverall(const KaidoSegment& kaido, const RoadNetwork& network)
		{
			if (kaido.nodeIds.size() >= 2)
			{
				const RoadNode* first = network.getNode(kaido.nodeIds.front());
				const RoadNode* last  = network.getNode(kaido.nodeIds.back());
				if (first && last)
				{
					Vec2 d{ static_cast<float>(last->position.x - first->position.x),
					        static_cast<float>(last->position.z - first->position.z) };
					if (d.lengthSq() > 1e-6) return safeNormalized(d, Vec2{ 1.0f, 0.0f });
				}
			}
			return safeNormalized(kaido.dirAtCenter, Vec2{ 1.0f, 0.0f });
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

			const Vec2 kaidoDir = computeKaidoAxisOverall(kaido, network);
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

		Vec2 computeKaidoAxisFromCenter(
			const KaidoSegment& kaido,
			const Vec2& center,
			const RoadNetwork& network)
		{
			Vec2 bestDir = safeNormalized(kaido.dirAtCenter, Vec2{ 1.0, 0.0 });
			double bestDistSq = 1e300;

			for (const int edgeId : kaido.edgeIds)
			{
				const auto bez = network.getBezier(edgeId);
				if (!bez || bez->totalLength <= 1.0f) continue;

				constexpr int kSamples = 12;
				for (int i = 0; i <= kSamples; ++i)
				{
					const float s = bez->totalLength * (static_cast<float>(i) / kSamples);
					const Vec3 p = bez->positionAt(s);
					Vec2 tan{ bez->tangentAt(s).x, bez->tangentAt(s).z };
					if (tan.lengthSq() < 1e-8) continue;
					tan.normalize();

					const double dx = p.x - center.x;
					const double dz = p.z - center.y;
					const double dSq = dx * dx + dz * dz;
					if (dSq < bestDistSq)
					{
						bestDistSq = dSq;
						bestDir = tan;
					}
				}
			}

			return safeNormalized(bestDir, Vec2{ 1.0, 0.0 });
		}

		struct CastleTownFrame
		{
			Vec2  center;
			Vec2  axisX;
			Vec2  axisZ;
			float halfExtent = 0.0f;
			float margin     = 0.0f;
		};

		Array<CastleTownFrame> buildCastleTownFrames(
			const Array<MapGenerator::Settlement>& settlements,
			[[maybe_unused]] const RoadNetwork& network)
		{
			Array<CastleTownFrame> frames;
			frames.reserve(settlements.size());

			for (const auto& settlement : settlements)
			{
				if (settlement.kind != MapGenerator::SettlementKind::CastleTown) continue;

				const float halfExtent = castleHalfExtent(settlement);
				const Vec2 axisX = safeNormalized(settlement.gridAxisX, Vec2{ 1.0f, 0.0f });
				const Vec2 axisZ = safeNormalized(settlement.gridAxisZ, Vec2{ 0.0f, 1.0f });
				frames << CastleTownFrame{
					settlement.center,
					axisX,
					axisZ,
					halfExtent,
					80.0f
				};
			}

			return frames;
		}

		Vec2 toFrameLocal(const Vec2& p, const CastleTownFrame& frame)
		{
			const Vec2 d = p - frame.center;
			return Vec2{ d.dot(frame.axisX), d.dot(frame.axisZ) };
		}

		bool segmentIntersectsFrame(const Vec2& aWorld, const Vec2& bWorld, const CastleTownFrame& frame)
		{
			const Vec2 a = toFrameLocal(aWorld, frame);
			const Vec2 b = toFrameLocal(bWorld, frame);
			const Vec2 d = b - a;

			const float e = frame.halfExtent + frame.margin;
			const double minX = -e, maxX = e;
			const double minY = -e, maxY = e;

			double t0 = 0.0, t1 = 1.0;
			auto clip = [&](double p, double q) -> bool
			{
				if (Math::Abs(p) < 1e-9) return (q >= 0.0);
				const double r = q / p;
				if (p < 0.0)
				{
					if (r > t1) return false;
					if (r > t0) t0 = r;
				}
				else
				{
					if (r < t0) return false;
					if (r < t1) t1 = r;
				}
				return true;
			};

			return clip(-d.x, a.x - minX)
			    && clip( d.x, maxX - a.x)
			    && clip(-d.y, a.y - minY)
			    && clip( d.y, maxY - a.y);
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
			MapGenerator::Settlement& settlement,
			const KaidoSegment& kaido,
			const World& world,
			RoadNetwork& network)
		{
			[[maybe_unused]] const uint64 localSeed = seed ^ (0xCA57A11ULL + static_cast<uint64>(settlementIndex) * 2654435761ULL);

			const float halfExtent = castleHalfExtent(settlement);
			const float arterialRadius = halfExtent;
			const float ringTolerance = Max(150.0f, halfExtent * 0.15f);

			// 1) 城下町サイズに合わせた半径で幹線ノード抽出
			Array<int> arterialNodes = collectArterialNodesAroundRadius(
				settlement.center, arterialRadius, ringTolerance, network);

			// 2) 半径内の幹線を削除
			removeInnerArterials(settlement.center, arterialRadius, network);

			// 3) グリッド軸：街道が通過する場合はその方向を優先し、settlement に保存（buildCastleTownFrames と共有）
			Vec2 axisX, axisZ;
			if (kaido.passesThrough && kaido.dirAtCenter.lengthSq() > 1e-6f)
			{
				axisX = safeNormalized(kaido.dirAtCenter, Vec2{ 1.0f, 0.0f });
				axisZ = Vec2{ -axisX.y, axisX.x };
			}
			else
			{
				computeCastleGridAxes(settlement.center, halfExtent, arterialNodes, network, axisX, axisZ);
				axisX = safeNormalized(axisX, Vec2{ 1.0f, 0.0f });
				axisZ = safeNormalized(axisZ, Vec2{ 0.0f, 1.0f });
			}
			settlement.gridAxisX = axisX;
			settlement.gridAxisZ = axisZ;

			// 幹線ノードは作業用に新規 ID を発行し直す
			Array<int> arterialWorkNodes;
			arterialWorkNodes.reserve(arterialNodes.size());
			for (const int nodeId : arterialNodes)
			{
				const RoadNode* src = network.getNode(nodeId);
				if (!src) continue;
				const int newNodeId = network.addNode(src->position, NodeType::Joint);
				arterialWorkNodes << newNodeId;
				if (auto eid = network.addEdge(
					nodeId, newNodeId,
					src->position,
					src->position,
					RoadType::Arterial, 4))
				{
					rebuildStraightGeometry(*eid, world, network);
				}
			}

			// 4) 格子構築（外郭 + 内郭、テンプレは従来）
			const float extentWithMargin = halfExtent + 40.0f;
			const Array<float> coords = buildCastleGridCoords(halfExtent);
			const int n = static_cast<int>(coords.size());

		Grid<int> nodeIds(n, n, -1);
		HashSet<int> outerGridPointSet;
		HashTable<int, Array<GridAdjEdge>> gridGraph;

		auto localToWorld = [&](float lx, float lz) -> Vec3
		{
			const Vec2 xz = settlement.center + axisX * lx + axisZ * lz;
			const float y = world.computeHeight(static_cast<float>(xz.x), static_cast<float>(xz.y));
			return Vec3{ xz.x, y, xz.y };
		};

		for (int row = 0; row < n; ++row)
		{
			for (int col = 0; col < n; ++col)
			{
				const float lx = coords[col];
				const float lz = coords[row];
				if (Math::Abs(lx) > extentWithMargin || Math::Abs(lz) > extentWithMargin) continue;
				const Vec3 pos = localToWorld(lx, lz);
				if (pos.y < 0.5f) continue;

				const int nid = network.addNode(pos, NodeType::Intersection);
				nodeIds[{ col, row }] = nid;

				if (row == 0 || row == n - 1 || col == 0 || col == n - 1)
				{
					outerGridPointSet.insert(nid);
				}
			}
		}

		auto registerGridEdge = [&](int edgeId, int nodeA, int nodeB)
		{
			if (edgeId < 0 || nodeA < 0 || nodeB < 0) return;
			const RoadNode* na = network.getNode(nodeA);
			const RoadNode* nb = network.getNode(nodeB);
			if (!na || !nb) return;
			const float len = static_cast<float>((na->position - nb->position).length());
			gridGraph[nodeA] << GridAdjEdge{ nodeB, edgeId, len };
			gridGraph[nodeB] << GridAdjEdge{ nodeA, edgeId, len };
		};

		auto tryAddGridEdge = [&](int colA, int rowA, int colB, int rowB)
		{
			if (insideCastleBlock(coords[colA], coords[rowA], coords[colB], coords[rowB])) return;

			const int nodeA = nodeIds[{ colA, rowA }];
			const int nodeB = nodeIds[{ colB, rowB }];
			if (nodeA < 0 || nodeB < 0 || nodeA == nodeB) return;

			int edgeId = findEdgeBetweenNodes(network, nodeA, nodeB);
			if (edgeId < 0)
			{
				if (!tryAddLocalRoadEdge(network, nodeA, nodeB)) return;
				edgeId = findEdgeBetweenNodes(network, nodeA, nodeB);
			}
			registerGridEdge(edgeId, nodeA, nodeB);
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

		Array<int> outerGridPoints;
		outerGridPoints.reserve(outerGridPointSet.size());
		for (const int nid : outerGridPointSet) outerGridPoints << nid;
		if (outerGridPoints.isEmpty()) return;

			// 5) 抽出幹線ノード -> 最寄り最外周格子点へ接続（A* / 失敗時直線）
			// 6) 接続した格子点リストを保持
			Array<int> connectedOuterGridPoints;
			HashSet<int> connectedOuterSet;
			Array<int> newlyAddedArterials;
			for (const int arterialNodeId : arterialWorkNodes)
			{
				const RoadNode* src = network.getNode(arterialNodeId);
				if (!src) continue;

			int bestOuterNode = -1;
			double bestDistSq = 1e300;
			for (const int outerNodeId : outerGridPoints)
			{
				const RoadNode* dst = network.getNode(outerNodeId);
				if (!dst) continue;
				const double dx = src->position.x - dst->position.x;
				const double dz = src->position.z - dst->position.z;
				const double dSq = dx * dx + dz * dz;
				if (dSq < bestDistSq)
				{
					bestDistSq = dSq;
					bestOuterNode = outerNodeId;
				}
			}
			if (bestOuterNode < 0) continue;

			Array<int> connectorEdges;
			bool connected = false;
			if (addArterialWithPathfinder(world, network, arterialNodeId, bestOuterNode, &connectorEdges))
			{
				for (const int eid : connectorEdges) newlyAddedArterials << eid;
				connected = true;
			}
			else
			{
				const RoadNode* dst = network.getNode(bestOuterNode);
				if (dst)
				{
					const Vec3 dir = dst->position - src->position;
					if (auto eid = network.addEdge(
						arterialNodeId, bestOuterNode,
						src->position + dir * (1.0 / 3.0),
						src->position + dir * (2.0 / 3.0),
						RoadType::Arterial, 4))
					{
						newlyAddedArterials << *eid;
						connected = true;
					}
				}
			}

			if (connected && !connectedOuterSet.contains(bestOuterNode))
			{
				connectedOuterSet.insert(bestOuterNode);
				connectedOuterGridPoints << bestOuterNode;
			}
		}

		// 7) 接続格子点ペアを格子内探索（曲がりペナルティ）して経路を幹線化
		constexpr float kTurnPenalty = 180.0f;
		HashSet<int> arterialGridEdges;
		for (size_t i = 0; i < connectedOuterGridPoints.size(); ++i)
		{
			for (size_t j = i + 1; j < connectedOuterGridPoints.size(); ++j)
			{
				const int startNodeId = connectedOuterGridPoints[i];
				const int goalNodeId = connectedOuterGridPoints[j];
				const Array<int> edgePath = findGridPathTurnPenalty(
					network, startNodeId, goalNodeId, gridGraph, kTurnPenalty);
				for (const int edgeId : edgePath)
				{
					arterialGridEdges.insert(edgeId);
				}
			}
		}

		for (const int edgeId : arterialGridEdges)
		{
			if (RoadEdge* edge = network.getEdge(edgeId))
			{
				edge->roadType = RoadType::Arterial;
				newlyAddedArterials << edgeId;
			}
		}

		// Route 再付与: 新規幹線辺に隣接 Route を継承
		HashSet<int> uniqueArterialEdges;
		for (const int eid : newlyAddedArterials) uniqueArterialEdges.insert(eid);
		for (const int eid : uniqueArterialEdges)
		{
			reattachRoutesFromNeighbors(network, eid);
		}

		Logger << U"[DistrictRoads] CastleTown si={}: arterialNodes={}, connectors={}, arterialGridEdges={}"_fmt(
			settlementIndex, arterialNodes.size(), connectedOuterGridPoints.size(), arterialGridEdges.size());
	}

		void generatePostTown(
			uint64 seed,
			int settlementIndex,
			const MapGenerator::Settlement& settlement,
			const KaidoSegment& kaido,
			const World& world,
			RoadNetwork& network)
		{
			(void)seed;
			(void)settlementIndex;
			(void)settlement;
			(void)world;
			(void)network;
			if (!kaido.passesThrough || kaido.nodeIds.size() < 3)
			{
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
			(void)settlement;
			if (!kaido.passesThrough || kaido.edgeIds.isEmpty())
			{
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
			return;
		}
	}

	void straightenCastleTownRoads(
		const Array<MapGenerator::Settlement>& settlements,
		const World& world,
		RoadNetwork& network)
	{
		const Array<CastleTownFrame> frames = buildCastleTownFrames(settlements, network);
		if (frames.isEmpty()) return;

		Array<int> targetEdgeIds;
		targetEdgeIds.reserve(network.edges().size());

		for (const auto& edge : network.edges())
		{
			if (edge.id < 0) continue;
			const RoadNode* nodeA = network.getNode(edge.nodeA);
			const RoadNode* nodeB = network.getNode(edge.nodeB);
			if (!nodeA || !nodeB) continue;

			const Vec2 a{ nodeA->position.x, nodeA->position.z };
			const Vec2 b{ nodeB->position.x, nodeB->position.z };
			for (const auto& frame : frames)
			{
				if (segmentIntersectsFrame(a, b, frame))
				{
					targetEdgeIds << edge.id;
					break;
				}
			}
		}

		{
			int localCount = 0;
			for (const int eid : targetEdgeIds)
			{
				const RoadEdge* e = network.getEdge(eid);
				if (e && e->roadType == RoadType::LocalRoad) ++localCount;
			}
			Console << U"[straighten] target={} (LocalRoad={})"_fmt(targetEdgeIds.size(), localCount);
			if (!frames.isEmpty())
				Console << U"[straighten] frame axisX=({:.3f},{:.3f})"_fmt(frames[0].axisX.x, frames[0].axisX.y);
		}

		for (const int edgeId : targetEdgeIds)
		{
			RoadEdge* edge = network.getEdge(edgeId);
			if (!edge) continue;
			const RoadNode* nodeA = network.getNode(edge->nodeA);
			const RoadNode* nodeB = network.getNode(edge->nodeB);
			if (!nodeA || !nodeB) continue;

			const Vec3 dir = nodeB->position - nodeA->position;
			if (dir.lengthSq() < 1e-8) continue;

			edge->ctrlA = nodeA->position + dir * (1.0 / 3.0);
			edge->ctrlB = nodeA->position + dir * (2.0 / 3.0);
			if (const auto bez = network.getBezier(edgeId))
			{
				edge->length = bez->totalLength;
			}
			network.updateEdgeElevation(edgeId, world);
			network.rebuildNodeConnectivity(edge->nodeA, edge->nodeB);
		}

		// 修正後LocalRoadの接線が frame axisX/Z と一致しているか先頭5件を確認
		if (!frames.isEmpty())
		{
			const CastleTownFrame& f0 = frames[0];
			int checked = 0;
			for (const int eid : targetEdgeIds)
			{
				if (checked >= 5) break;
				const RoadEdge* e = network.getEdge(eid);
				if (!e || e->roadType != RoadType::LocalRoad) continue;
				const RoadNode* na = network.getNode(e->nodeA);
				const RoadNode* nb = network.getNode(e->nodeB);
				if (!na || !nb) continue;
				const Vec3 d = nb->position - na->position;
				const double len = d.length();
				if (len < 1.0) continue;
				const Vec2 dir2D{ d.x / len, d.z / len };
				const double dotX = dir2D.dot(f0.axisX);
				const double dotZ = dir2D.dot(f0.axisZ);
				Console << U"[straighten edge {}] dir=({:.3f},{:.3f}) dotX={:.3f} dotZ={:.3f}"_fmt(
					eid, dir2D.x, dir2D.y, dotX, dotZ);
				++checked;
			}
		}
	}

}
