#pragma once
#include "BridgeStructure.hpp"
#include "MeshBoolean.hpp"
#include "../road/RoadEnvironment.hpp"

/// @brief 道路・鉄道共通の馬蹄形覆工。内面と外面、厚みのある坑門を独立して作る。
class RoadNetwork;
namespace TunnelGeometry
{
	struct Opening { Array<Vec2> footprint; RectF bounds; float floor=0; Array<MeshBoolean::HalfSpace> planes; };
	struct Geometry { MeshData lining, lights; Array<Opening> openings; };
	struct Section { Vec3 center; double radius=0; Geometry geometry; };
	/// @brief 道路グラフの地下交差点に分岐空間を作り、各坑道の壁を開口する。
	[[nodiscard]] Array<Section> buildRoadNetwork(const World& world,const RoadNetwork& roads);
	inline constexpr int kArchSegments = 32;
	/// @brief 同一の測点断面で覆工と切削を連続させる。GPU資源の生成は描画側で行う。
	[[nodiscard]] Geometry build(const CubicBezier& curve,const World& world,double width,bool railwayTrack);
	/// @brief 同じ馬蹄断面を閉じたブーリアン用切削ソリッドへ押し出す。
	inline MeshData cutter(Vec3 a,Vec3 b,double halfWidth,double crown)
	{
		MeshData mesh;const Vec3 delta=b-a;
		const Vec3 right=Vec3{delta.z,0,-delta.x}.normalized();
		Array<Vec2> section{{-halfWidth,-.5},{halfWidth,-.5}};
		for (int i=0;i<=32;++i) { const double angle=i*Math::Pi/32;section<<Vec2{Cos(angle)*halfWidth,2.2+Sin(angle)*(crown-2.2)}; }
		for (size_t i=0;i<section.size();++i)
		{
			const Vec2 p=section[i],q=section[(i+1)%section.size()];
			BridgeStructure::quad(mesh,a+right*p.x+Vec3{0,p.y,0},a+right*q.x+Vec3{0,q.y,0},b+right*q.x+Vec3{0,q.y,0},b+right*p.x+Vec3{0,p.y,0});
		}
		for (const Vec3 center : {a,b})
		{
			MeshBoolean::Face face;
			for (const Vec2 p : section) { face<<Vertex3D{Float3{center+right*p.x+Vec3{0,p.y,0}},Float3{0,1,0},Float2{p}}; }
			MeshBoolean::append(mesh,face,center==a);
		}
		// All solid faces point outwards, independently of cross-section winding.
		const Vec3 center=(a+b)*.5+Vec3{0,crown*.45,0};
		for (auto& face : mesh.indices)
		{
			const Vec3 p{mesh.vertices[face.i0].pos},q{mesh.vertices[face.i1].pos},r{mesh.vertices[face.i2].pos};
			if ((q-p).cross(r-p).dot((p+q+r)/3-center)<0) { std::swap(face.i1,face.i2); }
		}
		mesh.computeNormals();return mesh;
	}
	/// @brief コンクリート坑門から断面ソリッドを減算した厚みのある開口。
	inline MeshData portal(Vec3 center,Vec3 direction,double half,double crown)
	{
		const Vec3 right=Vec3{direction.z,0,-direction.x}.normalized(),along=Vec3{direction.x,0,direction.z}.normalized();
		MeshData block=MeshData::Box(Float3{0,static_cast<float>(crown*.5),0},Float3{static_cast<float>(2*half+2.4),static_cast<float>(crown+2),1.6f});
		for (auto& vertex : block.vertices) { const Vec3 p{vertex.pos}; vertex.pos=Float3{center+right*p.x+Vec3{0,p.y,0}+along*p.z}; }
		block.computeNormals();return MeshBoolean::difference(block,cutter(center-along*2,center+along*2,half,crown));
	}
	inline MeshData lining(Vec3 a,Vec3 b,Vec3 rightA,Vec3 rightB,double halfWidth,double crown,bool portal=false)
	{
		MeshData mesh;
		const auto section=[&](Vec3 center,Vec3 right,int index,double thickness)
		{
			const double angle=index*Math::Pi/kArchSegments;
			return center+right*(Cos(angle)*(halfWidth+thickness))+Vec3{0,2.2+Sin(angle)*(crown-2.2+thickness),0};
		};
		for (int i=0;i<kArchSegments;++i)
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
			const Vec3 p=a+rightA*(halfWidth*side)-Vec3{0,.35,0},q=b+rightB*(halfWidth*side)-Vec3{0,.35,0};
			BridgeStructure::quad(mesh,p,q,q+Vec3{0,2.55,0},p+Vec3{0,2.55,0});
			BridgeStructure::quad(mesh,p,p+Vec3{0,2.55,0},q+Vec3{0,2.55,0},q);
		}
		BridgeStructure::quad(mesh,a-rightA*halfWidth-Vec3{0,.35,0},b-rightB*halfWidth-Vec3{0,.35,0},b+rightB*halfWidth-Vec3{0,.35,0},a+rightA*halfWidth-Vec3{0,.35,0});
		return mesh;
	}
}
