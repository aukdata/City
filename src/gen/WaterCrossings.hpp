#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"
#include "RoadDesignLimits.hpp"
#include <queue>

/// @brief 完成した曲線・路肩まで水域検査し、必要な橋と連続した取り付け勾配を構成する。
namespace WaterCrossings
{
	struct Result { int wetEdges=0; int elevatedEdges=0; };
	/// @brief 水域と橋の取り付けだけを短区間に分け、離れた交差点へ川の高さを直接代入しない。
	template<class HeightSampler, class WaterSampler>
	void splitWaterSpans(RoadNetwork& network, const HeightSampler& height, const WaterSampler& water)
	{
		// Short approach spans follow the real bank instead of lifting an entire kilometre-long road.
		HashSet<int> subdivide;
		for (const auto& edge : network.edges())
		{
			if (edge.id<0 || edge.tunnel) { continue; }
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			bool wet=false;
			for (float arc=0;arc<=curve->totalLength;arc+=GenerationSettings::get().crossings_waterSampleStep)
			{
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,0,1})
				{
					const Vec3 sample=point+right*(edge.totalWidth()*.5*side);
					wet|=height(sample.x,sample.z)<water(sample.x,sample.z)+GenerationSettings::get().crossings_waterBankMargin;
				}
			}
			if (!wet) { continue; }
			subdivide.insert(edge.id);
			for (const int node : {edge.nodeA,edge.nodeB})
			{
				for (const auto& attachment : network.getNode(node)->attachments) { subdivide.insert(attachment.edgeId); }
			}
		}
		Array<int> splitIds{subdivide.begin(),subdivide.end()}; splitIds.sort();
		for (int id : splitIds)
		{
			const auto* edge=network.getEdge(id);
			if (!edge || edge->tunnel) { continue; }
			const int end=edge->nodeB;
			const int count=Max(1,static_cast<int>(std::ceil(edge->length/GenerationSettings::get().crossings_approachSegmentLength)));
			for (int remaining=count;remaining>1;--remaining)
			{
				const int node=network.splitEdgeAtParameter(id,1.0f/remaining);
				if (node<0) { break; }
				auto* point=network.getNode(node);
				for (const auto& attachment : point->attachments)
				{
					auto* segment=network.getEdge(attachment.edgeId);
					const double a=network.getNode(segment->nodeA)->position.y,b=network.getNode(segment->nodeB)->position.y;
					segment->ctrlA.y=Math::Lerp(a,b,1.0/3); segment->ctrlB.y=Math::Lerp(a,b,2.0/3);
					if (segment->nodeA==end || segment->nodeB==end) { id=segment->id; }
				}
			}
		}
	}

	template<class HeightSampler,class WaterSampler>
	Result repair(RoadNetwork& network, const HeightSampler& height,const WaterSampler& water)
	{
		Result result;
		splitWaterSpans(network, height, water);
		HashTable<int,double> original, level;
		HashSet<int> affected;
		std::priority_queue<std::pair<double,int>> pending;
		for (const auto& node : network.nodes())
		{
			if (node.id>=0) { original[node.id]=level[node.id]=node.position.y; }
		}
		const auto raise = [&](int id, double value)
		{
			if (value>level[id]+.001) { level[id]=value; pending.emplace(value,id); }
		};
		for (const auto& edge : network.edges())
		{
			if (edge.id<0 || edge.tunnel) { continue; }
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/GenerationSettings::get().crossings_waterSampleStep)));
			bool wet=false; double bridgeLevel=GenerationSettings::get().crossings_waterClearance;
			for (int index=0;index<=count;++index)
			{
				const float t=static_cast<float>(index)/count;
				const Vec3 point=curve->evaluate(t), right=tangentToRight(curve->tangent(t));
				double low=1e9,surface=0;
				for (const int side : {-1,0,1})
				{
					const Vec3 sample=point+right*(edge.totalWidth()*.5*side);
					const double ground=height(sample.x,sample.z);
					surface=Max(surface,water(sample.x,sample.z));
					low=Min(low,ground);
				}
				surface=Max(surface,water(point.x,point.z));
				wet|=low<surface+GenerationSettings::get().crossings_waterBankMargin;
				if (low<surface+GenerationSettings::get().crossings_waterBankMargin) { bridgeLevel=Max(bridgeLevel,surface+GenerationSettings::get().crossings_waterClearance); }
			}
			if (wet)
			{
				++result.wetEdges; affected.insert(edge.id);
				raise(edge.nodeA,bridgeLevel); raise(edge.nodeB,bridgeLevel);
				pending.emplace(level[edge.nodeA],edge.nodeA);pending.emplace(level[edge.nodeB],edge.nodeB);
			}
		}
		HashSet<int> wetNodes;
		for (const int id : affected) { const auto* edge=network.getEdge(id); wetNodes.insert(edge->nodeA); wetNodes.insert(edge->nodeB); }
		for (const int start : wetNodes)
		{
			// A valley bridge considers both banks. An uphill road alone is not a reason to lift the downhill town.
			double commonBank = Math::Inf; int approaches = 0;
			for (const auto& entry : network.getNode(start)->attachments)
			{
				const auto* first = network.getEdge(entry.edgeId); if (!first || first->tunnel) { continue; }
				const int neighbour = first->nodeA == start ? first->nodeB : first->nodeA;
				std::priority_queue<std::pair<double,int>> search;
				HashTable<int,double> distance; distance[start] = 0; distance[neighbour] = first->length;
				search.emplace(-first->length, neighbour);
				double bankLevel = level[start];
				while (!search.empty())
				{
					const auto [negativeRun,id] = search.top(); search.pop(); const double run = -negativeRun;
					if (run > GenerationSettings::get().crossings_bankSearchDistance || run > distance[id] + .01) { continue; }
					bankLevel = Max(bankLevel, original[id] - run * GenerationSettings::get().crossings_bankApproachGrade);
					for (const auto& attachment : network.getNode(id)->attachments)
					{
						const auto* edge = network.getEdge(attachment.edgeId); if (!edge || edge->tunnel) { continue; }
						const int other = edge->nodeA == id ? edge->nodeB : edge->nodeA;
						const double nextRun = run + edge->length;
						if (nextRun <= GenerationSettings::get().crossings_bankSearchDistance && (!distance.contains(other) || nextRun < distance[other])) { distance[other] = nextRun; search.emplace(-nextRun, other); }
					}
				}
				commonBank = Min(commonBank, bankLevel); ++approaches;
			}
			if (approaches >= 2) { raise(start, commonBank); }
		}
		// Height raises spread along incident edges, so a bridge never ends in a vertical step.
		const auto propagate = [&]()
		{
			while (!pending.empty())
			{
				const auto [value,id]=pending.top(); pending.pop();
				if (value<level[id]-.001) { continue; }
				for (const auto& attachment : network.getNode(id)->attachments)
				{
					const auto* edge=network.getEdge(attachment.edgeId);
					if (!edge) { continue; }
					const int other=edge->nodeA==id ? edge->nodeB : edge->nodeA;
					const Vec3 delta=network.getNode(id)->position-network.getNode(other)->position;
					const double run=Max(1.0,Vec2{delta.x,delta.z}.length());
					if (edge->tunnel) { continue; }
					const double grade = RoadDesignLimits::forType(edge->roadType).maximumGrade * GenerationSettings::get().routing_finalGradeReserve;
					if (value-run*grade>level[other]+.001 || value>original[id]+.001)
					{
						affected.insert(edge->id);
						raise(other,value-run*grade);
					}
				}
			}
		};
		propagate();
		// Anchor the immediate dry approach ends once, using only their original heights.
		// The extra dry segment joins a deeply incised bank without recursively borrowing distant hills.
		HashSet<int> bankAnchors;
		for (const auto& node : network.nodes())
		{
			if (node.id < 0) { continue; }
			bool inside = false, outside = false;
			for (const auto& attachment : node.attachments)
			{
				inside |= affected.contains(attachment.edgeId);
				outside |= !affected.contains(attachment.edgeId);
			}
			if (!inside || !outside) { continue; }
			bankAnchors.insert(node.id);
			for (const auto& attachment : node.attachments)
			{
				if (affected.contains(attachment.edgeId)) { continue; }
				const auto* edge = network.getEdge(attachment.edgeId);
				if (!edge || edge->tunnel || edge->length > GenerationSettings::get().crossings_bankSearchDistance) { continue; }
				const int other = edge->nodeA == node.id ? edge->nodeB : edge->nodeA;
				const Vec3 point = network.getNode(other)->position;
				if (height(point.x, point.z) >= water(point.x, point.z) + GenerationSettings::get().crossings_waterBankMargin) { bankAnchors.insert(other); }
			}
		}
		for (const int id : bankAnchors) { pending.emplace(original[id], id); }
		propagate();
		for (const auto& [id,value] : level) { network.getNode(id)->position.y=value; }
		for (const int id : affected)
		{
			auto* edge=network.getEdge(id);
			const Vec3 start=network.getNode(edge->nodeA)->position,end=network.getNode(edge->nodeB)->position;
			const Vec2 chord{end.x-start.x,end.z-start.z};
			const auto profile=[&](Vec3 point) { return start.y+(end.y-start.y)*Vec2{point.x-start.x,point.z-start.z}.dot(chord)/Max(.001,chord.lengthSq()); };
			edge->ctrlA.y=profile(edge->ctrlA);edge->ctrlB.y=profile(edge->ctrlB);
			edge->useElevation=true;
			if (const auto curve=network.getBezier(id)) { edge->length=curve->totalLength; }
			++result.elevatedEdges;
		}
		for (const int id : affected)
		{
			const auto* edge=network.getEdge(id);
			network.rebuildNodeConnectivity(edge->nodeA,edge->nodeB);
		}
		return result;
	}
	template<class HeightSampler>
	Result repair(RoadNetwork& network,const HeightSampler& height)
	{
		return repair(network,height,[](double,double) { return 0.0; });
	}

}
