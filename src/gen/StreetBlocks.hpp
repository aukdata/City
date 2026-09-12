#pragma once
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
				if (block.area>100) { blocks << std::move(block); }
			}
		}
		return blocks;
	}
}
