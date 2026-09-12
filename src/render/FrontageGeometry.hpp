#pragma once
#include <Siv3D.hpp>
#include "TreeGeometry.hpp"

/// @brief 接道面の玄関・店先設備。メートル単位で作り、チャンクの材質別バッチへ結合する。
namespace FrontageGeometry
{
	struct Part { int material; MeshData mesh; };
	inline Array<Part> build(double width, double front, bool commercial, uint32 seed)
	{
		Array<Part> parts;
		constexpr int kConcrete = 117, kMetal = 108, kDark = 115, kTimber = 105;
		const auto box = [&](int material, Vec3 center, Vec3 size)
		{
			parts << Part{material, MeshData::Box(Float3{center}, Float3{size})};
		};
		const auto tube = [&](int material, Vec3 a, Vec3 b, double radius)
		{
			MeshData mesh; TreeGeometry::branch(mesh,a,b,radius); parts << Part{material,std::move(mesh)};
		};
		const auto plant = [&](double x)
		{
			const Vec3 center{x,.19,front-.24};
			parts << Part{105,MeshData::Cylinder(Float3{center},.18,.38,12)};
			parts << Part{115,MeshData::Cylinder(Float3{center+Vec3{0,.20,0}},.16,.025,12)};
			for (int cluster=0;cluster<5;++cluster)
			{
				const double angle=cluster*2.4+seed;
				MeshData leaves=MeshData::Sphere(.17,4);
				leaves.scale(1,1.4,1).translate(Float3{center+Vec3{Cos(angle)*.10,.35+cluster*.035,Sin(angle)*.07}});
				parts << Part{125,std::move(leaves)};
			}
		};
		const double signWidth=Min(4.5,width*.62);
		const double entryFront = front - .34;
		for (const double side : {-1.0,1.0})
		{
			box(kConcrete,{side*.60,1.13,front-.20},{.16,2.26,.46});
		}
		box(kConcrete,{0,2.22,front-.20},{1.36,.16,.46});
		// A shallow entrance surround joins the structural facade across projecting sills.
		box(kDark,{0,1.03,entryFront-.025},{1.06,2.06,.075});
		box(commercial ? kMetal : kTimber,{0,1.04,entryFront-.070},{.90,1.98,.065});
		box(kConcrete,{0,.07,entryFront-.19},{1.25,.14,.40});
		for (const double side : {-1.0,1.0})
		{
			box(kMetal,{side*.515,1.07,entryFront-.09},{.045,2.12,.085});
		}
		box(kMetal,{0,2.09,entryFront-.08},{1.08,.055,.10});
		box(135,{0,1.34,entryFront-.108},{.77,1.20,.019});
		box(kMetal,{0,.72,entryFront-.123},{.83,.045,.030});
		box(kDark,{.32,1.05,entryFront-.135},{.025,.27,.04});
		box(kMetal,{-width*.38,1.24,front-.10},{.21,.31,.10});
		box(kDark,{-width*.38,1.25,front-.16},{.12,.15,.025});
		plant(width*.36);
		if (commercial)
		{
			const int paint=seed%2 ? 132 : 133;
			box(paint,{0,2.39,front-.23},{signWidth+.18,.12,.48});
			box(paint,{0,2.67,front-.07},{signWidth,.44,.14});
			MeshData sign;
			const float left=static_cast<float>(-signWidth*.48),right=-left;
			const float z=static_cast<float>(front-.146);
			const int tile=16+seed%6;
			const float u=static_cast<float>(tile%8)/8,v=static_cast<float>(tile/8)/4;
			const Float3 normal{0,0,-1};
			sign.vertices = {
				Vertex3D{{left,2.49f,z},normal,{u+.006f,v+.235f}},
				Vertex3D{{left,2.85f,z},normal,{u+.006f,v+.012f}},
				Vertex3D{{right,2.85f,z},normal,{u+.119f,v+.012f}},
				Vertex3D{{right,2.49f,z},normal,{u+.119f,v+.235f}}
			};
			sign.indices = {TriangleIndex32{0,1,2},TriangleIndex32{0,2,3}};
			parts << Part{134,std::move(sign)};
			box(kDark,{-width*.31,.56,front-.24},{.48,.84,.10});
			box(kConcrete,{-width*.31,.57,front-.30},{.41,.72,.02});
			for(int line=0;line<5;++line)
			{
				box(kDark,{-width*.31,.39+line*.09,front-.316},{.27,.018,.008});
			}
			plant(-width*.42);
		}
		else
		{
			box(kMetal,{-width*.31,.95,front-.20},{.36,.24,.18});
			box(kDark,{-width*.31,.98,front-.30},{.27,.025,.01});
		}
		// Parked city bicycle: two spoked wheels, frame, mudguards, handlebar and basket.
		if (width>5.0 && seed%3!=0)
		{
			const double origin=width*.25;
			const double z=front-.27;
			const Vec3 rear{origin-.47,.32,z},ahead{origin+.47,.32,z};
			for (const Vec3 center : {rear,ahead})
			{
				for (int segment=0;segment<20;++segment)
				{
					const double a=segment*Math::TwoPi/20,b=(segment+1)*Math::TwoPi/20;
					const Vec3 start=center+Vec3{Cos(a)*.30,Sin(a)*.30,0};
					const Vec3 end=center+Vec3{Cos(b)*.30,Sin(b)*.30,0};
					tube(kDark,start,end,.022);
					if(segment%2==0) { tube(kMetal,center,start,.006); }
				}
			}
			const Vec3 crank{origin,.30,z},seat{origin-.14,.83,z},steer{origin+.38,.90,z};
			for (const auto ends : {std::pair{rear,seat},{rear,crank},{seat,crank},{crank,steer},{seat,steer},{steer,ahead}})
			{
				tube(133,ends.first,ends.second,.025);
			}
			box(kDark,seat+Vec3{0,.08,0},{.24,.065,.17});
			tube(kMetal,steer,steer+Vec3{0,.19,0},.022);
			tube(kMetal,steer+Vec3{0,.19,-.18},steer+Vec3{0,.19,.18},.020);
			for (const double side : {-1.0,1.0})
			{
				for (int wire=0;wire<5;++wire)
				{
					tube(kMetal,{origin+.47+wire*.06,.84,z+side*.14},{origin+.47+wire*.06,1.07,z+side*.14},.007);
				}
				tube(kMetal,{origin+.47,1.07,z+side*.14},{origin+.71,1.07,z+side*.14},.011);
			}
		}
		return parts;
	}
}
