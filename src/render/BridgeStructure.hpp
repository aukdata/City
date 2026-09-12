#pragma once
#include "../road/BezierUtil.hpp"

/// @brief Shared completed and in-progress concrete geometry, in metres.
namespace BridgeStructure
{
	inline void append(MeshData& target,const MeshData& mesh)
	{
		const uint32 offset=static_cast<uint32>(target.vertices.size());target.vertices.append(mesh.vertices);
		for(const auto& t:mesh.indices) target.indices << TriangleIndex32{offset+t.i0,offset+t.i1,offset+t.i2};
	}
	inline void quad(MeshData& mesh,Vec3 a,Vec3 b,Vec3 c,Vec3 d)
	{
		const Float3 normal{(b-a).cross(c-a).normalized()};const uint32 n=static_cast<uint32>(mesh.vertices.size());
		const double width=a.distanceFrom(d),height=a.distanceFrom(b);
		mesh.vertices << Vertex3D{Float3{a},normal,Float2{0,0}} << Vertex3D{Float3{b},normal,Float2{0,static_cast<float>(height*.5)}}
			<< Vertex3D{Float3{c},normal,Float2{static_cast<float>(width*.5),static_cast<float>(height*.5)}} << Vertex3D{Float3{d},normal,Float2{static_cast<float>(width*.5),0}};
		mesh.indices << TriangleIndex32{n,n+1,n+2} << TriangleIndex32{n,n+2,n+3};
	}
	inline MeshData pier(Vec3 position,Vec3 right,double ground,double top,double roadWidth,double fraction=1)
	{
		MeshData mesh;const Vec3 along{-right.z,0,right.x};
		auto point=[&](double x,double y,double z){return Vec3{position.x,y,position.z}+right*x+along*z;};
		const double height=Max(.5,top-ground),cap=Min(.95,height*.2),columnTop=top-cap;
		auto ring=[&](double y,double width,double depth)
		{
			const double w=width*.5,d=depth*.5,chamfer=.16;
			return std::array<Vec3,8>{point(-w+chamfer,y,-d),point(w-chamfer,y,-d),point(w,y,-d+chamfer),point(w,y,d-chamfer),
				point(w-chamfer,y,d),point(-w+chamfer,y,d),point(-w,y,d-chamfer),point(-w,y,-d+chamfer)};
		};
		auto join=[&](const auto& a,const auto& b)
		{
			for(size_t i=0;i<8;++i) {const size_t j=(i+1)%8;quad(mesh,a[i],b[i],b[j],a[j]);}
			const Vec3 center=(b[0]+b[4])*.5;
			for(size_t i=0;i<8;++i)
			{
				const uint32 n=static_cast<uint32>(mesh.vertices.size());
				for(Vec3 p:{center,b[(i+1)%8],b[i]}) mesh.vertices << Vertex3D{Float3{p},Float3{0,1,0},Float2{static_cast<float>(p.x*.5),static_cast<float>(p.z*.5)}};
				mesh.indices << TriangleIndex32{n,n+1,n+2};
			}
		};
		join(ring(ground-.25,4.8,4.2),ring(ground+.16,4.8,4.2));
		const double columnHeight=Max(.2,columnTop-ground-.15);
		const double poured=Clamp(fraction,0.0,1.0);
		if(poured>.01) join(ring(ground+.15,2.45,1.85),ring(ground+.15+columnHeight*poured,2.1,1.65));
		if(fraction>=.99)
		{
			const double crosshead=Clamp(roadWidth*.78,3.6,12.0);
			join(ring(columnTop,2.1,1.65),ring(columnTop+cap*.65,crosshead,2.0));
			join(ring(columnTop+cap*.65,crosshead,2.0),ring(top,crosshead,2.0));
		}
		return mesh;
	}
	inline MeshData girders(const CubicBezier& curve,double width,double endArc)
	{
		MeshData mesh;
		for(double arc=0;arc<endArc-.01;arc+=5)
		{
			const Vec3 a=curve.positionAt(static_cast<float>(arc)),b=curve.positionAt(static_cast<float>(Min(endArc,arc+5)));
			const Vec3 right=tangentToRight(curve.tangentAt(static_cast<float>((arc+Min(endArc,arc+5))*.5)));
			for(double offset:{-width*.27,width*.27})
			{
				for(const auto& layer:std::array<Vec3,3>{Vec3{.46,.10,-1.12},Vec3{.12,.86,-.65},Vec3{.46,.10,-.20}})
				{
					const Vec3 p=a+right*offset+Vec3{0,layer.z,0},q=b+right*offset+Vec3{0,layer.z,0};
					const Vec3 side=right*layer.x*.5,up{0,layer.y*.5,0};
					quad(mesh,p-side-up,q-side-up,q-side+up,p-side+up);
					quad(mesh,q+side-up,p+side-up,p+side+up,q+side+up);
					quad(mesh,p-side+up,q-side+up,q+side+up,p+side+up);
					quad(mesh,q-side-up,p-side-up,p+side-up,q+side-up);
					quad(mesh,p-side-up,p-side+up,p+side+up,p+side-up);
					quad(mesh,q+side-up,q+side+up,q-side+up,q-side-up);
				}
			}
		}
		return mesh;
	}
}
