#pragma once
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../debug/DebugLog.hpp"

/// @brief 小さな切盛土で施工できる道路は地上に戻し、共有地形を路面へ整形する。
namespace RoadTerrainFit
{
	inline int apply(RoadNetwork& roads,World& world)
	{
		struct Target { double height=0,weight=0,strength=0; };
		HashTable<int64,Target> targets; Array<int> groundEdges;
		constexpr double spacing=static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS;
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || !edge.useElevation) { continue; }
			const auto curve=roads.getBezier(edge.id); bool fits=true;
			const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/8)));
			for (int i=0;i<=count && fits;++i)
			{
				const Vec3 point=curve->evaluate(static_cast<float>(i)/count),right=tangentToRight(curve->tangent(static_cast<float>(i)/count));
				for (const int side : {-1,0,1})
				{
					const Vec3 sample=point+right*(edge.totalWidth()*.5*side);
					const double terrain=world.sampleHeight(static_cast<float>(sample.x),static_cast<float>(sample.z));
					fits&=Abs(point.y-terrain)<=2.0 && terrain>world.waterSurfaceHeight(sample.x,sample.z)+1.2;
				}
			}
			if (!fits) { continue; }
			groundEdges << edge.id;
			const double radius=edge.totalWidth()*.5+spacing;
			for (int i=0;i<=count;++i)
			{
				const Vec3 point=curve->evaluate(static_cast<float>(i)/count);
				for (int z=Max(0,static_cast<int>((point.z-radius)/spacing));z<=Min(WORLD_CHUNKS*HEIGHT_CELLS,static_cast<int>((point.z+radius)/spacing)+1);++z)
					for (int x=Max(0,static_cast<int>((point.x-radius)/spacing));x<=Min(WORLD_CHUNKS*HEIGHT_CELLS,static_cast<int>((point.x+radius)/spacing)+1);++x)
					{
						const Vec2 sample{x*spacing,z*spacing}; const double distance=sample.distanceFrom({point.x,point.z}); if (distance>=radius) { continue; }
						const double weight=Clamp((radius-distance)/spacing,0.0,1.0);
						auto& target=targets[static_cast<int64>(z)*65536+x]; target.height+=point.y*weight; target.weight+=weight; target.strength=Max(target.strength,weight);
					}
			}
		}
		for (const auto& [key,target] : targets)
		{
			const int x=static_cast<int>(key%65536),z=static_cast<int>(key/65536);
			const float terrain=world.sampleHeight(static_cast<float>(x*spacing),static_cast<float>(z*spacing));
			const double desired=Math::Lerp(terrain,target.height/target.weight,target.strength);
			if (Abs(desired-terrain)<=2.0 && desired>world.waterSurfaceHeight(x*spacing,z*spacing)+1.0) { world.setGridHeight(x,z,static_cast<float>(desired)); }
		}
		HashSet<int> nodes;
		for (const int id : groundEdges)
		{
			auto* edge=roads.getEdge(id); edge->useElevation=false;
			for (const auto& object : roads.objects()) { if (object.id>=0 && object.parentEdgeId==id && object.type==RoadObjectType::Pier) { roads.removeObject(object.id); } }
			nodes.insert(edge->nodeA); nodes.insert(edge->nodeB);
		}
		for (const int id : nodes)
		{
			auto* node=roads.getNode(id); bool ground=true;
			for (const auto& attachment : node->attachments) { const auto* edge=roads.getEdge(attachment.edgeId); ground&=edge && !edge->useElevation; }
			if (ground) { node->position.y=world.sampleHeight(static_cast<float>(node->position.x),static_cast<float>(node->position.z)); }
		}
		for (const int id : groundEdges)
		{
			auto* edge=roads.getEdge(id); const double a=roads.getNode(edge->nodeA)->position.y,b=roads.getNode(edge->nodeB)->position.y;
			edge->ctrlA.y=Math::Lerp(a,b,1.0/3); edge->ctrlB.y=Math::Lerp(a,b,2.0/3); edge->length=roads.getBezier(id)->totalLength;
			roads.rebuildNodeConnectivity(edge->nodeA,edge->nodeB);
		}
		for (int z=0;z<WORLD_CHUNKS;++z) for (int x=0;x<WORLD_CHUNKS;++x) { if (auto* chunk=world.getChunk({x,z});chunk && chunk->meshDirty) { chunk->updateHeightBounds(); } }
		DBG_LOG(U"[RoadEarthworks] groundedEdges={} adjustedVertices={} maximumCutFill=2m"_fmt(groundEdges.size(),targets.size()));
		return static_cast<int>(groundEdges.size());
	}
}
