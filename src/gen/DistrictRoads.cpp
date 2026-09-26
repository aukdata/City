#include "GenerationSettings.hpp"
#include "DistrictRoads.hpp"
#include "RoadNodeIndex.hpp"
#include "StreetProfile.hpp"
#include "SettlementFringe.hpp"
#include "NewTownLayout.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../debug/DebugLog.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <random>

namespace DistrictRoads
{
	namespace
	{
		Vec2 safeNormalized(Vec2 v, Vec2 fallback = Vec2{ 1.0, 0.0 })
		{
			if (v.lengthSq() < 1e-6) return fallback;
			v.normalize();
			return v;
		}

		uint32 districtHash(uint64 seed, int a, int b, int c = 0)
		{
			uint64 value = seed ^ (static_cast<uint64>(a) * 0x9E3779B97F4A7C15ULL);
			value ^= static_cast<uint64>(b) * 0xBF58476D1CE4E5B9ULL;
			value ^= static_cast<uint64>(c) * 0x94D049BB133111EBULL;
			value ^= value >> 30;
			value *= 0xBF58476D1CE4E5B9ULL;
			value ^= value >> 27;
			value *= 0x94D049BB133111EBULL;
			value ^= value >> 31;
			return static_cast<uint32>(value);
		}

		float districtHashSigned(uint64 seed, int a, int b, int c = 0)
		{
			return (static_cast<float>(districtHash(seed, a, b, c) & 0xFFFFu) / 32767.5f) - 1.0f;
		}
		/// @brief Arterial / LocalRoad のみを街道候補エッジとみなす
		bool isKaidoEdge(const RoadNetwork& network, int edgeId)
		{
			const RoadEdge* e = network.getEdge(edgeId);
			if (!e || e->id < 0) return false;
			return e->roadType == RoadType::Arterial
				|| e->roadType == RoadType::LocalRoad;
		}

		/// @brief Change generation profiles together: geometry, lane directions and speed.
		void applyStreetProfile(RoadEdge& edge, int laneCount, bool arterial, bool reverse = false)
		{
			edge.roadType = arterial ? RoadType::Arterial : RoadType::LocalRoad;
			edge.lanes = RoadNetwork::buildDefaultLanes(laneCount, edge.roadType);
			edge.speedLimit = arterial ? (laneCount >= 4 ? GenerationSettings::get().districtRoads_mainStreetSpeed : GenerationSettings::get().districtRoads_collectorSpeed) : GenerationSettings::get().districtRoads_localSpeed;
			if (!arterial)
			{
				for (auto& lane : edge.lanes)
				{
					lane.lineLeft = lane.lineRight = LineType::None;
					if (laneCount == 1) { lane.dir = reverse ? LaneDir::Backward : LaneDir::Forward; }
				}
			}
			RoadNetwork::buildDefaultParts(edge);
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
			if (dh / dist > GenerationSettings::get().districtRoads_maxSlope) return false;

			const Vec3 dir = (nb->position - na->position);
			const Vec3 ctrlA = na->position + dir * (1.0 / 3.0);
			const Vec3 ctrlB = na->position + dir * (2.0 / 3.0);
			return static_cast<bool>(network.addEdge(nodeIdA, nodeIdB, ctrlA, ctrlB,
			                                         RoadType::LocalRoad, 2));
		}

		int ensureNodeWithMerge(
			RoadNodeIndex& nodeHash,
			const World& world,
			RoadNetwork& network,
			Vec2 xz,
			NodeType type = NodeType::Intersection)
		{
			// 新規候補点は既存近傍ノードへ吸着し、十分離れているときだけ新設する。
			const float y = world.sampleHeight(static_cast<float>(xz.x), static_cast<float>(xz.y));
			if (y < GenerationSettings::get().districtRoads_minimumDryHeight) return -1;
			const Vec3 pos{ xz.x, y, xz.y };

			const auto existing = nodeHash.findNearest(pos, network, GenerationSettings::get().districtRoads_nodeMergeRadius);
			if (existing) { return *existing; }

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
			// A small civic square leaves the town centre walkable and connected.
			
			return (Math::Abs(midX) <= GenerationSettings::get().districtRoads_civicSquareHalfSize && Math::Abs(midZ) <= GenerationSettings::get().districtRoads_civicSquareHalfSize);
		}

