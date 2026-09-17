#include "VillageConnections.hpp"
#include "RoadAlignment.hpp"
#include "RoadAutoPlace.hpp"
#include "RoadNodeIndex.hpp"
#include "StreetProfile.hpp"
#include "../debug/DebugLog.hpp"
#include <queue>
#include <random>

namespace VillageConnections
{
	double shortestDistance(const RoadNetwork& roads, int start, int goal, double limit)
	{
		if (!roads.getNode(start) || !roads.getNode(goal))
		{
			return Math::Inf;
		}
		using Entry = std::pair<double, int>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
		HashTable<int, double> distance;
		distance[start] = 0;
		pending.emplace(0, start);
		while (!pending.empty())
		{
			const auto [cost, id] = pending.top();
			pending.pop();
			if (cost > limit)
			{
				break;
			}
			if (distance.at(id) < cost)
			{
				continue;
			}
			if (id == goal)
			{
				return cost;
			}
			for (const auto& attachment : roads.getNode(id)->attachments)
			{
				const auto* edge = roads.getEdge(attachment.edgeId);
				if (!edge || !edge->hasRoadLanes())
				{
					continue;
				}
				const bool forward = edge->nodeA == id;
				bool allowed = false;
				for (const auto& lane : edge->lanes)
				{
					allowed |= lane.type != LaneType::Rail && lane.type != LaneType::KeepOut &&
							   lane.type != LaneType::TrafficIsland && (lane.dir == LaneDir::Forward) == forward;
				}
				if (!allowed)
				{
					continue;
				}
				const int next = forward ? edge->nodeB : edge->nodeA;
				const double proposal = cost + edge->length;
				const auto found = distance.find(next);
				if (proposal <= limit && (found == distance.end() || proposal < found->second))
				{
					distance[next] = proposal;
					pending.emplace(proposal, next);
				}
			}
		}
		return Math::Inf;
	}

	Result improve(
		uint64 seed, const Array<MapGenerator::Settlement>& settlements, const World& world, RoadNetwork& roads)
	{
		Array<Vec2> villages;
		for (const auto& place : settlements)
		{
			if (place.kind == MapGenerator::SettlementKind::RuralSettlement)
			{
				villages << place.center;
			}
		}
		Result result;
		if (villages.size() < 2)
		{
			return result;
		}
		const auto& config = GenerationSettings::get();
		std::mt19937_64 random{seed ^ 0x6A59DE771ULL};
		result.trials = std::uniform_int_distribution<int>{
			config.network_villageShortcutMinimumTrials, config.network_villageShortcutMaximumTrials}(random);
		HashSet<uint64> attempted;
		for (int trial = 0; trial < result.trials; ++trial)
		{
			const size_t a = static_cast<size_t>(random() % villages.size());
			size_t b = a;
			double direct = Math::Inf;
			for (size_t other = 0; other < villages.size(); ++other)
			{
				if (other == a)
				{
					continue;
				}
				const double length = villages[a].distanceFrom(villages[other]);
				if (length < direct)
				{
					direct = length;
					b = other;
				}
			}
			const uint64 key = (static_cast<uint64>(a) << 32) | static_cast<uint32>(b);
			if (direct < 1 || !attempted.insert(key).second)
			{
				continue;
			}
			// 内部街路生成で村の中心ノードが変わっても、実際に道路へ接続する近傍点を使う。
			const auto access = [&](Vec2 place) -> Optional<int>
			{
				Optional<int> nearest;
				double best = Square(1000.0);
				for (const auto& node : roads.nodes())
				{
					if (node.id < 0 || node.attachments.isEmpty())
					{
						continue;
					}
					bool road = false;
					for (const auto& item : node.attachments)
					{
						const auto* edge = roads.getEdge(item.edgeId);
						road |= edge && edge->hasRoadLanes();
					}
					const double distance = place.distanceFromSq({node.position.x, node.position.z});
					if (road && distance < best)
					{
						best = distance;
						nearest = node.id;
					}
				}
				return nearest;
			};
			const auto start = access(villages[a]), goal = access(villages[b]);
			if (!start || !goal || *start == *goal)
			{
				continue;
			}
			const Vec3 from = roads.getNode(*start)->position, to = roads.getNode(*goal)->position;
			const double accessLength =
				villages[a].distanceFrom({from.x, from.z}) + villages[b].distanceFrom({to.x, to.z});
			const double limit = direct * config.network_villageDetourRatio;
			if (shortestDistance(roads, *start, *goal, limit) + accessLength < limit)
			{
				continue;
			}
			++result.detours;
			const auto alignment = RoadAlignment::find(world, from, to, RoadType::LocalRoad);
			if (!alignment)
			{
				continue;
			}
			double length = accessLength;
			for (const auto& curve : alignment->curves)
			{
				length += curve.totalLength;
			}
			const double oldLength = shortestDistance(roads, *start, *goal);
			if (length >= oldLength + accessLength)
			{
				continue;
			}
			RoadNetwork proposed = roads;
			RoadEdge road;
			GeneratedStreet::apply(road, GeneratedStreet::describe(GeneratedStreet::Role::Local));
			const auto added = RoadAutoPlace::buildAlignment(proposed, world, alignment->curves, road, .5f);
			if (added.isEmpty())
			{
				continue;
			}
			for (int id : added)
			{
				proposed.getEdge(id)->edgeState = EdgeState::Existing;
			}
			proposed.resolveIntersections(*std::min_element(added.begin(), added.end()));
			const double newLength = shortestDistance(proposed, *start, *goal);
			if (newLength >= oldLength)
			{
				continue;
			}
			roads = std::move(proposed);
			++result.connected;
			DBG_LOG(U"[VillageShortcut] from={} to={} directM={:.1f} beforeM={:.1f} afterM={:.1f}"_fmt(
				a, b, direct, oldLength, newLength));
		}
		DBG_LOG(U"[VillageShortcuts] trials={} detours={} connected={}"_fmt(
			result.trials, result.detours, result.connected));
		return result;
	}
} // namespace VillageConnections
