#include "TunnelGeometry.hpp"
#include "../road/RoadGeometry.hpp"
#include "../road/RoadNetwork.hpp"
#include "../debug/DebugLog.hpp"

namespace
{
	constexpr double kCutOverlap = .25;
	constexpr double kCutClearance = .08;
	constexpr double kMaintenanceWidth = .7;

	/// @brief Float3メッシュから平面を逆算せず、倍精度の断面と縦断勾配から切削平面を作る。
	Array<MeshBoolean::HalfSpace> excavationPlanes(Vec3 start,Vec3 end,double half,double crown,bool covered)
	{
		const Vec3 delta=end-start;
		const double length=Vec2{delta.x,delta.z}.length();
		const Vec3 along{delta.x/length,0,delta.z/length},right{along.z,0,-along.x};
		const double grade=delta.y/length;
		Array<Vec2> section{{-half,-.5},{half,-.5}};
		if (covered)
		{
			for (int i=0;i<=TunnelGeometry::kArchSegments;++i)
			{
				const double angle=i*Math::Pi/TunnelGeometry::kArchSegments;
				section<<Vec2{Cos(angle)*half,2.2+Sin(angle)*(crown-2.2)};
			}
		}
		else { section<<Vec2{half,1000}<<Vec2{-half,1000}; }
		Array<MeshBoolean::HalfSpace> result{{-along,-along.dot(start)},{along,along.dot(end)}};
		for (size_t i=0;i<section.size();++i)
		{
			const Vec2 point=section[i],edge=section[(i+1)%section.size()]-point;
			const Vec2 outward{edge.y,-edge.x};
			const Vec3 normal=(right*outward.x+Vec3{0,outward.y,0}-along*(outward.y*grade)).normalized();
			result<<MeshBoolean::HalfSpace{normal,normal.dot(start+right*point.x+Vec3{0,point.y,0})};
		}
		return result;
	}
}