		Array<float> buildCastleGridCoords(float halfExtent, uint64 seed, int axis)
		{
			// 城下町グリッドの分割数を、街区幅制約を守りつつ 64m 近傍になるよう選ぶ。
			Array<float> out;
			const float fullExtent = halfExtent * 2.0f;

			// セル幅制約 [64m, 190m] を満たす分割数を探索し、64m 近傍を優先する
			const int minCells = Max(1, static_cast<int>(Ceil(fullExtent / GenerationSettings::get().districtRoads_maximumBlockWidth)));
			const int maxCells = Max(minCells, static_cast<int>(Floor(fullExtent / GenerationSettings::get().districtRoads_minimumBlockWidth)));

			int bestCells = minCells;
			float bestScore = 1e30f;
			for (int cells = minCells; cells <= maxCells; ++cells)
			{
				const float cellW = fullExtent / cells;
				
				const float score = Math::Abs(cellW - GenerationSettings::get().districtRoads_preferredBlockWidth);
				if (score < bestScore)
				{
					bestScore = score;
					bestCells = cells;
				}
			}

			// Vary entire street intervals, never individual intersection positions.
			// Merchant frontage has shallow blocks; former residential quarters are deeper.
			Array<float> widths;
			float total = 0.0f;
			for (int i = 0; i < bestCells; ++i)
			{
				const bool merchantQuarter = Abs(i - bestCells / 2) <= bestCells / 5;
				const float districtScale = merchantQuarter ? (axis == 0 ? GenerationSettings::get().districtRoads_merchantWidthRatio : GenerationSettings::get().districtRoads_merchantDepthRatio) : (axis == 0 ? GenerationSettings::get().districtRoads_housingWidthRatio : GenerationSettings::get().districtRoads_housingDepthRatio);
				const float width = districtScale * (1.0f + districtHashSigned(seed, i, axis, 11) * 0.045f);
				widths << width;
				total += width;
			}
			out << -halfExtent;
			for (const float width : widths) { out << out.back() + fullExtent * width / total; }
			out.back() = halfExtent;
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

		/// @brief Clip the old regional network to the actual rotated town boundary.
		Array<int> cutCastleApproaches(const Vec2& center, const Vec2& axisX, const Vec2& axisZ,
			Vec2 halfExtent, RoadNetwork& network)
		{
			
			const Vec2 boundary = halfExtent + Vec2{GenerationSettings::get().districtRoads_approachMargin,GenerationSettings::get().districtRoads_approachMargin};
			const auto signedDistance = [&](const Vec3& p)
			{
				const Vec2 local{ p.x - center.x, p.z - center.y };
				return Max(Abs(local.dot(axisX))-boundary.x, Abs(local.dot(axisZ))-boundary.y);
			};
			Array<int> originalEdges;
			for (const auto& edge : network.edges())
			{
				if (edge.id >= 0 && (edge.roadType == RoadType::Arterial || edge.roadType == RoadType::LocalRoad)) { originalEdges << edge.id; }
			}
			HashSet<int> entrances;
			int removed = 0;
			for (const int id : originalEdges)
			{
				const auto original = network.getBezier(id);
				if (!original) { continue; }
				// A regional node may lie exactly on the clipping boundary. Splitting
				// at t=0/1 is intentionally rejected, so retain that existing gateway.
				const auto* source = network.getEdge(id);
				for (const int nodeId : {source->nodeA, source->nodeB})
				{
					if (Abs(signedDistance(network.getNode(nodeId)->position)) <= 0.05)
					{
						entrances.insert(nodeId);
					}
				}
				Array<float> cuts;
				const int steps = Max(8, static_cast<int>(Ceil(original->totalLength / 16.0f)));
				for (int step = 1; step <= steps; ++step)
				{
					float low = (step - 1.0f) / steps, high = step / static_cast<float>(steps);
					const bool inside = signedDistance(original->evaluate(low)) < 0.0;
					if (inside == (signedDistance(original->evaluate(high)) < 0.0)) { continue; }
					for (int iteration = 0; iteration < 20; ++iteration)
					{
						const float mid = (low + high) * 0.5f;
						if ((signedDistance(original->evaluate(mid)) < 0.0) == inside) { low = mid; }
						else { high = mid; }
					}
					cuts << (low + high) * 0.5f;
				}
				Array<int> pieces{ id };
				// Split from the end so earlier parameters remain exact on the prefix.
				int prefix = id;
				float previous = 1.0f;
				for (auto cut = cuts.rbegin(); cut != cuts.rend(); ++cut)
				{
					const auto* edge = network.getEdge(prefix);
					if (!edge) { break; }
					const int start = edge->nodeA;
					const int split = network.splitEdgeAtParameter(prefix, *cut / previous);
					if (split < 0) { continue; }
					entrances.insert(split);
					for (const int child : network.getNode(split)->edgeIds())
					{
						pieces << child;
						if (network.getEdge(child)->nodeA == start) { prefix = child; }
					}
					previous = *cut;
				}
				for (const int piece : pieces)
				{
					const auto curve = network.getBezier(piece);
					if (curve && signedDistance(curve->evaluate(0.5f)) < -0.01)
					{
						network.removeEdge(piece);
						++removed;
					}
				}
			}
			Array<int> result;
			for (const int id : entrances)
			{
				const auto* node = network.getNode(id);
				if (node && !node->attachments.isEmpty()) { result << id; }
			}
			result.sort();
			// Closely spaced parallel regional approaches share one gateway. Giving
			// each a separate city mouth made their curved connectors cross.
			int mergedGateways=0;
			for (size_t first=0;first<result.size();++first)
			{
				RoadNode* kept=network.getNode(result[first]);
				if (!kept) { continue; }
				for (size_t second=first+1;second<result.size();++second)
				{
					const RoadNode* mergedNode=network.getNode(result[second]);
					if (!mergedNode || kept->position.distanceFrom(mergedNode->position)>GenerationSettings::get().districtRoads_nodeMergeRadius
						|| Abs(kept->position.y-mergedNode->position.y)>1) { continue; }
					const auto side=[&](Vec3 position)
					{
						const Vec2 delta{position.x-center.x,position.z-center.y};
						const double x=delta.dot(axisX)/boundary.x,z=delta.dot(axisZ)/boundary.y;
						return Abs(x)>Abs(z) ? (x>0 ? 0 : 1) : (z>0 ? 2 : 3);
					};
					if (side(kept->position)!=side(mergedNode->position)) { continue; }
					const Vec3 shift=kept->position-mergedNode->position;
					const auto attachments=mergedNode->attachments;
					for (const auto& attachment : attachments)
					{
						auto* edge=network.getEdge(attachment.edgeId);
						if (!edge) { continue; }
						if (edge->nodeA==mergedNode->id) { edge->nodeA=kept->id; edge->ctrlA+=shift; }
						if (edge->nodeB==mergedNode->id) { edge->nodeB=kept->id; edge->ctrlB+=shift; }
						if (edge->nodeA==edge->nodeB) { network.removeEdge(edge->id); continue; }
						kept->attachments << attachment;
						edge->length=network.getBezier(edge->id)->totalLength;
					}
					network.removeNode(result[second]);
					network.rebuildNodeConnectivity(kept->id,kept->id);
					++mergedGateways;
				}
			}
			result.remove_if([&](int id) { return !network.getNode(id); });
			DBG_LOG(U"[SettlementPlan] mergedGateways={}"_fmt(mergedGateways));
			DBG_LOG(U"[SettlementPlan] clippedInteriorRoads={} boundaryEntrances={}"_fmt(removed, result.size()));
			return result;
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
			return Max(GenerationSettings::get().districtRoads_castleHalfMin, Min(GenerationSettings::get().districtRoads_castleHalfMax, settlement.radius * GenerationSettings::get().districtRoads_castleExtentRatio));
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
			std::uniform_real_distribution<float> ratioDist(GenerationSettings::get().districtRoads_gridPruneMinimum, GenerationSettings::get().districtRoads_gridPruneMaximum);
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
			if (!nodeA || !nodeB || !bez || bez->totalLength < GenerationSettings::get().districtRoads_crankMinimumLength) return;

			Vec2 dirAB{
				static_cast<float>(nodeB->position.x - nodeA->position.x),
				static_cast<float>(nodeB->position.z - nodeA->position.z)
			};
			dirAB = safeNormalized(dirAB, Vec2{ 1.0, 0.0 });

			const int nodeN1 = network.splitEdgeAt(targetEdgeId, bez->totalLength * GenerationSettings::get().districtRoads_crankFirstSplit);
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
			if (!bez2 || bez2->totalLength < GenerationSettings::get().districtRoads_crankSecondMinimumLength) return;

			const int nodeN2 = network.splitEdgeAt(forwardEdgeId, bez2->totalLength * GenerationSettings::get().districtRoads_crankSecondSplit);
			if (nodeN2 < 0) return;

			const Vec2 kaidoDir = computeKaidoAxisOverall(kaido, network);
			Vec2 normal{ -kaidoDir.y, kaidoDir.x };
			normal = safeNormalized(normal, Vec2{ 0.0, 1.0 });

			std::mt19937_64 rng(seed ^ (0xC1A0D0ULL + static_cast<uint64>(settlementIndex) * 7919ULL));
			std::uniform_real_distribution<float> offDist(GenerationSettings::get().districtRoads_crankOffsetMinimum, GenerationSettings::get().districtRoads_crankOffsetMaximum);
			const float offset = offDist(rng);

			auto moveNode = [&](int nodeId, float sign)
			{
				RoadNode* node = network.getNode(nodeId);
				if (!node) return;

				const float nx = static_cast<float>(node->position.x)
				               + static_cast<float>(normal.x) * offset * sign;
				const float nz = static_cast<float>(node->position.z)
				               + static_cast<float>(normal.y) * offset * sign;
				const float ny = world.sampleHeight(nx, nz);
				if (ny < GenerationSettings::get().districtRoads_minimumDryHeight) return;

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
				double bestAlign = GenerationSettings::get().districtRoads_minimumKaidoAlignment;
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
			Vec2 halfExtent{0,0};
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
				if (!settlement.plan.ready || settlement.plan.frontageRoads) { continue; }

				const Vec2 halfExtent = settlement.plan.halfExtent;
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

	namespace
	{
		bool segmentFitsTerrain(const World& world, Vec3 a, Vec3 b,double minimumHeight=GenerationSettings::get().districtRoads_minimumTerrainHeight)
		{
			const double distance=Vec2{a.x,a.z}.distanceFrom(Vec2{b.x,b.z});
			const int samples=Max(2,static_cast<int>(Ceil(distance/GenerationSettings::get().districtRoads_terrainSampleStep)));
			double previous=world.sampleHeight(static_cast<float>(a.x),static_cast<float>(a.z));
			if (previous<world.waterSurfaceHeight(a.x,a.z)+minimumHeight) { return false; }
			for (int sample=1;sample<=samples;++sample)
			{
				const Vec3 p=a+(b-a)*(static_cast<double>(sample)/samples);
				const double height=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
				if (height<world.waterSurfaceHeight(p.x,p.z)+minimumHeight || Abs(height-previous)>distance/samples*GenerationSettings::get().districtRoads_maxSlope) { return false; }
				previous=height;
			}
			return true;
		}

		/// @brief Plan before removing any regional road; reject submerged and steep streets.
		bool fitTownToTerrain(MapGenerator::Settlement& settlement, const World& world)
		{
			for (int attempt=0;attempt<12;++attempt)
			{
				const auto x=UrbanMorphology::streetCoordinates(settlement.plan,false);
				const auto z=UrbanMorphology::streetCoordinates(settlement.plan,true);
				const auto position=[&](float localX,float localZ)
				{
					const Vec2 p=settlement.center+settlement.gridAxisX*localX+settlement.gridAxisZ*localZ;
					return Vec3{p.x,world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.y)),p.y};
				};
				bool fits=true;
				for (size_t row=0;row<z.size() && fits;++row)
				{
					for (size_t col=0;col<x.size() && fits;++col)
					{
						if (col+1<x.size() && UrbanMorphology::allowStreet(settlement.plan,{x[col],z[row]},{x[col+1],z[row]}))
						{
							fits=segmentFitsTerrain(world,position(x[col],z[row]),position(x[col+1],z[row]),GenerationSettings::get().districtRoads_townFreeboard);
						}
						if (fits && row+1<z.size() && UrbanMorphology::allowStreet(settlement.plan,{x[col],z[row]},{x[col],z[row+1]}))
						{
							fits=segmentFitsTerrain(world,position(x[col],z[row]),position(x[col],z[row+1]),GenerationSettings::get().districtRoads_townFreeboard);
						}
					}
				}
				// Street surfaces alone do not establish buildable plots: sample the full block interiors.
				for (double localZ=-settlement.plan.halfExtent.y+GenerationSettings::get().districtRoads_buildableSampleInset;localZ<settlement.plan.halfExtent.y && fits;localZ+=GenerationSettings::get().districtRoads_buildableSampleStep)
				{
					for (double localX=-settlement.plan.halfExtent.x+GenerationSettings::get().districtRoads_buildableSampleInset;localX<settlement.plan.halfExtent.x && fits;localX+=GenerationSettings::get().districtRoads_buildableSampleStep)
					{
						const Vec3 point=position(static_cast<float>(localX),static_cast<float>(localZ));
						fits=point.y>=world.waterSurfaceHeight(point.x,point.z)+GenerationSettings::get().districtRoads_townFreeboard;
					}
				}
				if (fits)
				{
					if (settlement.plan.station)
					{
						const auto center=[](const Array<float>& coordinates,double value)
						{
							for (size_t i=1;i<coordinates.size();++i) { if (value<=coordinates[i]) { return (coordinates[i-1]+coordinates[i])*.5; } }
							return value;
						};
						settlement.plan.station=Vec2{center(x,settlement.plan.station->x),center(z,settlement.plan.station->y)};
					}
					UrbanStructure::alignCenters(settlement.plan);
					return true;
				}
				if (Min(settlement.plan.halfExtent.x,settlement.plan.halfExtent.y)*GenerationSettings::get().districtRoads_townShrinkRatio<GenerationSettings::get().districtRoads_minimumTownHalfExtent) { return false; }
				UrbanMorphology::rescale(settlement.plan,GenerationSettings::get().districtRoads_townShrinkRatio);
			}
			return false;
		}

		/// @brief 集村は裏道の輪、散村は農地への取付道、谷筋は短い枝道。街道自体は保存する。
		void generateRuralFrontage(MapGenerator::Settlement& settlement,const World& world,RoadNetwork& network)
		{
			settlement.plan.frontageRoads=true;
			Array<int> originalRoads;
			const Vec2 plannedExtent=settlement.plan.halfExtent;
			const double searchRadius=plannedExtent.length()+GenerationSettings::get().districtRoads_ruralRoadSearchMargin;
			for (const auto& edge : network.edges())
			{
				if (edge.id<0 || (edge.roadType!=RoadType::Arterial && edge.roadType!=RoadType::LocalRoad)) { continue; }
				const Vec3 a=network.getNode(edge.nodeA)->position,b=network.getNode(edge.nodeB)->position;
				const double minX=Min(Min(a.x,b.x),Min(edge.ctrlA.x,edge.ctrlB.x)), maxX=Max(Max(a.x,b.x),Max(edge.ctrlA.x,edge.ctrlB.x));
				const double minZ=Min(Min(a.z,b.z),Min(edge.ctrlA.z,edge.ctrlB.z)), maxZ=Max(Max(a.z,b.z),Max(edge.ctrlA.z,edge.ctrlB.z));
				if (settlement.center.x<minX-searchRadius || settlement.center.x>maxX+searchRadius
					|| settlement.center.y<minZ-searchRadius || settlement.center.y>maxZ+searchRadius) { continue; }
				originalRoads << edge.id;
			}
			Array<int> spineRoads=originalRoads;
			Array<int> backNodes;
			int added=0,noAnchor=0,noTerrain=0,noConnection=0;
			const bool clustered=settlement.plan.ruralForm==UrbanMorphology::RuralForm::Clustered;
			const bool dispersed=settlement.plan.ruralForm==UrbanMorphology::RuralForm::Dispersed;
			for (int index=-2;index<=2;++index)
			{
				const Vec2 desired=settlement.center+settlement.gridAxisX*(index*plannedExtent.x*GenerationSettings::get().districtRoads_ruralAnchorRatio);
				int bestNode=-1;
				Vec3 anchorPosition{},tangent{};
				const double anchorStep=plannedExtent.x*GenerationSettings::get().districtRoads_ruralAnchorRatio;
				double bestDistance=Square(Min(GenerationSettings::get().districtRoads_ruralAnchorSearchRadius,Max(20.0,anchorStep*.4)));
				for (const int id : spineRoads)
				{
					const auto* edge=network.getEdge(id);
					const auto curve=network.getBezier(id);
					if (!edge || !curve || curve->totalLength<3) { continue; }
					for (const int nodeId : {edge->nodeA,edge->nodeB})
					{
						const auto* node=network.getNode(nodeId);
						if (!node || node->attachments.size()>2) { continue; }
						const double distance=desired.distanceFromSq(Vec2{node->position.x,node->position.z});
						if (distance>=bestDistance) { continue; }
						bestDistance=distance;bestNode=nodeId;anchorPosition=node->position;
						tangent=curve->tangentAt(nodeId==edge->nodeA ? 0 : curve->totalLength);
					}
				}
				if (bestNode<0)
				{
					// 交差点のない長い街道にも枝道の接続点を作る。
					int bestEdge=-1;float bestArc=0;
					double bestRoadDistance=Square(GenerationSettings::get().districtRoads_ruralAnchorSearchRadius);
					for (const int id:spineRoads)
					{
						const auto curve=network.getBezier(id);
						if (!curve) { continue; }
						const int samples=Max(8,static_cast<int>(Ceil(curve->totalLength/16.0f)));
						for (int sample=1;sample<samples;++sample)
						{
							const float arc=curve->totalLength*sample/samples;
							if (arc<GenerationSettings::get().districtRoads_minimumRuralBranchLength
								|| curve->totalLength-arc<GenerationSettings::get().districtRoads_minimumRuralBranchLength) { continue; }
							const Vec3 point=curve->positionAt(arc);
							const double distance=desired.distanceFromSq({point.x,point.z});
							if (distance<bestRoadDistance) { bestRoadDistance=distance;bestEdge=id;bestArc=arc; }
						}
					}
					if (bestEdge>=0)
					{
						const auto curve=network.getBezier(bestEdge);
						tangent=curve->tangentAt(bestArc);
						bestNode=network.splitEdgeAt(bestEdge,bestArc);
						if (bestNode>=0)
						{
							anchorPosition=network.getNode(bestNode)->position;
							spineRoads.remove_if([&](int id) { return id==bestEdge; });
							for (const auto& attachment:network.getNode(bestNode)->attachments)
							{
								if (!spineRoads.contains(attachment.edgeId)) { spineRoads << attachment.edgeId; }
							}
						}
					}
				}
				if (bestNode<0) { ++noAnchor;continue; }
				Vec2 normal{-tangent.z,tangent.x};
				if (normal.lengthSq()<.001) { continue; }
				normal.normalize();
				const double length=dispersed ? GenerationSettings::get().districtRoads_dispersedBranchLength : (clustered ? GenerationSettings::get().districtRoads_clusterBranchLength : GenerationSettings::get().districtRoads_valleyBranchLength);
				const double side=(index%2==0 ? 1.0 : -1.0)*(settlement.plan.salt%2==0 ? 1.0 : -1.0);
				Vec2 end{};Vec3 endPosition{};bool terrainFits=false;
				for (const double candidateSide : {side,-side})
				{
					for (const double fraction : {1.0,.75,.5})
					{
						const double reach=Max(GenerationSettings::get().districtRoads_minimumRuralBranchLength,length*fraction);
						const Vec2 candidate=Vec2{anchorPosition.x,anchorPosition.z}+normal*(reach*candidateSide);
						const Vec3 target{candidate.x,world.sampleHeight(static_cast<float>(candidate.x),static_cast<float>(candidate.y)),candidate.y};
						if (!segmentFitsTerrain(world,anchorPosition,target)) { continue; }
						end=candidate;endPosition=target;terrainFits=true;break;
					}
					if (terrainFits) { break; }
				}
				if (!terrainFits) { ++noTerrain;continue; }
				const int anchor=bestNode;
				const int endNode=network.addNode(endPosition,NodeType::Endpoint);
				if (!tryAddLocalRoadEdge(network,anchor,endNode)) { network.removeNode(endNode);++noConnection;continue; }
				GeneratedStreet::apply(*network.getEdge(findEdgeBetweenNodes(network,anchor,endNode)),GeneratedStreet::describe(dispersed ? GeneratedStreet::Role::FarmAccess : GeneratedStreet::Role::Village));
				if (clustered && !backNodes.isEmpty())
				{
					const Vec3 previous=network.getNode(backNodes.back())->position;
					const double previousSide=Vec2{previous.x-settlement.center.x,previous.z-settlement.center.y}.dot(settlement.gridAxisZ);
					const double currentSide=(end-settlement.center).dot(settlement.gridAxisZ);
					if (previousSide*currentSide>0 && previous.distanceFrom(endPosition)>GenerationSettings::get().districtRoads_minimumRuralBranchLength && segmentFitsTerrain(world,previous,endPosition)
						&& tryAddLocalRoadEdge(network,backNodes.back(),endNode))
					{
						GeneratedStreet::apply(*network.getEdge(findEdgeBetweenNodes(network,backNodes.back(),endNode)),GeneratedStreet::describe(GeneratedStreet::Role::Village));
					}
				}
				backNodes << endNode;
				const Vec2 delta=end-settlement.center;
				const Vec2 local{delta.dot(settlement.gridAxisX),delta.dot(settlement.gridAxisZ)};
				settlement.plan.ruralHomes << local;
				settlement.plan.halfExtent.x=Max(settlement.plan.halfExtent.x,Abs(local.x)+GenerationSettings::get().districtRoads_ruralPlanMargin);
				settlement.plan.halfExtent.y=Max(settlement.plan.halfExtent.y,Abs(local.y)+GenerationSettings::get().districtRoads_ruralPlanMargin);
				++added;
			}
			DBG_LOG(U"[RuralFrontage] form={} accesses={} noAnchor={} noTerrain={} noConnection={} regionalRoadsPreserved=true"_fmt(
				static_cast<int>(settlement.plan.ruralForm),added,noAnchor,noTerrain,noConnection));
		}
	}

		void generateSettlement(
			uint64 seed,
			int settlementIndex,
			MapGenerator::Settlement& settlement,
			const KaidoSegment& kaido,
			const World& world,
			RoadNetwork& network)
		{
			if (!settlement.plan.ready)
			{
				const auto origin=settlement.kind==MapGenerator::SettlementKind::RegionalCity ? UrbanMorphology::Origin::Castle
					: (settlement.kind==MapGenerator::SettlementKind::LocalTown ? UrbanMorphology::Origin::Post : UrbanMorphology::Origin::Rural);
				settlement.plan=UrbanMorphology::makePlan(origin,static_cast<uint8>(settlement.kind),UrbanMorphology::Site{},seed+settlementIndex,false);
			}
			const auto& plan=settlement.plan;

			Vec2 halfExtent=plan.halfExtent;
			const float arterialRadius = static_cast<float>(halfExtent.length());
			const float ringTolerance = Max(GenerationSettings::get().districtRoads_ringToleranceMinimum, arterialRadius * GenerationSettings::get().districtRoads_ringToleranceRatio);

			// 1) 城下町サイズに合わせた半径で幹線ノード抽出
			Array<int> arterialNodes = collectArterialNodesAroundRadius(
				settlement.center, arterialRadius, ringTolerance, network);

			// 2) 既存街道から町割の軸を決める
			// Select the historical street axis before clipping the regional roads.

			// 3) グリッド軸：街道が通過する場合はその方向を優先し、settlement に保存（buildCastleTownFrames と共有）
			Vec2 axisX, axisZ;
			if (plan.origin==UrbanMorphology::Origin::Port || plan.structure==UrbanStructure::Type::CoastalHubs || plan.structure==UrbanStructure::Type::ConstrainedLinear || (plan.origin==UrbanMorphology::Origin::Rural && plan.ruralForm==UrbanMorphology::RuralForm::Valley))
			{
				axisX=settlement.gridAxisX; axisZ=settlement.gridAxisZ;
			}
			else if (kaido.passesThrough && kaido.dirAtCenter.lengthSq() > 1e-6f)
			{
				axisX = safeNormalized(kaido.dirAtCenter, Vec2{ 1.0f, 0.0f });
				axisZ = Vec2{ -axisX.y, axisX.x };
			}
			else
			{
				computeCastleGridAxes(settlement.center, arterialRadius, arterialNodes, network, axisX, axisZ);
				axisX = safeNormalized(axisX, Vec2{ 1.0f, 0.0f });
				axisZ = safeNormalized(axisZ, Vec2{ 0.0f, 1.0f });
			}
			settlement.gridAxisX = axisX;
			settlement.gridAxisZ = axisZ;

			// 宿場・門前町は既存の街道を骨格にする。町全域を矩形街路へ置き換えない。
			if (plan.origin == UrbanMorphology::Origin::Rural ||
				(plan.scale == 1 &&
					(plan.origin == UrbanMorphology::Origin::Post || plan.origin == UrbanMorphology::Origin::Temple)))
			{
				generateRuralFrontage(settlement,world,network); return;
			}
			if (!fitTownToTerrain(settlement,world))
			{
				DBG_LOG(U"[SettlementPlan] index={} terrainConstrained=true preserveRegionalRoads=true"_fmt(settlementIndex));
				generateRuralFrontage(settlement,world,network);
				SettlementFringe::generate(settlement,world,network); return;
			}
			halfExtent=plan.halfExtent;

			// Regional roads stop outside the complete rotated grid, including its corners.
			const Array<int> arterialWorkNodes = cutCastleApproaches(settlement.center, axisX, axisZ, halfExtent, network);

			// 4) 街路ごとの間隔で町人地・住宅地の街区を構築
			const Array<float> coordsX=UrbanMorphology::streetCoordinates(plan,false);
			const Array<float> coordsZ=UrbanMorphology::streetCoordinates(plan,true);
			const int n = static_cast<int>(coordsX.size());
			const int rows=static_cast<int>(coordsZ.size());

		Grid<int> nodeIds(n, rows, -1);
		HashSet<int> outerGridPointSet;
		HashTable<int, Array<GridAdjEdge>> gridGraph;

		auto localToWorld = [&](float lx, float lz) -> Vec3
		{
			const Vec2 xz = settlement.center + axisX * lx + axisZ * lz;
			const float y = world.sampleHeight(static_cast<float>(xz.x), static_cast<float>(xz.y));
			return Vec3{ xz.x, y, xz.y };
		};

		for (int row = 0; row < rows; ++row)
		{
			for (int col = 0; col < n; ++col)
			{
				const float lx = coordsX[col];
				const float lz = coordsZ[row];

				const Vec3 pos = localToWorld(lx, lz);
				if (pos.y < GenerationSettings::get().districtRoads_minimumDryHeight) continue;

				const int nid = network.addNode(pos, NodeType::Intersection);
				nodeIds[{ col, row }] = nid;

				if (row == 0 || row == rows - 1 || col == 0 || col == n - 1)
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
			if (!UrbanMorphology::allowStreet(plan,{coordsX[colA],coordsZ[rowA]},{coordsX[colB],coordsZ[rowB]})) { return; }
			if (plan.structure!=UrbanStructure::Type::None && !UrbanStructure::allowStreet(plan,coordsX,coordsZ,colA,rowA,colB,rowB)) { return; }

			const bool outerFrame = (colA == colB && (colA == 0 || colA == n - 1))
				|| (rowA == rowB && (rowA == 0 || rowA == rows - 1));

			const int nodeA = nodeIds[{ colA, rowA }];
			const int nodeB = nodeIds[{ colB, rowB }];
			if (nodeA < 0 || nodeB < 0 || nodeA == nodeB) return;

			const Vec3 start=network.getNode(nodeA)->position, end=network.getNode(nodeB)->position;
			const int samples=Max(2,static_cast<int>(Ceil(start.distanceFrom(end)/GenerationSettings::get().districtRoads_terrainSampleStep)));
			double previous=start.y;
			for (int sample=1;sample<=samples;++sample)
			{
				const Vec3 point=start+(end-start)*(static_cast<double>(sample)/samples);
				const double height=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z));
				if (height<GenerationSettings::get().districtRoads_minimumConnectorHeight || Abs(height-previous)>start.distanceFrom(end)/samples*GenerationSettings::get().districtRoads_maximumConnectorSlope) { return; }
				previous=height;
			}
			int edgeId = findEdgeBetweenNodes(network, nodeA, nodeB);
			if (edgeId < 0)
			{
				if (!tryAddLocalRoadEdge(network, nodeA, nodeB)) return;
				edgeId = findEdgeBetweenNodes(network, nodeA, nodeB);
			}
			if (RoadEdge* edge = network.getEdge(edgeId))
			{
				const int corridor = rowA == rowB ? rowA : colA;
				const int centreCorridor = rowA == rowB ? rows / 2 : n / 2;
				const int offset = Abs(corridor - centreCorridor);
				const bool largeCity=plan.scale==0 && Max(halfExtent.x,halfExtent.y)>GenerationSettings::get().districtRoads_mainArterialTownSize;
				const bool boulevard = largeCity && !outerFrame && offset==0;
				const auto& coordinates=rowA==rowB ? coordsZ : coordsX;
				int stationCorridor=-1;
				if (plan.station)
				{
					const double desired=rowA==rowB ? plan.station->y : plan.station->x;
					for (int index=0;index<static_cast<int>(coordinates.size());++index)
					{
						if (coordinates[index]>=desired) { stationCorridor=index; break; }
					}
				}
				const bool collector = plan.scale!=2 && !boulevard && ((!outerFrame && (plan.origin==UrbanMorphology::Origin::Planned ? offset % 4 == 0 : offset % 3 == 0)) || (outerFrame && plan.origin!=UrbanMorphology::Origin::Castle) || (corridor==stationCorridor && (plan.origin!=UrbanMorphology::Origin::Planned || corridor%2==0)));
				const bool oneWay = plan.scale!=2 && plan.origin!=UrbanMorphology::Origin::Planned && !boulevard && !collector && rowA != rowB && offset < 3;
				using Role=GeneratedStreet::Role;
				const Role role=plan.scale==2 ? (offset==0 ? Role::Village : Role::FarmAccess)
					: (boulevard ? Role::MainArterial : (collector ? Role::Collector : (oneWay ? Role::OneWay
					: (plan.origin==UrbanMorphology::Origin::Planned ? Role::ResidentialWalkways : Role::Local))));
				GeneratedStreet::Profile profile=GeneratedStreet::describe(plan.structure==UrbanStructure::Type::None ? role
					: UrbanStructure::streetRole(plan,coordsX,coordsZ,colA,rowA,colB,rowB));
				if (corridor==stationCorridor) { profile.walkwayLeft=profile.walkwayRight=GenerationSettings::get().districtRoads_stationWalkwayWidth; }
				if (outerFrame && plan.scale!=2)
				{
					// Only the developed side of the urban edge needs a raised walkway.
					const bool developedOnLeft=rowA==rowB ? rowA==0 : colA==n-1;
					if (developedOnLeft) { profile.walkwayRight=0; }
					else { profile.walkwayLeft=0; }
				}
				GeneratedStreet::apply(*edge,profile,(corridor%2)!=0);
			}
			registerGridEdge(edgeId, nodeA, nodeB);
		};

		for (int row = 0; row < rows; ++row)
		{
			for (int col = 0; col + 1 < n; ++col)
			{
				tryAddGridEdge(col, row, col + 1, row);
			}
		}
		for (int col = 0; col < n; ++col)
		{
			for (int row = 0; row + 1 < rows; ++row)
			{
				tryAddGridEdge(col, row, col, row + 1);
			}
		}

		// Removing civic/terrain blocks must not leave interior road stubs.
		// Preserve boundary vertices because they may be regional gateways.
		int prunedStubs=0;
		bool pruned=true;
		while (pruned)
		{
			pruned=false;
			for (int& id : nodeIds)
			{
				const auto* node=network.getNode(id);
				if (!node || outerGridPointSet.contains(id) || node->attachments.size()!=1) { continue; }
				const int edgeId=node->attachments.front().edgeId;
				const auto* edge=network.getEdge(edgeId);
				const int other=edge->nodeA==id ? edge->nodeB : edge->nodeA;
				gridGraph[other].remove_if([&](const GridAdjEdge& adjacency) { return adjacency.edgeId==edgeId; });
				gridGraph.erase(id);
				network.removeEdge(edgeId); network.removeNode(id); id=-1;
				++prunedStubs; pruned=true;
			}
		}
		DBG_LOG(U"[SettlementPlan] prunedInteriorStubs={}"_fmt(prunedStubs));

		Array<int> outerGridPoints;
		outerGridPoints.reserve(outerGridPointSet.size());
		for (const int nid : outerGridPointSet) outerGridPoints << nid;
		if (outerGridPoints.isEmpty()) return;

			// 5) 矩形外側の街道端を、同じ辺の城下口へ接続
			// 6) 接続した格子点リストを保持
			Array<int> connectedOuterGridPoints;
			HashSet<int> connectedOuterSet;
			Array<int> newlyAddedArterials;
			for (const int arterialNodeId : arterialWorkNodes)
			{
				const RoadNode* src = network.getNode(arterialNodeId);
				if (!src) continue;

			int bestOuterNode = -1;
			const Vec2 sourceLocal{src->position.x-settlement.center.x,src->position.z-settlement.center.y};
			const double sx=sourceLocal.dot(axisX),sz=sourceLocal.dot(axisZ);
			const bool onXSide=Abs(sx)/(halfExtent.x+100)>Abs(sz)/(halfExtent.y+100);
			// Prefer an interior gateway. A compact temple town may only have a corner
			// gateway on the shrine side; connect there through the exterior buffer.
			for (int pass=0;pass<2 && bestOuterNode<0;++pass)
			{
				double bestDistSq=1e300;
				for (const int outerNodeId : outerGridPoints)
				{
					const RoadNode* dst=network.getNode(outerNodeId);
					if (!dst || dst->attachments.isEmpty() || connectedOuterSet.contains(outerNodeId)) { continue; }
					const Vec2 local{dst->position.x-settlement.center.x,dst->position.z-settlement.center.y};
					const double tx=local.dot(axisX),tz=local.dot(axisZ);
					if (onXSide ? (Abs(tx)<halfExtent.x-.1 || tx*sx<=0)
						: (Abs(tz)<halfExtent.y-.1 || tz*sz<=0)) { continue; }
					if (pass==0 && (onXSide ? Abs(tz)>halfExtent.y-GenerationSettings::get().districtRoads_gatewayCornerSetback : Abs(tx)>halfExtent.x-GenerationSettings::get().districtRoads_gatewayCornerSetback)) { continue; }
					const double distance=src->position.distanceFromSq(dst->position);
					if (distance<bestDistSq) { bestDistSq=distance; bestOuterNode=outerNodeId; }
				}
			}
			if (bestOuterNode < 0) continue;

			bool connected = false;
			const Vec3 start = src->position;
			const Vec3 end = network.getNode(bestOuterNode)->position;
			const Vec2 inward=onXSide ? axisX*(sx>0 ? -1.0 : 1.0) : axisZ*(sz>0 ? -1.0 : 1.0);
			const Vec3 tangent{ inward.x, 0, inward.y };
			const double handle = Min(GenerationSettings::get().districtRoads_gatewayMaximumHandle, start.distanceFrom(end) / 3.0);
			if (const auto id = network.addEdge(arterialNodeId, bestOuterNode,
				start + (end-start)/3.0, end-tangent*handle, plan.scale==2 ? RoadType::LocalRoad : RoadType::Arterial, plan.scale==0 ? 4 : 2))
			{
				GeneratedStreet::apply(*network.getEdge(*id),GeneratedStreet::describe(plan.scale==0 && Max(halfExtent.x,halfExtent.y)>GenerationSettings::get().districtRoads_mainArterialTownSize ? GeneratedStreet::Role::MainArterial
					: (plan.scale==2 ? GeneratedStreet::Role::Regional : GeneratedStreet::Role::Collector)));
				newlyAddedArterials << *id;
				connected = true;
			}

			if (connected && !connectedOuterSet.contains(bestOuterNode))
			{
				connectedOuterSet.insert(bestOuterNode);
				connectedOuterGridPoints << bestOuterNode;
			}
		}

		// 7) 接続格子点ペアを格子内探索（曲がりペナルティ）して経路を幹線化
		
		HashSet<int> arterialGridEdges;
		for (size_t i = 0; i < Min(size_t{1},connectedOuterGridPoints.size()); ++i)
		{
			for (size_t j = i + 1; j < connectedOuterGridPoints.size(); ++j)
			{
				const int startNodeId = connectedOuterGridPoints[i];
				const int goalNodeId = connectedOuterGridPoints[j];
				const Array<int> edgePath = findGridPathTurnPenalty(
					network, startNodeId, goalNodeId, gridGraph, GenerationSettings::get().districtRoads_turnPenalty);
				for (const int edgeId : edgePath)
				{
					arterialGridEdges.insert(edgeId);
				}
			}
		}

		HashSet<int> widenedRows, widenedColumns;
		for (int row=0;row<rows;++row)
		{
			for (int col=0;col<n;++col)
			{
				if (col+1<n && arterialGridEdges.contains(findEdgeBetweenNodes(network,nodeIds[{col,row}],nodeIds[{col+1,row}]))) { widenedRows.insert(row); }
				if (row+1<rows && arterialGridEdges.contains(findEdgeBetweenNodes(network,nodeIds[{col,row}],nodeIds[{col,row+1}]))) { widenedColumns.insert(col); }
			}
		}
		for (int row=0;row<rows;++row)
		{
			for (int col=0;col<n;++col)
			{
				const auto widen = [&](int a,int b,bool perimeter)
				{
					const int edgeId=findEdgeBetweenNodes(network,a,b);
					if (auto* edge=network.getEdge(edgeId))
					{
						if (!perimeter) { GeneratedStreet::apply(*edge,GeneratedStreet::describe(plan.scale==2 ? GeneratedStreet::Role::Regional
						: (plan.scale!=0 || Max(halfExtent.x,halfExtent.y)<=GenerationSettings::get().districtRoads_mainArterialTownSize ? GeneratedStreet::Role::Collector : GeneratedStreet::Role::MainArterial))); }
						newlyAddedArterials << edgeId;
					}
				};
				if (col+1<n && widenedRows.contains(row)) { widen(nodeIds[{col,row}],nodeIds[{col+1,row}],row==0 || row==rows-1); }
				if (row+1<rows && widenedColumns.contains(col)) { widen(nodeIds[{col,row}],nodeIds[{col,row+1}],col==0 || col==n-1); }
			}
		}

		// Route 再付与: 新規幹線辺に隣接 Route を継承
		HashSet<int> uniqueArterialEdges;
		for (const int eid : newlyAddedArterials) uniqueArterialEdges.insert(eid);
		for (const int eid : uniqueArterialEdges)
		{
			reattachRoutesFromNeighbors(network, eid);
		}

		// Split deep blocks with a 4.4 m access lane. Shared midpoints preserve topology.
		HashTable<int64,int> midpoints;
		const auto midpointKey = [](int a,int b) { return static_cast<int64>(Min(a,b))*0x100000000LL+Max(a,b); };
		const auto hasSide = [&](int a,int b) { return a>=0 && b>=0 && (midpoints.contains(midpointKey(a,b)) || findEdgeBetweenNodes(network,a,b)>=0); };
		const auto midpoint = [&](int a,int b)
		{
			const int64 key=midpointKey(a,b);
			if (midpoints.contains(key)) { return midpoints[key]; }
			const int id=network.splitEdgeAtParameter(findEdgeBetweenNodes(network,a,b),.5f);
			midpoints[key]=id; return id;
		};
		int alleyCount=0;
		for (int row=0;row+1<rows;++row)
		{
			for (int col=0;col+1<n;++col)
			{
				const float width=coordsX[col+1]-coordsX[col],depth=coordsZ[row+1]-coordsZ[row];
				if (plan.origin==UrbanMorphology::Origin::Planned || Max(width,depth)<GenerationSettings::get().districtRoads_alleyMinimumBlockLength || Min(width,depth)<GenerationSettings::get().districtRoads_alleyMinimumBlockDepth) { continue; }
				const int a=nodeIds[{col,row}],b=nodeIds[{col+1,row}],c=nodeIds[{col+1,row+1}],d=nodeIds[{col,row+1}];
				if (!hasSide(a,b) || !hasSide(b,c) || !hasSide(c,d) || !hasSide(d,a)) { continue; }
				const bool alongZ=width>=depth;
				const Vec2 start=alongZ ? Vec2{(coordsX[col]+coordsX[col+1])*.5,coordsZ[row]} : Vec2{coordsX[col],(coordsZ[row]+coordsZ[row+1])*.5};
				const Vec2 end=alongZ ? Vec2{start.x,coordsZ[row+1]} : Vec2{coordsX[col+1],start.y};
				if (!UrbanMorphology::allowStreet(plan,start,end)) { continue; }
				const int startId=alongZ ? midpoint(a,b) : midpoint(a,d),endId=alongZ ? midpoint(d,c) : midpoint(b,c);
				if (startId<0 || endId<0) { continue; }
				const Vec3 from=network.getNode(startId)->position,to=network.getNode(endId)->position;
				if (const auto id=network.addEdge(startId,endId,from+(to-from)/3,to-(to-from)/3,RoadType::LocalRoad,2))
				{
					auto profile=GeneratedStreet::describe(GeneratedStreet::Role::FarmAccess); profile.laneWidth=GenerationSettings::get().districtRoads_alleyLaneWidth; profile.shoulder=GenerationSettings::get().districtRoads_alleyShoulderWidth;
					GeneratedStreet::apply(*network.getEdge(*id),profile);
					++alleyCount;
				}
			}
		}
		if (plan.origin!=UrbanMorphology::Origin::Planned) { SettlementFringe::generate(settlement,world,network); }
		NewTownLayout::finish(settlement,network);
		if (plan.scale==0)
		{
			HashSet<int> wide,reviewed;
			for (const auto& edge:network.edges())
			{
				if (edge.id<0 || edge.designGrade || edge.lanes.size()<4) { continue; }
				const auto* a=network.getNode(edge.nodeA);
				const auto* b=network.getNode(edge.nodeB);
				if (!a || !b) { continue; }
				const Vec2 edgeMidpoint{(a->position.x+b->position.x)*.5-settlement.center.x,
					(a->position.z+b->position.z)*.5-settlement.center.y};
				if (Abs(edgeMidpoint.dot(axisX))<halfExtent.x+250 && Abs(edgeMidpoint.dot(axisZ))<halfExtent.y+250) { wide.insert(edge.id); }
			}
			int shortRuns=0,downgraded=0;
			for (const int seedEdge:wide)
			{
				if (reviewed.contains(seedEdge)) { continue; }
				Array<int> component{seedEdge};
				reviewed.insert(seedEdge);
				double length=0;
				for (size_t cursor=0;cursor<component.size();++cursor)
				{
					const auto* edge=network.getEdge(component[cursor]);
					if (!edge) { continue; }
					if (const auto curve=network.getBezier(edge->id)) { length+=curve->totalLength; }
					for (const int nodeId:{edge->nodeA,edge->nodeB})
					{
						const auto* node=network.getNode(nodeId);
						if (!node) { continue; }
						for (const auto& attachment:node->attachments)
						{
							if (!wide.contains(attachment.edgeId) || reviewed.contains(attachment.edgeId)) { continue; }
							reviewed.insert(attachment.edgeId);
							component << attachment.edgeId;
						}
					}
				}
				if (length>=240) { continue; }
				++shortRuns;
				for (const int id:component)
				{
					if (auto* edge=network.getEdge(id))
					{
						GeneratedStreet::apply(*edge,GeneratedStreet::describe(GeneratedStreet::Role::Local));
						++downgraded;
					}
				}
			}
			DBG_LOG(U"[ShortArterialReview] town={} shortRuns={} downgraded={}"_fmt(settlementIndex,shortRuns,downgraded));
		}
		DBG_LOG(U"[BlockAlleys] town={} alleys={}"_fmt(settlementIndex,alleyCount));
		DBG_LOG(U"[SettlementPlan] index={} origin={} scale={} extent=({}, {}) nodes={} entrances={} connected={} station={}"_fmt(
			settlementIndex,UrbanMorphology::originName(plan.origin),plan.scale,halfExtent.x,halfExtent.y,n*rows,arterialWorkNodes.size(),connectedOuterGridPoints.size(),plan.station.has_value()));
	}

	void generateCastleTown(uint64 seed,int index,MapGenerator::Settlement& settlement,const KaidoSegment& kaido,const World& world,RoadNetwork& network)
	{
		generateSettlement(seed,index,settlement,kaido,world,network);
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
				const Vec2 delta = b - a;
				const Vec2 localA = a-frame.center, localB = b-frame.center;
				const bool inside = Abs(localA.dot(frame.axisX))<=frame.halfExtent.x+0.1 && Abs(localA.dot(frame.axisZ))<=frame.halfExtent.y+0.1
					&& Abs(localB.dot(frame.axisX))<=frame.halfExtent.x+0.1 && Abs(localB.dot(frame.axisZ))<=frame.halfExtent.y+0.1;
				if (inside && Min(Abs(delta.dot(frame.axisX)),Abs(delta.dot(frame.axisZ))) < 0.01)
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
			DebugLog::print(U"[straighten] target={} (LocalRoad={})"_fmt(targetEdgeIds.size(), localCount));
			if (!frames.isEmpty())
				DebugLog::print(U"[straighten] frame axisX=({:.3f},{:.3f})"_fmt(frames[0].axisX.x, frames[0].axisX.y));
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


	}

}
