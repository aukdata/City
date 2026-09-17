#pragma once
#include "BridgeStructure.hpp"

/// @brief 凸な閉じた切削立体によるメッシュ差分。交点は面上に補間しUVを保持する。
namespace MeshBoolean
{
	struct HalfSpace { Vec3 normal; double offset=0; };
	using Face=Array<Vertex3D>;
	inline Face clip(const Face& polygon,const HalfSpace& plane,bool inside)
	{
		Face result;
		if (polygon.isEmpty()) { return result; }
		for (size_t i=0;i<polygon.size();++i)
		{
			const auto& a=polygon[i];const auto& b=polygon[(i+1)%polygon.size()];
			const double da=plane.normal.dot(Vec3{a.pos})-plane.offset,db=plane.normal.dot(Vec3{b.pos})-plane.offset;
			const bool keepA=inside ? da<=0 : da>=0,keepB=inside ? db<=0 : db>=0;
			if (keepA) { result<<a; }
			if (keepA!=keepB && Abs(da-db)>1e-10)
			{
				const double t=Clamp(da/(da-db),0.0,1.0);
				Vertex3D vertex=a;vertex.pos=Float3{Vec3{a.pos}.lerp(Vec3{b.pos},t)};vertex.tex=a.tex.lerp(b.tex,static_cast<float>(t));
				result<<vertex;
			}
		}
		return result;
	}
	inline void append(MeshData& mesh,const Face& face,bool reverse=false)
	{
		if (face.size()<3) { return; }
		const uint32 base=static_cast<uint32>(mesh.vertices.size());
		for (auto vertex : face) { if (reverse) { vertex.normal=-vertex.normal; } mesh.vertices<<vertex; }
		for (uint32 i=1;i+1<face.size();++i)
		{
			const Vec3 a=Vec3{face[i].pos}-Vec3{face[0].pos},b=Vec3{face[i+1].pos}-Vec3{face[0].pos};
			if (a.cross(b).lengthSq()<1e-12) { continue; }
			mesh.indices<<TriangleIndex32{base,base+(reverse ? i+1 : i),base+(reverse ? i : i+1)};
		}
	}
	inline Array<HalfSpace> planes(const MeshData& convex)
	{
		Array<HalfSpace> result;Vec3 center{0,0,0};
		for (const auto& vertex : convex.vertices) { center+=Vec3{vertex.pos}; }
		if (convex.vertices.isEmpty()) { return result; } center/=static_cast<double>(convex.vertices.size());
		for (const auto& face : convex.indices)
		{
			const Vec3 a{convex.vertices[face.i0].pos},b{convex.vertices[face.i1].pos},c{convex.vertices[face.i2].pos};
			Vec3 normal=(b-a).cross(c-a);if (normal.lengthSq()<1e-12) { continue; } normal.normalize();
			if (normal.dot(center-a)>0) { normal=-normal; }
			const double offset=normal.dot(a);bool duplicate=false;
			for (const auto& previous : result) { duplicate|=normal.dot(previous.normal)>1-1e-8 && Abs(offset-previous.offset)<.0001; }
			if (!duplicate) { result<<HalfSpace{normal,offset}; }
		}
		return result;
	}
	inline Array<Face> subtractFace(const Face& face,const Array<HalfSpace>& cutter)
	{
		// Reject disjoint faces before clipping. A mountain roof above the vault needs no tessellation.
		for (const auto& plane : cutter)
		{
			bool outside=true;
			for (const auto& vertex : face) { outside&=plane.normal.dot(Vec3{vertex.pos})-plane.offset>=.00001; }
			if (outside) { return {face}; }
		}
		Face remaining=face;Array<Face> result;
		for (const auto& plane : cutter)
		{
			bool allInside=true,allOutside=true;
			for (const auto& vertex : remaining) { const double d=plane.normal.dot(Vec3{vertex.pos})-plane.offset;allInside&=d<=.00001;allOutside&=d>=.00001; }
			if (allOutside) { result<<remaining;return result; }
			if (allInside) { continue; }
			const auto outside=clip(remaining,plane,false);if (outside.size()>=3) { result<<outside; }
			remaining=clip(remaining,plane,true);if (remaining.size()<3) { return result; }
		}
		return result;
	}
	/// @brief 凸ソリッド同士の差分。切削側の内壁も追加するので開口が閉じた厚みを持つ。
	inline MeshData difference(const MeshData& solid,const MeshData& cutter)
	{
		MeshData result;const auto cutPlanes=planes(cutter),solidPlanes=planes(solid);
		for (const auto& face : solid.indices)
		{
			for (const auto& fragment : subtractFace({solid.vertices[face.i0],solid.vertices[face.i1],solid.vertices[face.i2]},cutPlanes)) { append(result,fragment); }
		}
		for (const auto& face : cutter.indices)
		{
			Face fragment{cutter.vertices[face.i0],cutter.vertices[face.i1],cutter.vertices[face.i2]};
			for (const auto& plane : solidPlanes) { fragment=clip(fragment,plane,true);if (fragment.size()<3) { break; } }
			append(result,fragment,true);
		}
		return result;
	}
}
