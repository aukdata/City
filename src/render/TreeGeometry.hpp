#pragma once
#include <Siv3D.hpp>

/// @brief Deterministic branching trees with a separate low-cost distant crown.
namespace TreeGeometry
{
	struct Geometry { MeshData wood,leaves,distant; };
	inline void append(MeshData& target,const MeshData& source)
	{
		const uint32 offset=static_cast<uint32>(target.vertices.size());
		target.vertices.append(source.vertices);
		for (const auto triangle : source.indices) { target.indices << TriangleIndex32{triangle.i0+offset,triangle.i1+offset,triangle.i2+offset}; }
	}
	inline void branch(MeshData& mesh,Vec3 start,Vec3 end,double radius)
	{
		const Vec3 direction=(end-start).normalized();
		const Vec3 side=(Abs(direction.y)<.98 ? Vec3{0,1,0} : Vec3{1,0,0}).cross(direction).normalized();
		const Vec3 other=direction.cross(side);
		const uint32 offset=static_cast<uint32>(mesh.vertices.size());
		for (int ring=0;ring<2;++ring) for (int i=0;i<6;++i)
		{
			const double angle=i*Math::TwoPi/6;
			const Vec3 normal=side*Cos(angle)+other*Sin(angle);
			mesh.vertices << Vertex3D{Float3{(ring ? end : start)+normal*radius*(ring ? .36 : 1.0)},Float3{normal},Float2{static_cast<float>(i)/6,static_cast<float>(ring)}};
		}
		for (uint32 i=0;i<6;++i)
		{
			const uint32 next=(i+1)%6;
			mesh.indices << TriangleIndex32{offset+i,offset+next,offset+6+i} << TriangleIndex32{offset+next,offset+6+next,offset+6+i};
		}
	}
	inline Geometry build(uint32 seed,bool cedar)
	{
		Geometry result;
		branch(result.wood,{0,0,0},{.035,cedar ? .94 : .69,.025},.045);
		result.distant=cedar ? MeshData::Cone(Float3{0,.15f,0},.31,.85,9) : MeshData::Sphere(.34,5);
		if (!cedar) { result.distant.scale(1,.94,1).translate(0,.64,0); }
		for (auto& vertex : result.distant.vertices)
		{
			const float variation = static_cast<float>(1.0 + .09 * Sin(vertex.pos.y * 29 + vertex.pos.x * 17 + seed));
			vertex.pos.x *= variation;
			vertex.pos.z *= variation;
		}
		result.distant.computeNormals();
		const int clusters=cedar ? 60 : 84;
		for (int i=0;i<clusters;++i)
		{
			const double angle=i*2.399963+(seed%97)*.064;
			const double level=cedar ? static_cast<double>(i)/clusters : Fmod(i*.754877+seed*.136,1.0);
			const double spread=cedar ? .28*(1-level)+.035 : (.12+.18*Sqrt(1-Pow(level*2-1,2)))*Sqrt(.20+.80*Fmod(i*.5698+seed*.042,1.0));
			const double y=cedar ? .25+level*.69 : .40+level*.49;
			const Vec3 end{Cos(angle)*spread,y,Sin(angle)*spread};
			if (i%6==0) { branch(result.wood,{0,y*.7,0},end,.015*(1-level*.65)); }
			MeshData cluster=MeshData::Sphere(1,3);
			for (auto& vertex : cluster.vertices)
			{
				const double irregular=.88+.17*Sin(vertex.pos.x*9+vertex.pos.y*13+i+seed%13);
				vertex.pos*=static_cast<float>(irregular);
			}
			cluster.scale(cedar ? .10 : .095,cedar ? .04 : .095,cedar ? .07 : .085);
			cluster.translate(Float3{end});cluster.computeNormals();
			append(result.leaves,cluster);
		}
		return result;
	}
	/// @brief A 256 m tile prevents one nearby tree from enabling detailed trees in an entire 1 km chunk.
	inline int materialKey(int material,Vec2 position)
	{
		const int x=(static_cast<int>(Floor(position.x/256))%4+4)%4;
		const int z=(static_cast<int>(Floor(position.y/256))%4+4)%4;
		return material+1000*(1+x+z*4);
	}
	inline bool nearChunk(Point chunkCoord,Vec3 eye)
	{
		const Vec2 start{chunkCoord.x*1024.0,chunkCoord.y*1024.0};
		const Vec2 nearest{Clamp(eye.x,start.x,start.x+1024),Clamp(eye.z,start.y,start.y+1024)};
		return nearest.distanceFromSq(Vec2{eye.x,eye.z})<300*300 && eye.y<900;
	}
	inline bool nearTile(int encoded,Point chunkCoord,Vec3 eye)
	{
		const int tile=encoded/1000-1;
		const Vec2 start{chunkCoord.x*1024.0+(tile%4)*256,chunkCoord.y*1024.0+(tile/4)*256};
		const Vec2 nearest{Clamp(eye.x,start.x,start.x+256),Clamp(eye.z,start.y,start.y+256)};
		return nearest.distanceFromSq(Vec2{eye.x,eye.z})<300*300 && eye.y<900;
	}
}
