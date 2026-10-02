#include "VillageConnections.hpp"
#include "RoadAlignment.hpp"
#include "RoadAutoPlace.hpp"
#include "RoadConstructionCost.hpp"
#include "RoadNodeIndex.hpp"
#include "StreetProfile.hpp"
#include "../debug/DebugLog.hpp"
#include <numeric>
#include <queue>

namespace
{
	using Settlement = MapGenerator::Settlement;
	using Kind = MapGenerator::SettlementKind;
	constexpr int kTerrainSamples = 32;
	constexpr int kAccessCandidates = 3;
	constexpr int kMaximumAttachments = 6;
	constexpr double kMinimumDistance = 2;
	constexpr double kAccessSearchScale = 3;
	constexpr double kMinimumAccessRadius = 120;

	bool roadLane(const Lane& lane)
	{
		return lane.type != LaneType::Rail && lane.type != LaneType::KeepOut && lane.type != LaneType::TrafficIsland
			&& (lane.op == OpState::Open || lane.op == OpState::Provisional);
	}
	bool usableRoad(const RoadEdge& edge)
	{
		return edge.id >= 0 && edge.edgeState != EdgeState::Closed && edge.edgeState != EdgeState::UnderConstruction
			&& edge.isRoadbedBuilt() && std::isfinite(edge.length) && edge.length > 0 && edge.lanes.any(roadLane);
	}
	uint64 pairKey(size_t a, size_t b)
	{
		if (a > b) { std::swap(a, b); }
		return (static_cast<uint64>(a) << 32) | static_cast<uint32>(b);
	}
	double accessRadius(const Settlement& place)
	{
		return Min(GenerationSettings::get().network_regionalAccessRadius,
			Max(kMinimumAccessRadius, static_cast<double>(place.radius) * .5));
	}
	struct Access
	{
		Optional<int> id;
		Vec3 position;
		double offset = 0;
	};
	RoadNodeIndex makeAccessIndex(const RoadNetwork& roads)
	{
		RoadNodeIndex result;
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0) { continue; }
			for (const auto& item : node.attachments)
			{
				const auto* edge = roads.getEdge(item.edgeId);
				if (edge && usableRoad(*edge)) { result.insert(node.position, node.id); break; }
			}
		}
		return result;
	}
	Access primaryAccess(const Settlement& place, const World& world, const RoadNetwork& roads, const RoadNodeIndex& index)
	{
		Access result{none, {place.center.x, world.sampleHeight(static_cast<float>(place.center.x), static_cast<float>(place.center.y)), place.center.y}, 0};
		result.id = index.findBest(result.position, roads, static_cast<float>(accessRadius(place)), [](const RoadNode&, float distance) -> Optional<double> { return distance; });
		if (result.id)
		{
			result.position = roads.getNode(*result.id)->position;
			result.offset = place.center.distanceFrom({result.position.x, result.position.z});
		}
		return result;
	}
	struct Candidate
	{
		size_t a = 0, b = 0;
		double direct = 0, terrainCost = 0;
	};
	/// @brief 直線の標本は順位付けだけに使い、峠や橋の成立判断は最終線形探索へ任せる。
	double terrainEstimate(Vec2 a, Vec2 b, const World& world)
	{
		const double step = a.distanceFrom(b) / kTerrainSamples;
		if (step < kMinimumDistance / kTerrainSamples) { return Math::Inf; }
		const auto& config = GenerationSettings::get();
		double previous = world.sampleHeight(static_cast<float>(a.x), static_cast<float>(a.y));
		double cost = 0;
		for (int i = 1; i <= kTerrainSamples; ++i)
		{
			const Vec2 point = a.lerp(b, static_cast<double>(i) / kTerrainSamples);
			const double height = world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.y));
			const double slope = Abs(height - previous) / step;
			const bool wet = height < world.waterSurfaceHeight(point.x, point.y) + config.crossings_waterBankMargin;
			cost += step * (1 + Min(64.0, Square(slope / config.roads_localMaximumGrade))
				+ (wet ? RoadConstructionCost::Earthwork() : 0));
			previous = height;
		}
		return std::isfinite(cost) ? cost : Math::Inf;
	}
	HashTable<int, int> roadComponents(const RoadNetwork& roads);
	Array<Candidate> nearbyCandidates(const Array<Settlement>& settlements, const World& world, const RoadNetwork& roads)
	{
		const auto& config = GenerationSettings::get();
		Array<Candidate> result;
		HashSet<uint64> used;
		Array<Access> access;
		const auto accessIndex = makeAccessIndex(roads);
		for (const auto& place : settlements) { access << primaryAccess(place, world, roads, accessIndex); }
		const auto components = roadComponents(roads);
		const auto component = [&](size_t index)
		{
			return access[index].id && components.contains(*access[index].id) ? components.at(*access[index].id) : -static_cast<int>(index)-1;
		};
		HashTable<int, Candidate> repairs;
		const auto append = [&](const Candidate& candidate)
		{
			if (used.insert(pairKey(candidate.a, candidate.b)).second) { result << candidate; }
		};
		for (size_t a = 0; a < settlements.size(); ++a)
		{
			Array<std::pair<double, size_t>> nearby;
			for (size_t b = 0; b < settlements.size(); ++b)
			{
				if (a == b) { continue; }
				const double distance = settlements[a].center.distanceFrom(settlements[b].center);
				if (distance >= kMinimumDistance && distance <= config.network_regionalNeighborRadius) { nearby.emplace_back(distance, b); }
			}
			nearby.sort();
			const size_t pool = Min(nearby.size(), static_cast<size_t>(config.network_regionalNeighbors * 3));
			Array<Candidate> ranked;
			for (size_t i = 0; i < pool; ++i)
			{
				const auto [distance, b] = nearby[i];
				ranked << Candidate{Min(a, b), Max(a, b), distance, terrainEstimate(settlements[a].center, settlements[b].center, world)};
			}
			ranked.sort_by([](const Candidate& a, const Candidate& b)
			{
				return a.terrainCost != b.terrainCost ? a.terrainCost < b.terrainCost : pairKey(a.a, a.b) < pairKey(b.a, b.b);
			});
			const size_t count = Min(ranked.size(), static_cast<size_t>(config.network_regionalNeighbors));
			for (size_t i = 0; i < count; ++i)
			{
				append(ranked[i]);
			}
			// 密集した近隣村だけで候補枠を使い切らず、町の同格近隣と別成分への修復候補を確保する。
			Optional<Candidate> townPeer, repair;
			int townChecks = 0, repairChecks = 0;
			for (const auto& [distance, b] : nearby)
			{
				const bool peer = settlements[a].kind != Kind::RuralSettlement && settlements[b].kind != Kind::RuralSettlement && townChecks < kAccessCandidates;
				const bool separate = component(a) != component(b) && repairChecks < kAccessCandidates;
				if (!peer && !separate) { continue; }
				const Candidate candidate{Min(a,b), Max(a,b), distance, terrainEstimate(settlements[a].center, settlements[b].center, world)};
				if (peer) { ++townChecks; if (!townPeer || candidate.terrainCost < townPeer->terrainCost) { townPeer = candidate; } }
				if (separate) { ++repairChecks; if (!repair || candidate.terrainCost < repair->terrainCost) { repair = candidate; } }
			}
			if (townPeer) { append(*townPeer); }
			if (repair)
			{
				const int owner = component(a); const auto previous = repairs.find(owner);
				if (previous == repairs.end() || repair->terrainCost < previous->second.terrainCost
					|| (repair->terrainCost == previous->second.terrainCost && pairKey(repair->a,repair->b) < pairKey(previous->second.a,previous->second.b))) { repairs[owner] = *repair; }
			}
		}
		for (const auto& [owner, candidate] : repairs) { (void)owner; append(candidate); }
		result.sort_by([&](const Candidate& a, const Candidate& b)
		{
			const bool disconnectedA = component(a.a) != component(a.b), disconnectedB = component(b.a) != component(b.b);
			if (disconnectedA != disconnectedB) { return disconnectedA; }
			const bool townA = settlements[a.a].kind != Kind::RuralSettlement && settlements[a.b].kind != Kind::RuralSettlement;
			const bool townB = settlements[b.a].kind != Kind::RuralSettlement && settlements[b.b].kind != Kind::RuralSettlement;
			if (townA != townB) { return townA; }
			return a.terrainCost != b.terrainCost ? a.terrainCost < b.terrainCost : pairKey(a.a, a.b) < pairKey(b.a, b.b);
		});
		return result;
	}
	HashTable<int, int> roadComponents(const RoadNetwork& roads)
	{
		HashTable<int, int> components;
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0 || components.contains(node.id)) { continue; }
			std::queue<int> pending;
			pending.push(node.id); components[node.id] = node.id;
			while (!pending.empty())
			{
				const int id = pending.front(); pending.pop();
				for (const auto& item : roads.getNode(id)->attachments)
				{
					const auto* edge = roads.getEdge(item.edgeId);
					if (!edge || !usableRoad(*edge)) { continue; }
					const int next = edge->nodeA == id ? edge->nodeB : edge->nodeA;
					if (!roads.getNode(next) || components.contains(next)) { continue; }
					components[next] = node.id; pending.push(next);
				}
			}
		}
		return components;
	}
	bool differentComponents(const Access& a, const Access& b, const HashTable<int, int>& components)
	{
		return !a.id || !b.id || !components.contains(*a.id) || !components.contains(*b.id)
			|| components.at(*a.id) != components.at(*b.id);
	}
	double distanceLimit(const Candidate& pair)
	{
		const auto& config = GenerationSettings::get();
		return pair.direct * config.network_regionalDetourRatio * 2 + config.network_regionalAccessRadius * 2;
	}
	Array<Access> accessChoices(const Settlement& place, const Access& primary, Vec3 target, const RoadNetwork& roads, const RoadNodeIndex& index)
	{
		if (!primary.id) { return {primary}; }
		const auto& config = GenerationSettings::get();
		Array<Access> result;
		HashSet<int> examined;
		const Vec3 center{place.center.x, primary.position.y, place.center.y};
		for (int attempt = 0; attempt < kAccessCandidates * kAccessCandidates; ++attempt)
		{
			const auto choice = index.findBest(center, roads, static_cast<float>(accessRadius(place)), [&](const RoadNode& node, float distance) -> Optional<double>
			{
				if (examined.contains(node.id) || node.attachments.size() >= kMaximumAttachments) { return none; }
				return std::sqrt(distance) * 2 + Vec2{target.x - node.position.x, target.z - node.position.z}.length();
			});
			if (!choice) { break; }
			const int id = *choice; examined.insert(id);
			// 別の街路島を借りて、元の集落が接続したように見せない。
			if (id != *primary.id && (!std::isfinite(VillageConnections::shortestDistance(roads, *primary.id, id, config.network_regionalAccessRadius * kAccessSearchScale))
				|| !std::isfinite(VillageConnections::shortestDistance(roads, id, *primary.id, config.network_regionalAccessRadius * kAccessSearchScale)))) { continue; }
			const Vec3 position = roads.getNode(id)->position;
			result << Access{id, position, place.center.distanceFrom({position.x, position.z})};
			if (result.size() >= kAccessCandidates) { break; }
		}
		return result;
	}

}

