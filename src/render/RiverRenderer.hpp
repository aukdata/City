#pragma once
#include "../world/World.hpp"
#include "BridgeStructure.hpp"
#include "../asset/AssetRegistrar.hpp"

/// @brief 河道の連続した水面・寄州。1024 m単位の静的メッシュをロード時に作り、近傍のみ描画する。
class RiverRenderer
{
public:
	void build(const World& world)
	{
		m_batches.clear(); HashTable<int,MeshData> water,bars;
		for (const auto& reach : world.rivers().reaches)
		{
			const Vec3 delta=reach.end-reach.start; if (Vec2{delta.x,delta.z}.lengthSq()<1) { continue; }
			const Vec3 right=Vec3{delta.z,0,-delta.x}.normalized();
			const int key=static_cast<int>(reach.start.z/1024)*64+static_cast<int>(reach.start.x/1024);
			Vec3 a=reach.start+Vec3{0,.06,0},b=reach.end+Vec3{0,.06,0};
			BridgeStructure::quad(water[key],a-right*reach.halfWidth,b-right*reach.halfWidth,b+right*reach.halfWidth,a+right*reach.halfWidth);
			// Round joins keep the tributary mouths watertight even when widths differ.
			for (const Vec3 center : {a,b}) for (int i=0;i<16;++i)
			{
				const double t=i*Math::TwoPi/16,u=(i+1)*Math::TwoPi/16;
				const uint32 n=static_cast<uint32>(water[key].vertices.size());
				for (const Vec3 p : {center,center+Vec3{Cos(u),0,Sin(u)}*reach.halfWidth,center+Vec3{Cos(t),0,Sin(t)}*reach.halfWidth})
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
		for (auto& [key,data] : water)
		{
			Batch batch; batch.center={(key%64+.5)*1024,0,(key/64+.5)*1024}; batch.water=Mesh{data};
			if (bars.contains(key) && !bars[key].indices.isEmpty()) { batch.bar=Mesh{bars[key]}; }
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
			if (!batch.bar.isEmpty()) { batch.bar.draw(TextureAsset(Asset::Sand),ColorF{.64,.61,.49}.removeSRGBCurve()); }
			if (m_shader) { const ScopedCustomShader3D shader{m_shader}; batch.water.draw(ColorF{1}); }
			else { batch.water.draw(ColorF{.10,.29,.34}); }
		}
	}
private:
	struct Batch { Vec3 center; Mesh water,bar; };
	Array<Batch> m_batches;
	PixelShader m_shader;
};
