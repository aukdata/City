#include "RoadRenderer.hpp"
#include "BridgeStructure.hpp"
#include "../road/RoadGeometry.hpp"
#include "../debug/DebugLog.hpp"

namespace
{
	void append(MeshData& target, const MeshData& source)
	{
		const uint32 offset = static_cast<uint32>(target.vertices.size());
		target.vertices.append(source.vertices);
		for (const auto& t : source.indices) target.indices << TriangleIndex32{t.i0+offset,t.i1+offset,t.i2+offset};
	}
	/// @brief Compacted soil / crushed aggregate with shallow wheel ruts and a visible cut edge.
	MeshData earthSurface(const CubicBezier& curve,const World& world,const RoadEdge& edge,double end,bool aggregate)
	{
		MeshData mesh;
		const int alongCount=Clamp(static_cast<int>(Ceil(end)),2,700);
		const int acrossCount=Clamp(static_cast<int>(Ceil(edge.totalWidth()/.4)),8,72);
		Array<Vec3> previous;
		for(int row=0;row<=alongCount;++row)
		{
			const float arc=static_cast<float>(end*row/alongCount);
			const Vec3 center=curve.positionAt(arc),right=tangentToRight(curve.tangentAt(arc));
			const auto range=RoadGeometry::structuralRangeAt(edge,arc/Max(.01f,curve.totalLength));
			Array<Vec3> current;
			for(int col=0;col<=acrossCount;++col)
			{
				const double lateral=Math::Lerp(range.left,range.right,static_cast<float>(col)/acrossCount);
				Vec3 p=center+right*lateral;
				const double trackDistance=Min(Abs(lateral-1.1),Abs(lateral+1.1));
				const double track=Exp(-trackDistance*trackDistance/.065);
				const double grain=Sin(p.x*5.7+p.z*3.8)*Sin(p.x*2.1-p.z*4.3);
				p.y=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))+kRoadSurfaceLift
					+(aggregate ? -.024+grain*.004 : -.095+grain*.014-track*(.035+.004*Sin(arc*22)));
				current << p;
			}
			if(row>0)
			{
				for(int col=0;col<acrossCount;++col)
				{
					const uint32 base=static_cast<uint32>(mesh.vertices.size());
					BridgeStructure::quad(mesh,previous[col],current[col],current[col+1],previous[col+1]);
					for(uint32 i=base;i<mesh.vertices.size();++i)
					{
						auto& v=mesh.vertices[i];v.tex={v.pos.x*.5f,v.pos.z*.5f};
					}
				}
				for(int side:{0,acrossCount})
				{
					const Vec3 a=previous[side],b=current[side];
					if(side==0) BridgeStructure::quad(mesh,a,a-Vec3{0,.20,0},b-Vec3{0,.20,0},b);
					else BridgeStructure::quad(mesh,b,b-Vec3{0,.20,0},a-Vec3{0,.20,0},a);
				}
			}
			previous=std::move(current);
		}
		return mesh;
	}
	struct SiteModel
	{
		std::array<MeshData, 7> batches;
		Vec3 origin, right, forward;
		Vec3 point(Vec3 p) const { return origin+right*p.x+Vec3{0,p.y,0}+forward*p.z; }
		void box(int material, Vec3 center, Vec3 size)
		{
			auto mesh = MeshData::Box(Float3{center},Float3{size});
			for (auto& vertex : mesh.vertices)
			{
				vertex.pos = Float3{point(Vec3{vertex.pos})};
				vertex.normal = Float3{right*vertex.normal.x+Vec3{0,vertex.normal.y,0}+forward*vertex.normal.z};
			}
			append(batches[material],mesh);
		}
		void beam(int material, Vec3 a, Vec3 b, double width)
		{
			const Vec3 start=point(a),end=point(b),axis=(end-start).normalized();
			Vec3 side=axis.cross(Vec3{0,1,0});
			if (side.lengthSq()<.001) side={1,0,0}; else side.normalize();
			const Vec3 up=side.cross(axis).normalized();
			auto mesh=MeshData::Box(Float3{0,0,0},Float3{static_cast<float>(width),static_cast<float>(width),static_cast<float>((end-start).length())});
			for (auto& v:mesh.vertices)
			{
				v.pos=Float3{(start+end)*.5+side*v.pos.x+up*v.pos.y+axis*v.pos.z};
				v.normal=Float3{side*v.normal.x+up*v.normal.y+axis*v.normal.z};
			}
			append(batches[material],mesh);
		}
		void lineCart()
		{
			box(0,{0,.5,0},{.6,.75,.9}); box(1,{-.4,.2,0},{.13,.35,.8}); box(1,{.4,.2,0},{.13,.35,.8});
			beam(2,{0,.9,-.25},{0,1.2,-.85},.06); box(4,{0,.03,.8},{.18,.05,.5});
			box(5,{0,1.12,-1.25},{.5,.65,.3}); box(4,{0,1.59,-1.25},{.27,.23,.27});
			box(1,{-.15,.45,-1.25},{.16,.65,.18}); box(1,{.15,.45,-1.25},{.16,.65,.18});
		}
	};

}