TunnelGeometry::Geometry TunnelGeometry::build(const CubicBezier& curve,const World& world,double width,bool railwayTrack)
{
	Geometry result;
	if (curve.totalLength<.01f) { return result; }
	const double crown=railwayTrack ? RoadEnvironment::kRailTunnelCrown : RoadEnvironment::kRoadTunnelCrown;
	const double half=width*.5+kMaintenanceWidth;
	const double surfaceLift=railwayTrack ? .15 : kRoadSurfaceLift;
	const int count=Max(2,static_cast<int>(Ceil(curve.totalLength/RoadEnvironment::kSectionStep)));
	Array<float> cuts;
	const auto covered=[&](float arc) { return RoadEnvironment::coveredAt(world,curve.positionAt(arc),true,crown); };
	for (int i=0;i<=count;++i) { cuts<<curve.totalLength*i/count; }
	for (int i=0;i<count;++i)
	{
		float low=curve.totalLength*i/count,high=curve.totalLength*(i+1)/count;
		const bool before=covered(low);
		if (before==covered(high)) { continue; }
		for (int iteration=0;iteration<20;++iteration)
		{
			const float middle=(low+high)*.5f;
			if (covered(middle)==before) { low=middle; } else { high=middle; }
		}
		const float arc=(low+high)*.5f;cuts<<arc;
		BridgeStructure::append(result.lining,portal(curve.positionAt(arc)+Vec3{0,surfaceLift,0},curve.tangentAt(arc),half,crown));
	}
	cuts.sort();
	for (size_t i=0;i+1<cuts.size();++i)
	{
		const float start=cuts[i],end=cuts[i+1],middle=(start+end)*.5f;
		if (end-start<.01f) { continue; }
		const Vec3 center=curve.positionAt(middle);
		const double depth=world.sampleHeight(static_cast<float>(center.x),static_cast<float>(center.z))-center.y;
		if (depth<.1) { continue; }
		const Vec3 a=curve.positionAt(start)+Vec3{0,surfaceLift,0},b=curve.positionAt(end)+Vec3{0,surfaceLift,0};
		const Vec3 rightA=tangentToRight(curve.tangentAt(start)),rightB=tangentToRight(curve.tangentAt(end));
		const Vec3 along=(b-a).normalized(),right=tangentToRight(along);
		const bool enclosed=covered(middle);
		if (enclosed)
		{
			BridgeStructure::append(result.lining,lining(a,b,rightA,rightB,half,crown));
			// 舗装端と壁を連続した点検路で結び、下には閉じたインバートを残す。
			for (const int side : {-1,1})
			{
				const double inner=(width*.5-.04)*side,outer=half*side;
				const Vec3 p=a+rightA*inner,q=b+rightB*inner,r=b+rightB*outer,s=a+rightA*outer;
				BridgeStructure::quad(result.lining,p,q,r,s);BridgeStructure::quad(result.lining,s,r,q,p);
			}
		}
		else
		{
			for (const int side : {-1,1})
			{
				const Vec3 p=a+rightA*(half*side),q=b+rightB*(half*side);
				Vec3 topP=p,topQ=q;
				topP.y=Max(p.y,static_cast<double>(world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))))+.15;
				topQ.y=Max(q.y,static_cast<double>(world.sampleHeight(static_cast<float>(q.x),static_cast<float>(q.z))))+.15;
				BridgeStructure::quad(result.lining,p,q,topQ,topP);BridgeStructure::quad(result.lining,topP,topQ,q,p);
			}
		}
		Opening opening;
		const Vec3 from=a-along*kCutOverlap,to=b+along*kCutOverlap;
		const double cutHalf=half+kCutClearance;
		for (const Vec3 point : {from-right*cutHalf,to-right*cutHalf,to+right*cutHalf,from+right*cutHalf}) { opening.footprint<<Vec2{point.x,point.z}; }
		Vec2 low{1e9,1e9},high{-1e9,-1e9};
		for (const Vec2 point : opening.footprint)
		{
			low.x=Min(low.x,point.x);low.y=Min(low.y,point.y);high.x=Max(high.x,point.x);high.y=Max(high.y,point.y);
		}
		opening.bounds=RectF{low,high-low};opening.floor=static_cast<float>(Min(a.y,b.y)-.5);
		opening.planes=excavationPlanes(from,to,cutHalf,crown+kCutClearance,enclosed);
		result.openings<<std::move(opening);
		if (enclosed && i%8==0)
		{
			for (const int side : {-1,1})
			{
				BridgeStructure::append(result.lights,MeshData::Box(Float3{a+rightA*(side*(half-.22))+Vec3{0,3,0}},Float3{.15,.22,1.5}));
			}
		}
	}
	return result;
}

