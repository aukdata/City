#pragma once
#include "GenerationSettings.hpp"
#include "../world/World.hpp"
#include "../road/RoadNetwork.hpp"
#include "../debug/DebugLog.hpp"

/// @brief 同じ設計路面から切土・盛土と橋を区分し、1:1.5の法面で地形へ戻す。
namespace RoadTerrainFit
{
	inline int apply(RoadNetwork& roads,World& world,const HashSet<int>& onlyEdges={})
	{
		struct Target { double height=0,distance=1e30; };
		HashTable<int64,Target> targets;
		HashSet<int> converted;
		constexpr double spacing=static_cast<double>(CHUNK_SIZE)/HEIGHT_CELLS;
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || edge.tunnel || (!onlyEdges.empty() && !onlyEdges.contains(edge.id))) { continue; }
			const auto curve=roads.getBezier(edge.id);
			if (!curve) { continue; }
			const int count=Max(2,static_cast<int>(std::ceil(curve->totalLength/6)));
			bool fits=true;
			if (edge.useElevation)
			{
				for (int i=0;i<=count && fits;++i)
				{
					const Vec3 point=curve->evaluate(static_cast<float>(i)/count),right=tangentToRight(curve->tangent(static_cast<float>(i)/count));
					for (const int side : {-1,0,1})
					{
						const Vec3 sample=point+right*(edge.totalWidth()*.5*side);
						const double terrain=world.sampleHeight(static_cast<float>(sample.x),static_cast<float>(sample.z));
						fits&=point.y-terrain<=GenerationSettings::get().roads_maximumFill && terrain-point.y<=GenerationSettings::get().roads_maximumCut && terrain>world.waterSurfaceHeight(sample.x,sample.z)+GenerationSettings::get().crossings_earthworkWaterMargin;
					}
				}
				if (!fits) { continue; }
				converted.insert(edge.id);
			}
			const double half=edge.totalWidth()*.5,radius=half+64;
			for (int i=0;i<=count;++i)
			{
				Vec3 point=curve->evaluate(static_cast<float>(i)/count);
				if (!edge.usesDesignHeight()) { point.y=world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z)); }
				for (int z=Max(0,static_cast<int>((point.z-radius)/spacing));z<=Min(WORLD_CHUNKS*HEIGHT_CELLS,static_cast<int>((point.z+radius)/spacing)+1);++z)
					for (int x=Max(0,static_cast<int>((point.x-radius)/spacing));x<=Min(WORLD_CHUNKS*HEIGHT_CELLS,static_cast<int>((point.x+radius)/spacing)+1);++x)
					{
						const Vec2 sample{x*spacing,z*spacing}; const double distance=sample.distanceFrom({point.x,point.z});
						if (distance>=radius) { continue; }
						const double terrain=world.sampleHeight(static_cast<float>(sample.x),static_cast<float>(sample.y));
						if (terrain<=world.waterSurfaceHeight(sample.x,sample.y)+GenerationSettings::get().crossings_earthworkWaterMargin) { continue; }
						// One grid margin keeps interpolation flat across a narrow road; outside it the slope is bounded.
						const double relief=Max(0.0,distance-half-spacing)*GenerationSettings::get().crossings_earthworkSlope;
						const double desired=Clamp(terrain,point.y-relief,point.y+relief);
						auto& target=targets[static_cast<int64>(z)*65536+x];
						if (distance<target.distance) { target={desired,distance}; }
					}
			}
		}
		double cut=0,fill=0; int changed=0;
		for (const auto& [key,target] : targets)
		{
			const int x=static_cast<int>(key%65536),z=static_cast<int>(key/65536);
			const double terrain=world.sampleHeight(static_cast<float>(x*spacing),static_cast<float>(z*spacing));
			if (Abs(target.height-terrain)<.001) { continue; }
			cut=Max(cut,terrain-target.height);fill=Max(fill,target.height-terrain);++changed;
			world.setGridHeight(x,z,static_cast<float>(target.height));
		}
		for (const int id : converted)
		{
			roads.getEdge(id)->useElevation=false;
			Array<int> piers;
			for (const auto& object : roads.objects()) { if (object.id>=0 && object.parentEdgeId==id && object.type==RoadObjectType::Pier) { piers<<object.id; } }
			for (const int pier : piers) { roads.removeObject(pier); }
		}
		for (const auto& value : roads.nodes())
		{
			if (value.id<0) { continue; }
			bool ground=true;
			for (const auto& attachment : value.attachments) { const auto* edge=roads.getEdge(attachment.edgeId); ground&=edge && !edge->usesDesignHeight(); }
			if (ground) { roads.getNode(value.id)->position.y=world.sampleHeight(static_cast<float>(value.position.x),static_cast<float>(value.position.z)); }
		}
		for (int z=0;z<WORLD_CHUNKS;++z) for (int x=0;x<WORLD_CHUNKS;++x) { if (auto* chunk=world.getChunk({x,z});chunk && chunk->meshDirty) { chunk->updateHeightBounds(); } }
		DBG_LOG(U"[RoadEarthworks] groundedEdges={} adjustedVertices={} maxCut={:.2f} maxFill={:.2f} slope=1:1.5"_fmt(converted.size(),changed,cut,fill));
		return static_cast<int>(converted.size());
	}
}
