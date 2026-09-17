#pragma once
#include "GenerationSettings.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief Actual enclosed street faces, including merged civic grounds and new alley subdivisions.
namespace StreetBlocks
{
	struct Block
	{
		Array<Vec2> outline; Array<int> edges; Vec2 center{0,0}; RectF bounds; double area=0;
		bool contains(Vec2 point) const
		{
			if (!bounds.contains(point)) { return false; }
			bool inside=false;
			for (size_t i=0,j=outline.size()-1;i<outline.size();j=i++)
			{
				const Vec2 a=outline[i],b=outline[j];
				if ((a.y>point.y)!=(b.y>point.y) && point.x<(b.x-a.x)*(point.y-a.y)/(b.y-a.y)+a.x) { inside=!inside; }
			}
			return inside;
		}
	};
	inline Array<Block> collect(const RoadNetwork& network)
	{
		Array<Block> blocks; HashSet<int64> visited;
		for (const auto& initial : network.edges())
		{
			if (initial.id<0) { continue; }
			for (const int first : {initial.nodeA,initial.nodeB})
			{
				Block block; int id=initial.id,from=first;
				const int64 start=static_cast<int64>(id)*2+(from==initial.nodeB);
				if (visited.contains(start)) { continue; }
				bool closed=false;
				for (size_t step=0;step<=network.edges().size()*2;++step)
				{
					const auto* edge=network.getEdge(id); const bool forward=edge->nodeA==from;
					const int64 key=static_cast<int64>(id)*2+(!forward);
					if (step>0 && key==start) { closed=true; break; }
					if (!visited.insert(key).second) { break; }
					block.edges << id;
					const auto curve=network.getBezier(id);
					const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/20)));
					for (int sample=0;sample<count;++sample)
					{
						const Vec3 point=curve->evaluate(forward ? static_cast<float>(sample)/count : 1-static_cast<float>(sample)/count);
						block.outline << Vec2{point.x,point.z};
					}
					const int to=forward ? edge->nodeB : edge->nodeA;
					const Vec3 reverse=curve->tangent(forward ? 1.0f : 0.0f)*(forward ? -1 : 1);
					const double reverseAngle=Math::Atan2(reverse.z,reverse.x);
					int next=-1; double best=Math::TwoPi+1;
					for (const auto& attachment : network.getNode(to)->attachments)
					{
						const auto* candidate=network.getEdge(attachment.edgeId);
						if (!candidate) { continue; }
						const auto other=network.getBezier(candidate->id);
						const Vec3 direction=other->tangent(candidate->nodeA==to ? 0.0f : 1.0f)*(candidate->nodeA==to ? 1 : -1);
						double turn=std::fmod(reverseAngle-Math::Atan2(direction.z,direction.x)+Math::TwoPi,Math::TwoPi);
						if (candidate->id==id) { turn=Math::TwoPi; }
						if (turn<best) { best=turn; next=candidate->id; }
					}
					if (next<0) { break; }
					from=to; id=next;
				}
				if (!closed || block.outline.size()<3) { continue; }
				Vec2 lower{1e9,1e9},upper{-1e9,-1e9};
				for (size_t i=0;i<block.outline.size();++i)
				{
					const Vec2 a=block.outline[i],b=block.outline[(i+1)%block.outline.size()];
					block.area+=(a.x*b.y-b.x*a.y)*.5; block.center+=a;
					lower.x=Min(lower.x,a.x); lower.y=Min(lower.y,a.y); upper.x=Max(upper.x,a.x); upper.y=Max(upper.y,a.y);
				}
				block.center/=static_cast<double>(block.outline.size()); block.bounds={lower,upper-lower};
				if (block.area>GenerationSettings::get().parcels_minimumBlockArea) { blocks << std::move(block); }
			}
		}
		return blocks;
	}
	/// @brief 河川迂回と町割の間に生じた、住宅幅を確保できない細長い面を隣接街区へ統合する。
	inline int mergeNarrowFaces(RoadNetwork& network)
	{
		int removed=0;
		for (int pass=0;pass<3;++pass)
		{
			HashSet<int> remove;
			for (const auto& block : collect(network))
			{
				if (block.area>GenerationSettings::get().parcels_narrowBlockArea) { continue; }
				struct Boundary { Vec2 a,b; double width; };
				Array<Boundary> boundaries;
				for (const int id : block.edges)
				{
					const auto* edge=network.getEdge(id);const auto curve=network.getBezier(id);
					const int count=Max(1,static_cast<int>(std::ceil(curve->totalLength/4)));
					for (int i=0;i<count;++i) { const Vec3 a=curve->evaluate(static_cast<float>(i)/count),b=curve->evaluate(static_cast<float>(i+1)/count);boundaries<<Boundary{{a.x,a.z},{b.x,b.z},edge->totalWidth()*.5}; }
				}
				bool fits=false;
				for (double z=block.bounds.y+1;z<block.bounds.br().y && !fits;z+=2)
					for (double x=block.bounds.x+1;x<block.bounds.br().x && !fits;x+=2)
					{
						const Vec2 p{x,z};if (!block.contains(p)) { continue; }
						bool clear=true;
						for (const auto& boundary : boundaries)
						{
							const Vec2 delta=boundary.b-boundary.a;const double t=Clamp((p-boundary.a).dot(delta)/Max(.001,delta.lengthSq()),0.0,1.0);
							if (p.distanceFrom(boundary.a+delta*t)<boundary.width+GenerationSettings::get().parcels_minimumBuildingHalfWidth) { clear=false;break; }
						}
						fits=clear;
					}
				if (fits) { continue; }
				int selected=-1;double best=1e30;
				for (const int id : block.edges)
				{
					const auto* edge=network.getEdge(id);
					if (edge->tunnel || edge->useElevation || network.getNode(edge->nodeA)->attachments.size()<3 || network.getNode(edge->nodeB)->attachments.size()<3) { continue; }
					const double score=edge->totalWidth()*GenerationSettings::get().parcels_roadWidthPreservationWeight+edge->length;
					if (score<best) { best=score;selected=id; }
				}
				if (selected>=0) { remove.insert(selected); }
			}
			if (remove.empty()) { break; }
			// Recompute faces after each batch. Removing a cycle edge preserves connected access.
			Array<int> ids{remove.begin(),remove.end()};ids.sort();
			for (const int id : ids)
			{
				const auto* edge=network.getEdge(id);if (!edge) { continue; }const int a=edge->nodeA,b=edge->nodeB;
				if (network.getNode(a)->attachments.size()<3 || network.getNode(b)->attachments.size()<3) { continue; }
				network.removeEdge(id);network.rebuildNodeConnectivity(a,b);++removed;
			}
		}
		return removed;
	}

}