void RoadRenderer::drawConstruction(const RoadEdge& edge, const RoadNetwork& network, const World& world, bool close)
{
	using Stage=RoadConstruction::Stage;
	const auto progress=RoadConstruction::progress(network,edge,m_constructionNow);
	const int step=Clamp(static_cast<int>(progress.fraction*20),0,20);
	const int key=static_cast<int>(progress.stage)*21+step;
	auto& cache=m_constructionCache[edge.id];
	if (cache.key!=key)
	{
		Stopwatch timer{StartImmediately::Yes};
		cache=ConstructionCache{};cache.key=key;
		++m_geometryRevision;
		const auto curve=network.getBezier(edge.id);
		if(!curve) return;

		const double fraction=static_cast<double>(step)/20;
		const auto ra=RoadGeometry::structuralRangeAt(edge,0),rb=RoadGeometry::structuralRangeAt(edge,1);
		auto strip=[&](double start,double end,double height,const Texture* texture,ColorF color,bool elevated)
		{
			if(end-start<.03) return;
			MeshData data=buildStripMeshTapered(*curve,world,ra.left,ra.right,rb.left,rb.right,static_cast<float>(height),
				static_cast<float>(start),static_cast<float>(end),2.0f,elevated ? .10f : .08f,elevated);
			for(auto& v:data.vertices) v.tex=Float2{v.pos.x*.5f,v.pos.z*.5f};
			Mesh mesh{data};
			cache.surfaces << PartMeshEntry{{mesh,mesh},color,texture};
		};
		const int phase=static_cast<int>(progress.stage);
		if(!edge.useElevation)
		{
			Mesh soil{earthSurface(*curve,world,edge,curve->totalLength,false)};
			cache.surfaces << PartMeshEntry{{soil,soil},ColorF{1},&m_constructionSoil};
			if(phase>=2 && (phase>2 || fraction>.001))
			{
				Mesh gravel{earthSurface(*curve,world,edge,curve->totalLength*(phase==2 ? fraction : 1),true)};
				cache.surfaces << PartMeshEntry{{gravel,gravel},ColorF{.84},&m_constructionGravel};
			}
		}
		SiteModel site{{},Vec3{0,0,0},Vec3{1,0,0},Vec3{0,0,1}};
		if(edge.useElevation)
		{
			for(const auto& object:network.objects())
			{
				if(object.id<0 || object.parentEdgeId!=edge.id || object.type!=RoadObjectType::Pier) continue;
				const Vec3 p=curve->positionAt(object.arcPos);
				const double ground=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
				const double top=p.y+kRoadSurfaceLift-1.15;
				const double height=Max(.1,top-ground);
				site.box(6,{p.x,ground+.025,p.z},{6,.05,6});
				if(phase>=1)
				{
					const double poured=phase==1 ? Clamp((fraction-.20)/.75,0.0,1.0) : 1;
					const Vec3 right=tangentToRight(curve->tangentAt(object.arcPos));
					Mesh concrete{BridgeStructure::pier(p,right,ground,top,edge.totalWidth(),poured)};
					cache.surfaces << PartMeshEntry{{concrete,concrete},ColorF{1},&m_constructionConcrete};
					if(phase==1 && fraction<.95)
					{
						const Vec3 forward{-right.z,0,right.x};
						auto point=[&](double x,double y,double z){return Vec3{p.x,ground+y,p.z}+right*x+forward*z;};
						const double steelTop=Min(height-.8,Max(1.5,height*fraction+2.5));
						for(int i=0;i<6;++i) for(int side:{-1,1})
						{
							const double x=-.92+i*.368;
							site.beam(1,point(x,.25,side*.66),point(x,steelTop,side*.66),.04);
						}
						for(double y=.35;y<steelTop;y+=.36)
						{
							site.beam(1,point(-.98,y,-.72),point(.98,y,-.72),.026);
							site.beam(1,point(-.98,y,.72),point(.98,y,.72),.026);
							site.beam(1,point(-.98,y,-.72),point(-.98,y,.72),.026);
							site.beam(1,point(.98,y,-.72),point(.98,y,.72),.026);
						}
						// Scaffold uprights and rails follow the height of the active lift.
						for(int x:{-1,1}) for(int z:{-1,1})
							site.beam(2,point(x*1.7,.1,z*1.4),point(x*1.7,steelTop+.8,z*1.4),.055);
						for(double y=1.6;y<steelTop+.8;y+=1.8) for(int side:{-1,1})
						{
							site.beam(2,point(-1.7,y,side*1.4),point(1.7,y,side*1.4),.055);
							site.beam(2,point(side*1.7,y,-1.4),point(side*1.7,y,1.4),.055);
						}
						if(fraction>.25 && height>2.5)
						{
							const double bottom=Max(.18,height*poured-1.4),formHeight=Min(2.5,height-bottom-.9);
							const Vec3 old=site.origin,oldRight=site.right,oldForward=site.forward;
							site.origin={p.x,ground,p.z};site.right=right;site.forward=forward;
							for(int side:{-1,1})
							{
								site.box(6,{side*1.22,bottom+formHeight*.5,0},{.09,formHeight,1.98});
								site.box(6,{0,bottom+formHeight*.5,side*.99},{2.50,formHeight,.09});
								for(double y=bottom+.15;y<bottom+formHeight;y+=.55)
								{
									site.box(2,{side*1.30,y,0},{.08,.12,2.25});site.box(2,{0,y,side*1.07},{2.72,.12,.08});
								}
							}
							site.origin=old;site.right=oldRight;site.forward=oldForward;
						}
					}
				}
			}
			if(phase>=2)
			{
				const double length=curve->totalLength*(phase==2 ? fraction : 1);
				strip(0,length,-.06,&m_constructionConcrete,ColorF{.8},true);
				Mesh girders{BridgeStructure::girders(*curve,edge.totalWidth(),length)};
				cache.surfaces << PartMeshEntry{{girders,girders},ColorF{.24,.29,.31}.removeSRGBCurve(),nullptr};
			}
		}
		if(phase>=3)
		{
			const float end=curve->totalLength*static_cast<float>(phase==3 ? fraction : 1);
			if(end>.1f) cache.surfaces.append(buildPartMeshes(network,edge,*curve,world,0,curve->totalLength-end));
		}
		if(phase>=4)
		{
			const float end=curve->totalLength*static_cast<float>(phase==4 ? fraction : 1);
			if(end>.1f) cache.details.append(buildEdgeRoadMarkingBatches(network,edge,*curve,world,0,curve->totalLength-end));
		}
		// Construction fences and machinery have cached, grouped geometry.
		for(float arc:{Min(2.0f,curve->totalLength*.15f),Max(0.0f,curve->totalLength-2)})
		{
			const Vec3 p=curve->positionAt(arc);const Vec3 right=tangentToRight(curve->tangentAt(arc));
			const double ground=world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z));
			site.origin={p.x,edge.useElevation && (phase>=3 || (phase==2 && arc<curve->totalLength*fraction)) ? p.y+.1 : ground+.1,p.z};site.right=right;site.forward=Vec3{-right.z,0,right.x};
			const double width=Max(2.0,static_cast<double>(edge.totalWidth())*.85);
			site.box(4,{0,.9,0},{width,.26,.10});
			for(double x=-width*.5;x<width*.5;x+=1) site.box(0,{x,.9,-.065},{.5,.26,.03});
			for(double x:{-width*.42,width*.42}) {site.box(2,{x,.5,0},{.07,1,.07});site.box(1,{x,.04,0},{.45,.08,.5});}
		}
		const float machineArc=Clamp(curve->totalLength*static_cast<float>(fraction),Min(8.0f,curve->totalLength*.2f),Max(8.0f,curve->totalLength-8));
		Vec3 p=curve->positionAt(Min(machineArc,curve->totalLength));
		site.right=tangentToRight(curve->tangentAt(Min(machineArc,curve->totalLength)));site.forward=Vec3{-site.right.z,0,site.right.x};
		const bool crane=edge.useElevation && (phase==1 || phase==2);
		if(crane) p+=site.right*(edge.totalWidth()*.5+5);
		p.y=edge.useElevation && phase>=3 ? p.y+.1 : world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))+.1;
		site.origin=p;
		const double yaw=Atan2(site.forward.x,site.forward.z);
		auto machine=[&](int index,Vec3 position)
		{
			cache.machines << std::pair<int,Mat4x4>{index,Mat4x4::RotateY(static_cast<float>(yaw))*Mat4x4::Translate(position)};
		};
		if(crane) machine(3,p);
		else if(phase<2) machine(0,p);
		else if(phase==2) machine(1,p);
		else if(phase==3)
		{
			machine(2,p);
			Vec3 roller=p-site.forward*11;
			if(!edge.useElevation) roller.y=world.sampleHeight(static_cast<float>(roller.x),static_cast<float>(roller.z))+.1;
			machine(1,roller);
		}
		else site.lineCart();
		if(phase==0)
		{
			for(int i=0;i<12;++i) site.box(i%2 ? 2 : 6,{-2.0+(i%4)*.55,.1+(i%3)*.1,-3.5+(i/4)*.6},{.45,.25+(i%3)*.12,.43});
		}
		const std::array<ColorF,7> colors={ColorF{.95,.58,.035},ColorF{.065,.073,.079},ColorF{.53,.54,.51},ColorF{.085,.20,.26},ColorF{.88,.88,.82},ColorF{.95,.34,.035},ColorF{.34,.23,.14}};
		for(size_t i=0;i<site.batches.size();++i) if(!site.batches[i].vertices.isEmpty()) cache.details << LaneLineBatch{colors[i].removeSRGBCurve(),Mesh{site.batches[i]}};
		if(timer.msF()>3) DBG_LOG(U"[ConstructionMesh] edge={} stage={} step={} ms={:.2f}"_fmt(edge.id,progress.name(),step,timer.msF()));
	}
	for(const auto& surface:cache.surfaces)
	{
		if(surface.texture==&m_constructionSoil || surface.texture==&m_constructionGravel)
		{
			const bool aggregate=surface.texture==&m_constructionGravel;
			Graphics3D::SetPSTexture(4,aggregate ? m_constructionGravelNormal : m_constructionSoilNormal);
			const ScopedCustomShader3D shader{aggregate ? m_constructionAggregatePS : m_constructionEarthPS};
			surface.meshPair.detail.draw(*surface.texture,surface.color);
		}
		else if(surface.texture) surface.meshPair.detail.draw(*surface.texture,surface.color);
		else surface.meshPair.detail.draw(surface.color);
	}
	if(close || edge.useElevation) for(const auto& batch:cache.details) batch.mesh.draw(batch.color);
	if(close) for(const auto& [index,transform]:cache.machines) m_constructionModels[index].draw(transform);
}