namespace VillageConnections
{
	double shortestDistance(const RoadNetwork& roads, int start, int goal, double limit)
	{
		if (!roads.getNode(start) || !roads.getNode(goal) || limit < 0) { return Math::Inf; }
		if (start == goal) { return 0; }
		using Entry = std::pair<double, uint64>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
		HashTable<uint64, double> distances;
		const auto enqueue = [&](const RoadEdge& edge, int lane, int from, double cost)
		{
			if (!usableRoad(edge) || lane < 0 || lane >= static_cast<int>(edge.lanes.size())) { return; }
			const auto& value = edge.lanes[lane];
			if (!roadLane(value) || (value.dir == LaneDir::Forward ? edge.nodeA : edge.nodeB) != from) { return; }
			const double proposal = cost + edge.length;
			const uint64 key = (static_cast<uint64>(edge.id) << 32) | static_cast<uint32>(lane);
			const auto found = distances.find(key);
			if (proposal <= limit && (found == distances.end() || proposal < found->second))
			{
				distances[key] = proposal; pending.emplace(proposal, key);
			}
		};
		for (const auto& item : roads.getNode(start)->attachments)
		{
			if (const auto* edge = roads.getEdge(item.edgeId))
			{
				for (int lane = 0; lane < static_cast<int>(edge->lanes.size()); ++lane) { enqueue(*edge, lane, start, 0); }
			}
		}
		while (!pending.empty())
		{
			const auto [cost, key] = pending.top(); pending.pop();
			if (distances.at(key) < cost) { continue; }
			const int edgeId = static_cast<int>(key >> 32), lane = static_cast<int>(static_cast<uint32>(key));
			const auto* edge = roads.getEdge(edgeId);
			const int nodeId = edge->lanes[lane].dir == LaneDir::Forward ? edge->nodeB : edge->nodeA;
			if (nodeId == goal) { return cost; }
			const auto* node = roads.getNode(nodeId);
			if (!node) { continue; }
			// 同一方向の隣接車線へ道路上で移れる。通行止めや島を跨ぐ車線変更は認めない。
			int firstLane = lane, lastLane = lane;
			const auto sameDirection = [&](int index)
			{
				return index >= 0 && index < static_cast<int>(edge->lanes.size()) && roadLane(edge->lanes[index])
					&& edge->lanes[index].dir == edge->lanes[lane].dir;
			};
			while (sameDirection(firstLane - 1)) { --firstLane; }
			while (sameDirection(lastLane + 1)) { ++lastLane; }
			for (const auto& connection : node->laneConnections)
			{
				if (connection.fromEdgeId != edgeId || connection.fromLaneIndex < firstLane || connection.fromLaneIndex > lastLane) { continue; }
				if (const auto* next = roads.getEdge(connection.toEdgeId)) { enqueue(*next, connection.toLaneIndex, nodeId, cost); }
			}
		}
		return Math::Inf;
	}

