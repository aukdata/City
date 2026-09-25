#pragma once
#include "../world/World.hpp"
#include "BridgeStructure.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../road/RoadNetwork.hpp"

/// @brief 河道の連続した水面・寄州。1024 m単位の静的メッシュをロード時に作り、近傍のみ描画する。
class RiverRenderer
{
public:
	void build(const World& world,const RoadNetwork& network)
	{
		m_batches.clear(); HashTable<int,MeshData> water,bars,shores,levees,roadBerms;
		for (const auto& reach : world.rivers().reaches)
		{
			const Vec3 delta=reach.end-reach.start; if (Vec2{delta.x,delta.z}.lengthSq()<1) { continue; }
			const Vec3 right=Vec3{delta.z,0,-delta.x}.normalized();
			const int key=static_cast<int>(reach.start.z/1024)*64+static_cast<int>(reach.start.x/1024);
			Vec3 a=reach.start+Vec3{0,.06,0},b=reach.end+Vec3{0,.06,0};
			BridgeStructure::quad(water[key],a-right*reach.halfWidth,b-right*reach.endHalfWidth,b+right*reach.endHalfWidth,a+right*reach.halfWidth);
			const double run=Vec2{delta.x,delta.z}.length();
			if (Min(reach.halfWidth,reach.endHalfWidth)>=42 && Abs(delta.y)/run<.025
				&& Max(reach.start.y,reach.end.y)<180)
			{
				const auto ribbon=[&](MeshData& mesh,Vec3 innerA,Vec3 innerB,Vec3 outerB,Vec3 outerA,int side)
				{
					if (side>0) { BridgeStructure::quad(mesh,innerA,innerB,outerB,outerA); }
					else { BridgeStructure::quad(mesh,innerB,innerA,outerA,outerB); }
				};
				for (const int side : {-1,1})
				{
					const auto bankPoint=[&](Vec3 center,double halfWidth,double offset,double minimumY)
					{
						Vec3 point=center+right*(halfWidth+offset)*side;
						point.y=Max(static_cast<double>(world.computeHeight(static_cast<float>(point.x),static_cast<float>(point.z)))+.05,minimumY);
						return point;
					};
					Vec3 wetA=bankPoint(reach.start,reach.halfWidth,0,reach.start.y+.08);
					Vec3 wetB=bankPoint(reach.end,reach.endHalfWidth,0,reach.end.y+.08);
					wetA.y=reach.start.y+.08;wetB.y=reach.end.y+.08;
					const Vec3 toeA=bankPoint(reach.start,reach.halfWidth,18,reach.start.y+.25);
					const Vec3 toeB=bankPoint(reach.end,reach.endHalfWidth,18,reach.end.y+.25);
					ribbon(shores[key],wetA,wetB,toeB,toeA,side);
					if (Max(toeA.y-reach.start.y,toeB.y-reach.end.y)>4) { continue; }
					Vec3 crestInnerA=bankPoint(reach.start,reach.halfWidth,27,toeA.y+1.4);
					Vec3 crestInnerB=bankPoint(reach.end,reach.endHalfWidth,27,toeB.y+1.4);
					Vec3 crestOuterA=bankPoint(reach.start,reach.halfWidth,33,crestInnerA.y);
					Vec3 crestOuterB=bankPoint(reach.end,reach.endHalfWidth,33,crestInnerB.y);
					const Vec3 outerA=bankPoint(reach.start,reach.halfWidth,43,reach.start.y+.25);
					const Vec3 outerB=bankPoint(reach.end,reach.endHalfWidth,43,reach.end.y+.25);
					ribbon(levees[key],toeA,toeB,crestInnerB,crestInnerA,side);
					ribbon(levees[key],crestInnerA,crestInnerB,crestOuterB,crestOuterA,side);
					ribbon(levees[key],crestOuterA,crestOuterB,outerB,outerA,side);
				}
			}

			// Round joins keep the tributary mouths watertight even when widths differ.
			for (const Vec3 center : {a,b}) for (int i=0;i<16;++i)
			{
				const double t=i*Math::TwoPi/16,u=(i+1)*Math::TwoPi/16;
				const double width=center==a ? reach.halfWidth : reach.endHalfWidth;
				const uint32 n=static_cast<uint32>(water[key].vertices.size());
				for (const Vec3 p : {center,center+Vec3{Cos(u),0,Sin(u)}*width,center+Vec3{Cos(t),0,Sin(t)}*width})
					water[key].vertices << Vertex3D{Float3{p},Float3{0,1,0},Float2{static_cast<float>(p.x*.1),static_cast<float>(p.z*.1)}};
				water[key].indices << TriangleIndex32{n,n+1,n+2};
			}
			if (reach.barSide!=0 && Abs(delta.y)<delta.length()*.012)
			{
				const Vec3 side=right*(reach.halfWidth*reach.barSide);
				const Vec3 first=a+delta*.15+side*.84,last=a+delta*.8+side*.84,inside=a+delta*.46+side*.48;
				const Vec3 raised{0,.16,0}; const uint32 n=static_cast<uint32>(bars[key].vertices.size());
				for (const Vec3 p : {first+raised,inside+raised,last+raised}) { bars[key].vertices << Vertex3D{Float3{p},Float3{0,1,0},Float2{static_cast<float>(p.x*.15),static_cast<float>(p.z*.15)}}; }
				bars[key].indices << TriangleIndex32{n,n+1,n+2} << TriangleIndex32{n,n+2,n+1};
			}
		}
		int leveeRoadCount=0;
		for (const RoadEdge& edge:network.edges())
		{
			if (!edge.leveeRoad) { continue; }
			const auto curve=network.getBezier(edge.id);
			if (!curve) { continue; }
			const Vec3 middle=curve->positionAt(curve->totalLength*.5f);
			const int key=static_cast<int>(middle.z/1024)*64+static_cast<int>(middle.x/1024);
			const double halfWidth=edge.totalWidth()*.5;
			const int steps=Max(4,static_cast<int>(Ceil(curve->totalLength/20)));
			for (int index=0;index<steps;++index)
			{
				const float first=curve->totalLength*index/steps,last=curve->totalLength*(index+1)/steps;
				const Vec3 a=curve->positionAt(first),b=curve->positionAt(last);
				const Vec3 rightA=tangentToRight(curve->tangentAt(first));
				const Vec3 rightB=tangentToRight(curve->tangentAt(last));
				for (const int side:{-1,1})
				{
					Vec3 innerA=a+rightA*halfWidth*side,innerB=b+rightB*halfWidth*side;
					Vec3 outerA=a+rightA*(halfWidth+8)*side,outerB=b+rightB*(halfWidth+8)*side;
					innerA.y=a.y-.04;innerB.y=b.y-.04;
					outerA.y=world.sampleHeight(static_cast<float>(outerA.x),static_cast<float>(outerA.z))+.02;
					outerB.y=world.sampleHeight(static_cast<float>(outerB.x),static_cast<float>(outerB.z))+.02;
					if (side>0) { BridgeStructure::quad(roadBerms[key],innerA,innerB,outerB,outerA); }
					else { BridgeStructure::quad(roadBerms[key],innerB,innerA,outerA,outerB); }
				}
			}
			++leveeRoadCount;
		}
		for (auto& [key,data]:roadBerms)
		{
			Batch batch;batch.center={(key%64+.5)*1024,0,(key/64+.5)*1024};batch.levee=Mesh{data};
			m_batches << std::move(batch);
		}
		DBG_LOG(U"[RiverRenderer] leveeRoads={}"_fmt(leveeRoadCount));
		for (auto& [key,data] : water)
		{
			Batch batch; batch.center={(key%64+.5)*1024,0,(key/64+.5)*1024}; batch.water=Mesh{data};
			if (bars.contains(key) && !bars[key].indices.isEmpty()) { batch.bar=Mesh{bars[key]}; }
			if (shores.contains(key) && !shores[key].indices.isEmpty()) { batch.shore=Mesh{shores[key]}; }
			if (levees.contains(key) && !levees[key].indices.isEmpty()) { batch.levee=Mesh{levees[key]}; }
			m_batches << std::move(batch);
		}
		m_shader=HLSL{U"shaders/hlsl/city_forward.hlsl",U"River_PS"};
		DBG_LOG(U"[RiverRenderer] batches={} shader={}"_fmt(m_batches.size(),static_cast<bool>(m_shader)));
	}
	void draw(Vec3 eye) const
	{
		for (const auto& batch : m_batches)
		{
			if (Vec2{batch.center.x-eye.x,batch.center.z-eye.z}.length()>6500) { continue; }
			if (!batch.shore.isEmpty()) { batch.shore.draw(TextureAsset(Asset::Gravel),ColorF{.77,.72,.63}.removeSRGBCurve()); }
			if (!batch.levee.isEmpty()) { batch.levee.draw(TextureAsset(Asset::Grass),ColorF{.57,.63,.54}.removeSRGBCurve()); }
			if (!batch.bar.isEmpty()) { batch.bar.draw(TextureAsset(Asset::Sand),ColorF{.64,.61,.49}.removeSRGBCurve()); }
			if (!batch.water.isEmpty())
			{
				if (m_shader) { const ScopedCustomShader3D shader{m_shader}; batch.water.draw(ColorF{1}); }
				else { batch.water.draw(ColorF{.10,.29,.34}); }
			}
		}
	}
private:
	struct Batch { Vec3 center; Mesh water,bar,shore,levee; };
	Array<Batch> m_batches;
	PixelShader m_shader;
};