namespace
{
	MeshData excludeTunnelVolumes(const MeshData& mesh,const Array<TunnelGeometry::Opening>& volumes)
	{
		MeshData result;
		for (const auto& triangle : mesh.indices)
		{
			const auto a=mesh.vertices[triangle.i0],b=mesh.vertices[triangle.i1],c=mesh.vertices[triangle.i2];
			const RectF bounds{Vec2{Min({a.pos.x,b.pos.x,c.pos.x}),Min({a.pos.z,b.pos.z,c.pos.z})},
				Vec2{Max({a.pos.x,b.pos.x,c.pos.x})-Min({a.pos.x,b.pos.x,c.pos.x}),Max({a.pos.z,b.pos.z,c.pos.z})-Min({a.pos.z,b.pos.z,c.pos.z})}};
			Array<MeshBoolean::Face> fragments{{a,b,c}};
			for (const auto& volume : volumes)
			{
				if (!bounds.stretched(.01).intersects(volume.bounds)) { continue; }
				Array<MeshBoolean::Face> next;
				for (const auto& face : fragments) { next.append(MeshBoolean::subtractFace(face,volume.planes)); }
				fragments=std::move(next); if (fragments.isEmpty()) { break; }
			}
			for (const auto& face : fragments) { MeshBoolean::append(result,face); }
		}
		return result;
	}
	TunnelGeometry::Geometry junctionChamber(Vec3 center,double radius)
	{
		using namespace TunnelGeometry;
		constexpr int kSides=32;
		constexpr double kHeight=RoadEnvironment::kRoadTunnelCrown+1;
		Geometry result;Opening opening;opening.floor=static_cast<float>(center.y-.5);
		opening.bounds={center.x-radius,center.z-radius,radius*2,radius*2};
		opening.planes={{Vec3{0,-1,0},-center.y+.5},{Vec3{0,1,0},center.y+kHeight+.08}};
		for (int i=0;i<kSides;++i)
		{
			const double angle=i*Math::TwoPi/kSides,next=(i+1)*Math::TwoPi/kSides;
			const Vec3 a=center+Vec3{Cos(angle)*radius,0,Sin(angle)*radius},b=center+Vec3{Cos(next)*radius,0,Sin(next)*radius};
			const Vec3 normal{Cos((angle+next)*.5),0,Sin((angle+next)*.5)};
			opening.footprint<<Vec2{a.x,a.z};opening.planes<<MeshBoolean::HalfSpace{normal,normal.dot(a)};
			BridgeStructure::quad(result.lining,a-Vec3{0,.35,0},a+Vec3{0,kHeight,0},b+Vec3{0,kHeight,0},b-Vec3{0,.35,0});
			for (const double y : {-.35,kHeight})
			{
				const Float3 up{0,y<0 ? 1.0f : -1.0f,0};
				MeshBoolean::Face face;
				for (const Vec3 point : {center+Vec3{0,y,0},a+Vec3{0,y,0},b+Vec3{0,y,0}})
				{
					face<<Vertex3D{Float3{point},up,Float2{static_cast<float>((point.x-center.x)*.5),static_cast<float>((point.z-center.z)*.5)}};
				}
				MeshBoolean::append(result.lining,face,y<0);
			}
		}
		result.openings<<std::move(opening);
		for (int i=0;i<4;++i)
		{
			const double angle=i*Math::HalfPi;
			BridgeStructure::append(result.lights,MeshData::Box(Float3{center+Vec3{Cos(angle)*radius*.5,kHeight-.2,Sin(angle)*radius*.5}},Float3{1.6f,.18f,.5f}));
		}
		return result;
	}
}
Array<TunnelGeometry::Section> TunnelGeometry::buildRoadNetwork(const World& world,const RoadNetwork& roads)
{
	Array<Section> result;HashTable<int,size_t> sections;HashTable<int,Array<Opening>> cuts;
	for (const auto& edge : roads.edges())
	{
		if (edge.id<0 || !(edge.useElevation || edge.tunnel) || !edge.isRoadbedBuilt()) { continue; }
		const auto curve=roads.getBezier(edge.id);if (!curve) { continue; }
		auto geometry=build(*curve,world,edge.totalWidth(),false);if (geometry.openings.isEmpty()) { continue; }
		sections[edge.id]=result.size(); result<<Section{curve->positionAt(curve->totalLength*.5f),curve->totalLength*.5,std::move(geometry)};
	}
	int junctions=0;
	for (const auto& node : roads.nodes())
	{
		if (node.id<0 || node.attachments.size()<3 || !RoadEnvironment::coveredAt(world,node.position,true)) { continue; }
		Array<int> arms;double radius=12;
		for (const auto& attachment : node.attachments)
		{
			const auto* edge=roads.getEdge(attachment.edgeId);if (!edge || !sections.contains(edge->id)) { continue; }
			arms<<edge->id;radius=Max(radius,Max(edge->totalWidth()*2.0,static_cast<double>(edge->nodeA==node.id ? edge->cutoffA : edge->cutoffB)+2));
		}
		if (arms.size()<3) { continue; }
		const Vec3 center=node.position+Vec3{0,kRoadSurfaceLift,0};auto room=junctionChamber(center,radius);
		Array<Opening> passages;
		for (const int id : arms)
		{
			cuts[id].append(room.openings);
			for (const auto& opening : result[sections[id]].geometry.openings)
			{
				if (opening.bounds.intersects(room.openings.front().bounds.stretched(2))) { passages<<opening; }
			}
		}
		room.lining=excludeTunnelVolumes(room.lining,passages);
		result<<Section{center,radius,std::move(room)};++junctions;
	}
	for (const auto& [id,volumes] : cuts)
	{
		auto& geometry=result[sections[id]].geometry;
		geometry.lining=excludeTunnelVolumes(geometry.lining,volumes);
		geometry.lights=excludeTunnelVolumes(geometry.lights,volumes);
	}
	DBG_LOG(U"[TunnelJunction] rooms={} roadSections={}"_fmt(junctions,sections.size()));
	return result;
}