	static Audit measureCandidates(const Array<Settlement>& settlements, const World& world, const RoadNetwork& roads, const Array<Candidate>& candidates)
	{
		Audit result; result.settlements = static_cast<int>(settlements.size());
		Array<Access> access;
		const auto accessIndex = makeAccessIndex(roads);
		Array<int> parent(settlements.size()); std::iota(parent.begin(), parent.end(), 0);
		const auto root = [&](int id) { while (parent[id] != id) { id = parent[id]; } return id; };
		const auto join = [&](int a, int b) { a = root(a); b = root(b); if (a != b) { parent[Max(a, b)] = Min(a, b); } };
		struct Label { double distance = Math::Inf; int owner = -1; };
		HashTable<int, Label> labels;
		using Entry = std::pair<double, int>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
		for (size_t i = 0; i < settlements.size(); ++i)
		{
			access << primaryAccess(settlements[i], world, roads, accessIndex);
			if (!access.back().id) { continue; }
			++result.accessNodes;
			const int id = *access.back().id;
			if (labels.contains(id)) { join(labels.at(id).owner, static_cast<int>(i)); }
			else { labels[id] = {0, static_cast<int>(i)}; pending.emplace(0, id); }
		}
		HashSet<int> initialOwners;
		for (size_t i = 0; i < settlements.size(); ++i) { initialOwners.insert(root(static_cast<int>(i))); }
		result.compressedNodes = static_cast<int>(initialOwners.size());
		while (!pending.empty())
		{
			const auto [distance, id] = pending.top(); pending.pop();
			const Label current = labels.at(id);
			if (distance > current.distance) { continue; }
			for (const auto& item : roads.getNode(id)->attachments)
			{
				const auto* edge = roads.getEdge(item.edgeId);
				if (!edge || !usableRoad(*edge)) { continue; }
				const int next = edge->nodeA == id ? edge->nodeB : edge->nodeA;
				if (!roads.getNode(next)) { continue; }
				const double proposal = distance + edge->length;
				const auto found = labels.find(next);
				if (found == labels.end() || proposal < found->second.distance
					|| (proposal == found->second.distance && current.owner < found->second.owner))
				{
					labels[next] = {proposal, current.owner}; pending.emplace(proposal, next);
				}
			}
		}
		HashTable<uint64, Array<Vec2>> boundaries;
		for (const auto& edge : roads.edges())
		{
			if (!usableRoad(edge) || !labels.contains(edge.nodeA) || !labels.contains(edge.nodeB)) { continue; }
			const auto a = labels.at(edge.nodeA), b = labels.at(edge.nodeB);
			if (a.owner == b.owner) { continue; }
			const auto curve = roads.getBezier(edge.id);
			if (!curve) { continue; }
			const Vec3 point = curve->positionAt(static_cast<float>(Clamp((edge.length + b.distance - a.distance) * .5, 0.0, static_cast<double>(edge.length))));
			boundaries[pairKey(a.owner, b.owner)] << Vec2{point.x, point.z};
		}
		// 同じ2集落の異なる回廊は並列リンクとして残す。近接した街路境界の束だけをまとめる。
		const double mergeDistance = GenerationSettings::get().network_regionalCorridorMergeDistance;
		for (const auto& [key, points] : boundaries)
		{
			join(static_cast<int>(key >> 32), static_cast<int>(static_cast<uint32>(key)));
			Array<bool> seen(points.size(), false);
			for (size_t first = 0; first < points.size(); ++first)
			{
				if (seen[first]) { continue; }
				++result.links; seen[first] = true;
				Array<size_t> pendingPoints{first};
				while (!pendingPoints.isEmpty())
				{
					const size_t current = pendingPoints.back(); pendingPoints.pop_back();
					for (size_t next = 0; next < points.size(); ++next)
					{
						if (!seen[next] && points[current].distanceFromSq(points[next]) <= Square(mergeDistance))
						{
							seen[next] = true; pendingPoints << next;
						}
					}
				}
			}
		}
		HashSet<int> components;
		for (size_t i = 0; i < settlements.size(); ++i) { components.insert(root(static_cast<int>(i))); }
		result.components = static_cast<int>(components.size());
		result.cycles = Max(0, result.links - result.compressedNodes + result.components);
		const size_t count = Min(candidates.size(), static_cast<size_t>(GenerationSettings::get().network_regionalCandidateLimit));
		for (size_t i = 0; i < count; ++i)
		{
			const auto& pair = candidates[i]; const auto& a = access[pair.a]; const auto& b = access[pair.b];
			++result.nearbyPairs;
			const double forward = a.id && b.id ? shortestDistance(roads, *a.id, *b.id, distanceLimit(pair)) : Math::Inf;
			const double backward = a.id && b.id ? shortestDistance(roads, *b.id, *a.id, distanceLimit(pair)) : Math::Inf;
			if (!std::isfinite(forward) || !std::isfinite(backward)) { ++result.unreachablePairs; continue; }
			++result.reachablePairs;
			const double ratio = (Max(forward, backward) + a.offset + b.offset) / pair.direct;
			result.maximumDetour = Max(result.maximumDetour, ratio); result.meanDetour += ratio;
		}
		if (result.reachablePairs > 0) { result.meanDetour /= result.reachablePairs; }
		return result;
	}

