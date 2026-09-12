#pragma once
#include "../road/RoadNetwork.hpp"
#include <queue>

/// @brief 完成した曲線・路肩まで水域検査し、必要な橋と連続した取り付け勾配を構成する。
namespace WaterCrossings
{
	struct Result { int wetEdges=0; int elevatedEdges=0; };
	template<class HeightSampler,class WaterSampler>
	Result repair(RoadNetwork& network, const HeightSampler& height,const WaterSampler& water)
	{
		Result result;
		// Short approach spans follow the real bank instead of lifting an entire kilometre-long road.
		HashSet<int> subdivide;
		for (const auto& edge : network.edges())
		{
			if (edge.id<0 || edge.useElevation) { continue; }
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			bool wet=false;
			for (float arc=0;arc<=curve->totalLength;arc+=6)
			{
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,0,1})
				{
					const Vec3 sample=point+right*(edge.totalWidth()*.5*side);
					wet|=height(sample.x,sample.z)<water(sample.x,sample.z)+1;
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
			if (!edge || edge->useElevation) { continue; }
			const int end=edge->nodeB;
			const int count=Max(1,static_cast<int>(std::ceil(edge->length/48)));
			for (int remaining=count;remaining>1;--remaining)
			{
				const int node=network.splitEdgeAtParameter(id,1.0f/remaining);
				if (node<0) { break; }
				auto* point=network.getNode(node); point->position.y=height(point->position.x,point->position.z);
				for (const auto& attachment : point->attachments)
				{
					auto* segment=network.getEdge(attachment.edgeId);
					const double a=network.getNode(segment->nodeA)->position.y,b=network.getNode(segment->nodeB)->position.y;
					segment->ctrlA.y=Math::Lerp(a,b,1.0/3); segment->ctrlB.y=Math::Lerp(a,b,2.0/3);
					if (segment->nodeA==end || segment->nodeB==end) { id=segment->id; }
				}
			}
		}
		HashTable<int,double> original, level;
		HashTable<int,Array<std::pair<float,double>>> samples;
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
			if (edge.id<0) { continue; }
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/6)));
			bool wet=false; double bridgeLevel=6;
			for (int index=0;index<=count;++index)
			{
				const float t=static_cast<float>(index)/count;
				const Vec3 point=curve->evaluate(t), right=tangentToRight(curve->tangent(t));
				double low=1e9, high=-1e9,surface=0;
				for (const int side : {-1,0,1})
				{
					const Vec3 sample=point+right*(edge.totalWidth()*.5*side);
					const double ground=height(sample.x,sample.z);
					surface=Max(surface,water(sample.x,sample.z));
					low=Min(low,ground); high=Max(high,ground);
				}
				samples[edge.id] << std::pair<float,double>{t,high};
				surface=Max(surface,water(point.x,point.z));
				wet|=low<surface+1 && (!edge.useElevation || point.y<surface+5.5);
				if (low<surface+1) { bridgeLevel=Max(bridgeLevel,surface+6); }
			}
			if (wet)
			{
				++result.wetEdges; affected.insert(edge.id);
				raise(edge.nodeA,bridgeLevel); raise(edge.nodeB,bridgeLevel);
			}
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
					affected.insert(edge->id);
					const double grade=Max(.055,Abs(original[id]-original[other])/run);
					raise(other,value-run*grade);
				}
			}
		};
		propagate();
		for (const auto& [id,value] : level) { network.getNode(id)->position.y=value; }
		for (const int id : affected)
		{
			auto* edge=network.getEdge(id);
			edge->ctrlA.y=Math::Lerp(level[edge->nodeA],level[edge->nodeB],1.0/3);
			edge->ctrlB.y=Math::Lerp(level[edge->nodeA],level[edge->nodeB],2.0/3);
			// Fit a local convex vertical control hull to the bank without raising distant roads.
			double lift=0;
			for (const auto& [t,ground] : samples[id])
			{
				if (t<=.001f || t>=.999f) { continue; }
				const double deficit=ground-Math::Lerp(level[edge->nodeA],level[edge->nodeB],t);
				lift=Max(lift,deficit/(3*t*(1-t)));
			}
			edge->ctrlA.y+=lift; edge->ctrlB.y+=lift;
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
