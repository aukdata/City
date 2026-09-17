#include "MountainRoadGeometry.hpp"
#include "BridgeStructure.hpp"
#include "TreeGeometry.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/RoadEnvironment.hpp"

namespace
{
	constexpr float kStationSpacing = 4;
	constexpr double kValleySampleDistance = 25;
	constexpr double kBarrierDrop = 1.5;
	constexpr double kWallRelief = .4;
	constexpr double kWallOffset = .10;

	Vec3 groundPosition(const World& world,Vec3 position)
	{
		position.y=world.sampleHeight(static_cast<float>(position.x),static_cast<float>(position.z));
		return position;
	}

	void doubleQuad(MeshData& mesh,Vec3 a,Vec3 b,Vec3 c,Vec3 d)
	{
		BridgeStructure::quad(mesh,a,b,c,d);BridgeStructure::quad(mesh,d,c,b,a);
	}
}

MountainRoadGeometry::Geometry MountainRoadGeometry::build(const RoadEdge& edge,const CubicBezier& curve,
	const World& world,float start,float end)
{
	Geometry result;
	for (const int side : {-1,1})
	{
		int station=0;
		for (float arc=start;arc<end-.1f;arc+=kStationSpacing,++station)
		{
			const float finish=Min(arc+kStationSpacing,end);
			if (!RoadEnvironment::outdoorSpan(world,curve,arc,finish,edge.useElevation || edge.tunnel)) { continue; }
			Vec3 a=curve.positionAt(arc),b=curve.positionAt(finish);
			a.y=RoadGeometry::surfaceY(edge,a,world.sampleHeight(static_cast<float>(a.x),static_cast<float>(a.z)));
			b.y=RoadGeometry::surfaceY(edge,b,world.sampleHeight(static_cast<float>(b.x),static_cast<float>(b.z)));
			const Vec3 rightA=tangentToRight(curve.tangentAt(arc)),rightB=tangentToRight(curve.tangentAt(finish));
			const auto rangeA=RoadGeometry::structuralRangeAt(edge,arc/curve.totalLength),rangeB=RoadGeometry::structuralRangeAt(edge,finish/curve.totalLength);
			if (!rangeA.valid || !rangeB.valid) { continue; }
			const double offsetA=side<0 ? rangeA.left : rangeA.right,offsetB=side<0 ? rangeB.left : rangeB.right;
			const Vec3 outside=a+rightA*(offsetA+side*kValleySampleDistance);
			const double drop=a.y-groundPosition(world,outside).y;
			const Vec3 first=a+rightA*(offsetA+side*kWallOffset),last=b+rightB*(offsetB+side*kWallOffset);
			if (drop>kBarrierDrop)
			{
				TreeGeometry::branch(result.rail,first,first+Vec3{0,.85,0},.055);
				constexpr std::array<double,5> heights{.53,.61,.69,.77,.85};
				constexpr std::array<double,5> folds{0,.055,0,.055,0};
				for (size_t index=0;index+1<heights.size();++index)
				{
					const Vec3 p=first+Vec3{0,heights[index],0}+rightA*(side*folds[index]);
					const Vec3 q=first+Vec3{0,heights[index+1],0}+rightA*(side*folds[index+1]);
					const Vec3 r=last+Vec3{0,heights[index+1],0}+rightB*(side*folds[index+1]);
					const Vec3 s=last+Vec3{0,heights[index],0}+rightB*(side*folds[index]);
					doubleQuad(result.rail,p,q,r,s);
				}
				if (station%3==0)
				{
					const Vec3 post=first+rightA*(side*.18);
					TreeGeometry::branch(result.posts,post,post+Vec3{0,1.02,0},.025);
					MeshData reflector=MeshData::Sphere(.065,6);reflector.translate(Float3{post+Vec3{0,.99,0}});
					BridgeStructure::append(result.reflectors,reflector);
				}
			}
			if (edge.useElevation) { continue; }
			Vec3 topA=groundPosition(world,first),topB=groundPosition(world,last);
			topA.y=Max(first.y,topA.y)+.03;topB.y=Max(last.y,topB.y)+.03;
			if (Max(topA.y-first.y,topB.y-last.y)<kWallRelief) { continue; }
			// 切土の垂直面を閉じる。舗装上に土色の大きな面を重ねない。
			doubleQuad(result.wall,first-Vec3{0,.15,0},last-Vec3{0,.15,0},topB,topA);
			doubleQuad(result.wall,a+rightA*(offsetA-side*.03),b+rightB*(offsetB-side*.03),last,first);
			const Vec3 lipA=topA+rightA*(side*.20),lipB=topB+rightB*(side*.20);
			doubleQuad(result.wall,topA,topB,lipB,lipA);
			const double height=Min(topA.y-first.y,topB.y-last.y);
			for (int patch=0;patch<5;++patch)
			{
				const double fraction=(patch+.25)/5;
				const double seed=station*13.0+patch*7.0+edge.id;
				const double bottom=.03+height*(.12+.18*(1+Sin(seed)));
				const double rise=Min(height-bottom,.25+height*(.10+.12*(1+Cos(seed*.7))));
				if (rise<=.05) { continue; }
				const Vec3 across=first.lerp(last,Min(1.0,fraction+.14))-first.lerp(last,fraction);
				const Vec3 base=first.lerp(last,fraction)-rightA*(side*.018)+Vec3{0,bottom,0};
				doubleQuad(result.moss,base,base+across,base+across*.8+Vec3{0,rise*.82,0},base+Vec3{0,rise,0});
			}
		}
	}
	return result;
}
