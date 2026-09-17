#pragma once
#include "RailCostProfile.hpp"
#include "RoadConstructionCost.hpp"
#include "RoadDesignLimits.hpp"
#include "../road/RoadNetwork.hpp"
#include "../world/World.hpp"

/// @brief 交差点間の幹線を一つの縦断として最適化し、急な山越えにはトンネルを選べるようにする。
namespace RoadVerticalAlignment
{
	inline void apply(RoadNetwork& roads,const World& world)
	{
		HashSet<int> visited; int tunnelSections=0;
		for (const auto& first : roads.edges())
		{
			if (first.id<0 || first.roadType==RoadType::LocalRoad || visited.contains(first.id)) { continue; }
			Array<int> chain{first.id},nodes{first.nodeA,first.nodeB}; visited.insert(first.id);
			for (int direction=0;direction<2;++direction)
			{
				for (;;)
				{
					const int id=direction==0 ? nodes.front() : nodes.back(); const auto* node=roads.getNode(id);
					if (node->attachments.size()!=2) { break; }
					int next=-1;
					for (const auto& attachment : node->attachments) { if (!visited.contains(attachment.edgeId)) { next=attachment.edgeId; } }
					const auto* edge=roads.getEdge(next); if (!edge || edge->roadType!=first.roadType) { break; }
					visited.insert(next); const int other=edge->nodeA==id ? edge->nodeB : edge->nodeA;
					if (direction==0) { chain.insert(chain.begin(),next); nodes.insert(nodes.begin(),other); } else { chain << next; nodes << other; }
				}
			}
			Array<RailCostProfile::Sample> samples; Array<size_t> nodeSamples{0}; double length=0,low=1e9,high=-1e9;
			for (size_t i=0;i<chain.size();++i)
			{
				const auto curve=roads.getBezier(chain[i]); const bool reverse=roads.getEdge(chain[i])->nodeA!=nodes[i];
				const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/35))); length+=curve->totalLength;
				for (int j=0;j<count;++j)
				{
					const float t=static_cast<float>(j)/count; const Vec3 p=curve->positionAt(curve->totalLength*(reverse ? 1-t : t));
					const double ground=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)),water=world.waterSurfaceHeight(p.x,p.z);
					low=Min(low,ground);high=Max(high,ground);
					const bool wet = ground < water + 1;
					samples << RailCostProfile::Sample{{p.x,p.z}, ground, Max(-30.0, ground-200), Max(ground+14, water+12),
						wet ? water+6 : -1e9, wet ? ground-8.01 : 1e9};
				}
				nodeSamples << samples.size();
			}
			if (length<600 || high-low<20) { continue; }
			const Vec3 start=roads.getNode(nodes.front())->position,end=roads.getNode(nodes.back())->position;
			samples << RailCostProfile::Sample{{end.x,end.z},end.y,end.y,end.y}; samples.front().minimum=samples.front().maximum=start.y;
			const auto profile = RailCostProfile::solve(samples, start.y, end.y, RoadDesignLimits::forType(first.roadType).maximumGrade * .995,
				[&](const RailCostProfile::Sample& sample, double elevation)
				{
					return RoadConstructionCost::unit(elevation, sample.ground, world.waterSurfaceHeight(sample.position.x, sample.position.y));
				});
			if (!profile.feasible) { continue; }
			bool tunnel=false; for (size_t i=0;i<samples.size();++i) { tunnel|=samples[i].ground-profile.heights[i]>6; }
			if (!tunnel) { continue; }
			for (size_t i=1;i+1<nodes.size();++i) { roads.getNode(nodes[i])->position.y=profile.heights[nodeSamples[i]]; }
			for (size_t i=0;i<chain.size();++i)
			{
				auto* edge=roads.getEdge(chain[i]); const double a=roads.getNode(edge->nodeA)->position.y,b=roads.getNode(edge->nodeB)->position.y;
				edge->ctrlA.y=Math::Lerp(a,b,1.0/3);edge->ctrlB.y=Math::Lerp(a,b,2.0/3); roads.updateEdgeElevation(edge->id,world);
				if (edge->tunnel) { ++tunnelSections; } roads.rebuildNodeConnectivity(edge->nodeA,edge->nodeB);
			}
		}
		DBG_LOG(U"[RoadVerticalAlignment] tunnelSections={}"_fmt(tunnelSections));
	}
}
