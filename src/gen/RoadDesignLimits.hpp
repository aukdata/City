#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"
#include "../debug/DebugLog.hpp"
#include "RoadAlignment.hpp"
#include <queue>

/// @brief 道路種別ごとの縦断・平面線形。値はゲームの設計速度に対応する。
namespace RoadDesignLimits
{
	struct Limits { double maximumGrade; double minimumRadius; };

	inline Limits forType(RoadType type, TransportMode mode = TransportMode::Road)
	{
		if (mode == TransportMode::Rail) { return {GenerationSettings::get().railway_maximumGrade,GenerationSettings::get().railway_minimumRadius}; }
		switch (type)
		{
		case RoadType::LocalRoad: return { GenerationSettings::get().roads_localMaximumGrade, GenerationSettings::get().roads_localMinimumRadius };
		case RoadType::Expressway:
		case RoadType::Highway: return { GenerationSettings::get().roads_highwayMaximumGrade, GenerationSettings::get().roads_highwayMinimumRadius };
		default: return { GenerationSettings::get().roads_arterialMaximumGrade, GenerationSettings::get().roads_arterialMinimumRadius };
		}
	}

	inline double minimumRadius(const CubicBezier& curve) { return curve.minimumHorizontalRadius(); }

	/// @brief 通過道路は接線を共有した鎖として丸め、個別曲線の修正による折れを防ぐ。
	inline int smoothThroughChains(RoadNetwork& roads)
	{
		HashSet<int> through, visited;
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0 || node.attachments.size() != 2) { continue; }
			const auto* first = roads.getEdge(node.attachments[0].edgeId);
			const auto* second = roads.getEdge(node.attachments[1].edgeId);
			if (!first || !second || first->roadType != second->roadType) { continue; }
			const auto outward = [&](const RoadEdge& edge)
			{
				const auto curve = roads.getBezier(edge.id);
				const Vec3 tangent = edge.nodeA == node.id ? curve->tangent(0) : -curve->tangent(1);
				return Vec2{tangent.x, tangent.z}.normalized();
			};
			// Genuine street corners/intersections are low-speed turns, not through-road curves.
			if (outward(*first).dot(outward(*second)) < -.985) { through.insert(node.id); }
		}
		int corrected = 0;
		for (const auto& first : roads.edges())
		{
			if (first.id < 0 || visited.contains(first.id)) { continue; }
			Array<int> chain{first.id}, nodes{first.nodeA, first.nodeB};
			visited.insert(first.id);
			for (int direction = 0; direction < 2; ++direction)
			{
				for (;;)
				{
					const int node = direction == 0 ? nodes.front() : nodes.back();
					if (!through.contains(node)) { break; }
					int next = -1;
					for (const auto& attachment : roads.getNode(node)->attachments)
					{
						if (!visited.contains(attachment.edgeId)) { next = attachment.edgeId; }
					}
					if (next < 0) { break; }
					const auto* edge = roads.getEdge(next);
					visited.insert(next);
					const int other = edge->nodeA == node ? edge->nodeB : edge->nodeA;
					if (direction == 0) { chain.insert(chain.begin(), next); nodes.insert(nodes.begin(), other); }
					else { chain << next; nodes << other; }
				}
			}
			if (chain.size() < 2) { continue; }
			bool designed=false;
			for (int id : chain) { designed |= roads.getEdge(id)->designGrade; }
			if (designed) { continue; }
			const double required = forType(first.roadType).minimumRadius*1.03;
			bool needsRepair = false;
			for (int id : chain) { needsRepair |= minimumRadius(*roads.getBezier(id)) < required; }
			if (!needsRepair) { continue; }
			Array<Vec3> points;
			for (int node : nodes) { points << roads.getNode(node)->position; }
			const bool closed = nodes.front() == nodes.back();
			Array<CubicBezier> curves;
			const auto fit = [&]()
			{
				curves.clear();
				double smallest = Math::Inf;
				for (size_t index = 0; index+1 < points.size(); ++index)
				{
					const Vec3 a = points[index], b = points[index+1];
					const Vec3 previous = index > 0 ? points[index-1] : (closed ? points[points.size()-2] : a-(b-a));
					const Vec3 next = index+2 < points.size() ? points[index+2] : (closed ? points[1] : b+(b-a));
					Vec3 directionA = b-previous, directionB = next-a;
					directionA.y = directionB.y = 0;
					const double arm = Vec2{b.x-a.x,b.z-a.z}.length()/3;
					if (directionA.lengthSq() < 1e-12 || directionB.lengthSq() < 1e-12) { return false; }
					curves << CubicBezier{a, a+directionA.normalized()*arm, b-directionB.normalized()*arm, b};
					smallest = Min(smallest, minimumRadius(curves.back()));
				}
				return smallest >= required;
			};
			bool fitted = fit();
			for (int iteration = 0; iteration < 64 && !fitted; ++iteration)
			{
				if (closed) { break; }
				Array<Vec3> next = points;
				for (size_t index = 1; index+1 < points.size(); ++index)
				{
					next[index] = points[index].lerp((points[index-1]+points[index+1])*.5, .4);
				}
				points = std::move(next);
				fitted = fit();
			}
			if (!fitted && !closed)
			{
				// An unfit short chain becomes an engineered direct connection with the same endpoints.
				for (size_t index = 1; index+1 < points.size(); ++index)
				{
					points[index] = points.front().lerp(points.back(), static_cast<double>(index)/(points.size()-1));
				}
				fitted = fit();
			}
			if (!fitted) { continue; }
			for (size_t index = 1; index+1 < points.size(); ++index) { roads.getNode(nodes[index])->position = points[index]; }
			for (size_t index = 0; index < chain.size(); ++index)
			{
				auto* edge = roads.getEdge(chain[index]);
				const bool forward = edge->nodeA == nodes[index];
				edge->ctrlA = forward ? curves[index].p1 : curves[index].p2;
				edge->ctrlB = forward ? curves[index].p2 : curves[index].p1;
				edge->length = curves[index].totalLength;
			}
			++corrected;
		}
		return corrected;
	}


	/// @brief 固定した交差点の間の曲線だけを緩める。交差点内の右左折は対象外。
	inline void constrainCurve(RoadNetwork& roads, int id)
	{
		auto* edge = roads.getEdge(id);
		const auto original = roads.getBezier(id);
		if (!edge || !original) { return; }
		const double radius = forType(edge->roadType).minimumRadius;
		if (minimumRadius(*original) >= radius) { return; }
		const Vec3 straightA = original->p0.lerp(original->p3, 1.0/3);
		const Vec3 straightB = original->p0.lerp(original->p3, 2.0/3);
		double low = 0, high = 1;
		for (int iteration = 0; iteration < 18; ++iteration)
		{
			const double weight = (low+high)*.5;
			const CubicBezier candidate{original->p0, straightA.lerp(original->p1, weight),
				straightB.lerp(original->p2, weight), original->p3};
			if (minimumRadius(candidate) >= radius*1.01) { low = weight; }
			else { high = weight; }
		}
		edge->ctrlA = straightA.lerp(original->p1, low);
		edge->ctrlB = straightB.lerp(original->p2, low);
	}

	struct Audit
	{
		int edges = 0, gradeViolations = 0, radiusViolations = 0;
		double maximumGrade = 0, smallestRadius = Math::Inf;
	};

	inline Audit measure(const RoadNetwork& roads, const World& world)
	{
		Audit result;
		for (const auto& edge : roads.edges())
		{
			if (edge.id < 0) { continue; }
			const auto curve = roads.getBezier(edge.id);
			if (!curve || curve->totalLength < .01f) { continue; }
			++result.edges;
			const double radius = minimumRadius(*curve);
			result.smallestRadius = Min(result.smallestRadius, radius);
			result.radiusViolations += radius+.01 < forType(edge.roadType).minimumRadius;
			const int count = Max(8, static_cast<int>(std::ceil(curve->totalLength/3)));
			Vec3 previous{0, 0, 0};
			double grade = 0;
			for (int index = 0; index <= count; ++index)
			{
				Vec3 point = curve->evaluate(static_cast<float>(index)/count);
				if (!edge.usesDesignHeight()) { point.y = world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.z)); }
				if (index > 0)
				{
					const Vec3 delta = point-previous;
					grade = Max(grade, Abs(delta.y)/Max(.00001, Vec2{delta.x, delta.z}.length()));
				}
				previous = point;
			}
			result.maximumGrade = Max(result.maximumGrade, grade);
			result.gradeViolations += grade > forType(edge.roadType).maximumGrade+.0001;
		}
		return result;
	}

	/// @brief 急勾配の接続を再探索し、地形以下の包絡線で浅い切土を整える。街全体を持ち上げない。
	inline void fitGrades(RoadNetwork& roads, const World& world)
	{
		struct Link { int other; double rise; };
		HashTable<int, Array<Link>> links;
		HashTable<int, double> lower, waterFloor;
		for (const auto& node : roads.nodes())
		{
			if (node.id < 0) { continue; }
			lower[node.id] = node.position.y;
			waterFloor[node.id] = -1e9;
		}
		for (const auto& edge : roads.edges())
		{
			if (edge.id < 0) { continue; }
			constrainCurve(roads, edge.id);
			if (edge.roadType == RoadType::LocalRoad && minimumRadius(*roads.getBezier(edge.id)) < 30)
			{
				roads.getEdge(edge.id)->speedLimit = Min(edge.speedLimit, 20.0f);
			}
			const Vec3 a = roads.getNode(edge.nodeA)->position, b = roads.getNode(edge.nodeB)->position;
			const double rise = Vec2{b.x-a.x, b.z-a.z}.length()*forType(edge.roadType).maximumGrade*GenerationSettings::get().routing_finalGradeReserve;
			links[edge.nodeA] << Link{edge.nodeB, rise};
			links[edge.nodeB] << Link{edge.nodeA, rise};
			const auto curve = roads.getBezier(edge.id);
			const int count = Max(2, static_cast<int>(std::ceil(curve->totalLength/6)));
			for (int index = 0; index <= count; ++index)
			{
				const float t = static_cast<float>(index)/count;
				const Vec3 point = curve->evaluate(t), right = tangentToRight(curve->tangent(t));
				for (const int side : {-1, 0, 1})
				{
					const Vec3 sample = point + right*(edge.totalWidth()*.5*side);
					const double water = world.waterSurfaceHeight(sample.x, sample.z);
					const double ground = world.sampleHeight(static_cast<float>(sample.x), static_cast<float>(sample.z));
					// A covered tunnel beneath a stream does not need a bridge above that stream.
					if (ground - point.y > GenerationSettings::get().roads_maximumCut) { continue; }
					if (ground < water+GenerationSettings::get().crossings_waterBankMargin)
					{
						waterFloor[edge.nodeA] = Max(waterFloor[edge.nodeA], water+GenerationSettings::get().crossings_waterClearance);
						waterFloor[edge.nodeB] = Max(waterFloor[edge.nodeB], water+GenerationSettings::get().crossings_waterClearance);
					}
				}
			}
		}
		// The lower envelope never raises a dry street because of a remote mountain.
		// Water clearance is the only propagated lower bound, and decays along each approach.
		const auto envelope = [&](HashTable<int, double>& levels, double sign)
		{
			using Entry = std::pair<double, int>;
			std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
			for (const auto& [id, value] : levels) { pending.emplace(sign*value, id); }
			while (!pending.empty())
			{
				const auto [value, id] = pending.top(); pending.pop();
				if (value > levels[id]*sign+1e-8) { continue; }
				for (const auto& link : links[id])
				{
					const double next = value+link.rise;
					if (next < levels[link.other]*sign-1e-8)
					{
						levels[link.other] = next*sign;
						pending.emplace(next, link.other);
					}
				}
			}
		};
		envelope(lower, 1); envelope(waterFloor, -1);
		for (const auto& [id, value] : lower)
		{
			roads.getNode(id)->position.y = Max(value, waterFloor[id]);
		}
		for (const auto& value : roads.edges())
		{
			if (value.id < 0) { continue; }
			auto* edge = roads.getEdge(value.id);
			const Vec3 a = roads.getNode(edge->nodeA)->position, b = roads.getNode(edge->nodeB)->position;
			const Vec2 chord{b.x-a.x, b.z-a.z};
			const auto level = [&](Vec3 point)
			{
				return a.y+(b.y-a.y)*Vec2{point.x-a.x, point.z-a.z}.dot(chord)/Max(1e-12, chord.lengthSq());
			};
			edge->ctrlA.y = level(edge->ctrlA); edge->ctrlB.y = level(edge->ctrlB);
			edge->designGrade = true;
			roads.updateEdgeElevation(edge->id, world);
			edge->length = roads.getBezier(edge->id)->totalLength;
		}
		for (const auto& node : roads.nodes())
		{
			if (node.id >= 0) { roads.updateNodeCutoffs(node.id); roads.rebuildLaneConnections(node.id); }
		}
	}

	/// @brief 経路の選択と縦断の確定。交差点の統合後は fitGrades で制約を再確定する。
	inline void apply(RoadNetwork& roads,const World& world)
	{
		RoadAlignment::repairSteepEdges(roads,world);
		fitGrades(roads,world);
	}

}
