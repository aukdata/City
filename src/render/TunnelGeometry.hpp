#pragma once
#include "BridgeStructure.hpp"

/// @brief 道路・鉄道共通の馬蹄形覆工。内面と外面、厚みのある坑門を独立して作る。
namespace TunnelGeometry
{
	struct Opening { Array<Vec2> footprint; RectF bounds; float floor=0; };
	inline MeshData lining(Vec3 a,Vec3 b,Vec3 rightA,Vec3 rightB,double halfWidth,double crown,bool portal=false)
	{
		MeshData mesh;
		const auto section=[&](Vec3 center,Vec3 right,int index,double thickness)
		{
			const double angle=index*Math::Pi/16;
			return center+right*(Cos(angle)*(halfWidth+thickness))+Vec3{0,2.2+Sin(angle)*(crown-2.2+thickness),0};
		};
		for (int i=0;i<16;++i)
		{
			const Vec3 p=section(a,rightA,i,0),q=section(a,rightA,i+1,0),r=section(b,rightB,i+1,0),s=section(b,rightB,i,0);
			BridgeStructure::quad(mesh,p,s,r,q); // inward facing vault
			const double thickness=portal ? .65 : .35;
			const Vec3 pp=section(a,rightA,i,thickness),qq=section(a,rightA,i+1,thickness),rr=section(b,rightB,i+1,thickness),ss=section(b,rightB,i,thickness);
			BridgeStructure::quad(mesh,pp,qq,rr,ss);
			if (portal) { BridgeStructure::quad(mesh,p,pp,qq,q); BridgeStructure::quad(mesh,s,r,rr,ss); }
		}
		for (const int side : {-1,1})
		{
			const Vec3 p=a+rightA*(halfWidth*side),q=b+rightB*(halfWidth*side);
			BridgeStructure::quad(mesh,p,q,q+Vec3{0,2.2,0},p+Vec3{0,2.2,0});
			BridgeStructure::quad(mesh,p,p+Vec3{0,2.2,0},q+Vec3{0,2.2,0},q);
		}
		BridgeStructure::quad(mesh,a-rightA*halfWidth-Vec3{0,.35,0},b-rightB*halfWidth-Vec3{0,.35,0},b+rightB*halfWidth-Vec3{0,.35,0},a+rightA*halfWidth-Vec3{0,.35,0});
		return mesh;
	}
}