	Audit measure(const Array<Settlement>& settlements, const World& world, const RoadNetwork& roads)
	{
		return measureCandidates(settlements, world, roads, nearbyCandidates(settlements, world, roads));
	}

	Result improve(uint64 seed, const Array<Settlement>& settlements, const World& world, RoadNetwork& roads)
	{
		Result result;
		if (settlements.size() < 2) { return result; }
		const auto& config = GenerationSettings::get();
		// 地区生成の断面変更を、実際の双方向車線遷移へ反映してから評価する。
		for (const auto& node : roads.nodes()) { if (node.id >= 0) { roads.rebuildLaneConnections(node.id); } }
		auto candidates = nearbyCandidates(settlements, world, roads);
		result.candidates = static_cast<int>(candidates.size());
		const size_t selected = Min(candidates.size(), static_cast<size_t>(config.network_regionalCandidateLimit));
		candidates.resize(selected);
		result.budgetDeferred = result.candidates - static_cast<int>(selected);
		result.before = measureCandidates(settlements, world, roads, candidates);
		auto components = roadComponents(roads);
		auto accessIndex = makeAccessIndex(roads);
		Array<int> additions(settlements.size(), 0);
		int examined = 0;
		for (const auto& pair : candidates)
		{
			if (examined++ >= config.network_regionalCandidateLimit || result.trials >= config.network_regionalRoutingAttemptLimit)
			{
				result.budgetDeferred = result.candidates - examined + 1; break;
			}
			const auto first = primaryAccess(settlements[pair.a], world, roads, accessIndex), second = primaryAccess(settlements[pair.b], world, roads, accessIndex);
			if (first.id && second.id && *first.id == *second.id) { continue; }
			const bool disconnected = differentComponents(first, second, components);
			if (!disconnected && (additions[pair.a] >= config.network_regionalExtraLinks || additions[pair.b] >= config.network_regionalExtraLinks)) { continue; }
			const double limit = distanceLimit(pair);
			const double oldForward = first.id && second.id ? shortestDistance(roads, *first.id, *second.id, limit) : Math::Inf;
			const double oldBackward = first.id && second.id ? shortestDistance(roads, *second.id, *first.id, limit) : Math::Inf;
			const double oldWorst = Max(oldForward, oldBackward);
			if (std::isfinite(oldWorst) && oldWorst + first.offset + second.offset < pair.direct * config.network_regionalDetourRatio) { continue; }
			++result.detours;
			const auto fromChoices = accessChoices(settlements[pair.a], first, second.position, roads, accessIndex);
			const auto toChoices = accessChoices(settlements[pair.b], second, first.position, roads, accessIndex);
			const bool prefectural = settlements[pair.a].kind != Kind::RuralSettlement || settlements[pair.b].kind != Kind::RuralSettlement;
			RoadEdge road; GeneratedStreet::apply(road, GeneratedStreet::describe(prefectural ? GeneratedStreet::Role::Regional : GeneratedStreet::Role::Local));
			Array<std::pair<double, std::pair<size_t, size_t>>> endpoints;
			for (size_t a = 0; a < fromChoices.size(); ++a) for (size_t b = 0; b < toChoices.size(); ++b)
			{
				endpoints.emplace_back(fromChoices[a].position.distanceFrom(toChoices[b].position)
					+ 2 * (fromChoices[a].offset + toChoices[b].offset), std::pair{a, b});
			}
			endpoints.sort();
			bool accepted = false;
			for (size_t attempt = 0; attempt < Min(endpoints.size(), static_cast<size_t>(kAccessCandidates)); ++attempt)
			{
				if (result.trials >= config.network_regionalRoutingAttemptLimit) { break; }
				const auto& from = fromChoices[endpoints[attempt].second.first];
				const auto& to = toChoices[endpoints[attempt].second.second];
				if (from.position.distanceFrom(to.position) < kMinimumDistance) { continue; }
				++result.trials;
				const auto alignment = RoadAlignment::find(world, from.position, to.position, road.roadType,
					config.routing_generatedExpansionLimit, TransportMode::Road, config.roads_maximumGeneratedViaductHeight);
				if (!alignment || !std::isfinite(alignment->cost) || alignment->cost > pair.direct * config.network_regionalMaximumCostRatio) { continue; }
				RoadNetwork proposed = roads;
				// 孤立集落には本物の端点を作り、失敗時はコピーごと破棄する。
				const auto anchor = [&](const Access& access)
				{
					if (access.id) { return *access.id; }
					if (const auto existing = proposed.findNodeNear(access.position, .25f);
						existing && proposed.getNode(*existing)->position.distanceFrom(access.position) < .5) { return *existing; }
					return proposed.addNode(access.position);
				};
				const int start = anchor(first), goal = anchor(second);
				const int firstNewEdgeId = proposed.nextEdgeId();
				auto added = RoadAutoPlace::buildAlignment(proposed, world, alignment->curves, road, .5f);
				if (added.isEmpty()) { continue; }
				for (const int id : added) { proposed.getEdge(id)->edgeState = EdgeState::Existing; }
				proposed.resolveIntersections(*std::min_element(added.begin(), added.end()), &added);
				Array<CubicBezier> finalCurves;
				bool legal = true;
				// 交差点挿入で分割・移動した既存国道の子エッジも、元の道路種別で再検証する。
				for (const auto& affected : proposed.edges())
				{
					if (affected.id < firstNewEdgeId) { continue; }
					const auto curve = proposed.getBezier(affected.id);
					if (!curve || !RoadAlignment::respectsLimits(*curve, affected.roadType)
						|| !std::isfinite(RoadAlignment::constructionCost(world, {*curve}, affected.roadType))
						|| RoadAlignment::maximumClearance(world, {*curve}) > config.roads_maximumGeneratedViaductHeight)
					{
						legal = false; break;
					}
				}
				for (const int id : added)
				{
					const auto* edge = proposed.getEdge(id); const auto curve = proposed.getBezier(id);
					if (!edge || !curve || !RoadAlignment::respectsLimits(*curve, edge->roadType)) { legal = false; break; }
					finalCurves << *curve;
				}
				const double finalCost = legal ? RoadAlignment::constructionCost(world, finalCurves, road.roadType) : Math::Inf;
				if (!legal || finalCurves.isEmpty() || !std::isfinite(finalCost) || finalCost > pair.direct * config.network_regionalMaximumCostRatio
					|| RoadAlignment::maximumClearance(world, finalCurves) > config.roads_maximumGeneratedViaductHeight) { continue; }
				const double forward = shortestDistance(proposed, start, goal, limit), backward = shortestDistance(proposed, goal, start, limit);
				const double worst = Max(forward, backward);
				if (!std::isfinite(worst) || forward > oldForward || backward > oldBackward) { continue; }
				const double baseline = std::isfinite(oldWorst) ? oldWorst : limit;
				if (!disconnected && (baseline - worst < config.network_regionalMinimumSavedDistance
					|| worst > baseline * (1 - config.network_regionalMinimumSavingRatio))) { continue; }
				if (prefectural)
				{
					constexpr int kRouteNumbers = 400;
					HashSet<int> usedNumbers;
					for (const auto& route : proposed.routes())
					{
						if (route.id >= 0 && route.kind == RoadRouteKind::PrefectureRoute) { usedNumbers.insert(route.number); }
					}
					const int firstNumber = static_cast<int>((seed ^ pairKey(pair.a, pair.b)) % kRouteNumbers);
					int number = 0;
					for (int offset = 0; offset < kRouteNumbers; ++offset)
					{
						const int candidate = 1 + (firstNumber + offset) % kRouteNumbers;
						if (!usedNumbers.contains(candidate)) { number = candidate; break; }
					}
					proposed.addRoute(RoadRouteKind::PrefectureRoute, number > 0 ? U"県道{}号"_fmt(number) : U"地域連絡道", added, number);
				}
				roads = std::move(proposed); ++result.connected; result.repairedComponents += disconnected;
				++additions[pair.a]; ++additions[pair.b]; components = roadComponents(roads); accessIndex = makeAccessIndex(roads);
				DBG_LOG(U"[RegionalConnection] from={} to={} disconnected={} directM={:.1f} beforeM={} afterM={:.1f} cost={:.1f} prefectural={}"_fmt(
					pair.a, pair.b, disconnected, pair.direct, oldWorst, worst, alignment->cost, prefectural));
				accepted = true; break;
			}
			if (!accepted)
			{
				++result.failed;
				DBG_LOG(U"[RegionalConnectionBlocked] from={} to={} directM={:.1f} disconnected={}"_fmt(pair.a, pair.b, pair.direct, disconnected));
			}
		}
		result.after = measureCandidates(settlements, world, roads, candidates);
		DBG_LOG(U"[RegionalConnections] candidates={} trials={} connected={} repaired={} failed={} deferred={} components={}->{} links={}->{} cycles={}->{} nearReachable={}/{}->{} nearUnreachable={}->{} maximumDetour={:.2f}->{:.2f}"_fmt(
			result.candidates, result.trials, result.connected, result.repairedComponents, result.failed, result.budgetDeferred,
			result.before.components, result.after.components, result.before.links, result.after.links, result.before.cycles, result.after.cycles,
			result.before.reachablePairs, result.before.nearbyPairs, result.after.reachablePairs, result.before.unreachablePairs, result.after.unreachablePairs,
			result.before.maximumDetour, result.after.maximumDetour));
		return result;
	}
} // namespace VillageConnections
