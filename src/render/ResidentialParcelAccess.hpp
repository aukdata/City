#pragma once
#include <Siv3D.hpp>
#include <cmath>
#include <array>
#include <functional>

/// @brief A passable entrance strip is owned by its parcel and its bounded frontage gap.
namespace ResidentialParcelAccess
{
	inline constexpr double kWidth=1.2;
	inline constexpr double kMaximumFrontageGap=1.5;
	/// @brief A cached transport mask only guarantees complete exclusion within its resident chunk.
	inline bool insideMaskDomain(RectF bounds,RectF domain)
	{
		return std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.w) && std::isfinite(bounds.h)
			&& bounds.w>0 && bounds.h>0 && bounds.x>=domain.x && bounds.y>=domain.y
			&& bounds.x+bounds.w<=domain.x+domain.w && bounds.y+bounds.h<=domain.y+domain.h;
	}
	/// @brief Build only a full-width connected path; unsupported or obstructed lots stay unchanged.
	inline Optional<Array<Vec2>> create(const Array<Vec2>& parcel,Vec2 roadEnd,Vec2 entrance,
		const Array<Array<Vec2>>& obstacles={},double width=kWidth,String* reason=nullptr)
	{
		const auto reject=[&](StringView value)->Optional<Array<Vec2>> { if (reason) { *reason=value; } return none; };
		if (reason) { reason->clear(); }
		const double length=roadEnd.distanceFrom(entrance);
		if (parcel.size()<3 || !std::isfinite(length) || length<.20 || length>25 || !std::isfinite(width) || width<.9 || width>2.0) { return reject(U"invalid dimensions"); }
		const Vec2 inward=(entrance-roadEnd)/length,side{inward.y,-inward.x};
		Array<Vec2> local; for (const auto point : parcel) { if (!std::isfinite(point.x) || !std::isfinite(point.y)) { return reject(U"non-finite parcel"); } local << point-roadEnd; }
		if (!Geometry2D::IsClockwise(local)) { local.reverse(); }
		const Polygon owned{local}; if (!owned || owned.num_holes()!=0) { return reject(U"invalid parcel"); }
		// Convexity is a double-coordinate turn invariant, not equality of separately triangulated areas.
		double turnSign=0;
		for (size_t i=0;i<local.size();++i)
		{
			const Vec2 a=local[(i+1)%local.size()]-local[i],b=local[(i+2)%local.size()]-local[(i+1)%local.size()];
			const double turn=a.x*b.y-a.y*b.x;
			if (Abs(turn)<1e-10) { continue; }
			if (turnSign!=0 && turnSign*turn<0) { return reject(U"non-convex parcel"); }
			turnSign=turn;
		}
		if (turnSign==0) { return reject(U"degenerate parcel"); }
		Array<Vec2> strip{-side*(width*.5),side*(width*.5),inward*length+side*(width*.5),inward*length-side*(width*.5)};
		if (!Geometry2D::IsClockwise(strip)) { strip.reverse(); }
		const Polygon path{strip}; if (!path) { return reject(U"invalid strip"); }
		// The final full-width 20cm must be inside the actual parcel, not just its frontage reservation.
		Array<Vec2> endPoints{inward*(length-.20)-side*(width*.5),inward*(length-.20)+side*(width*.5),
			inward*length+side*(width*.5),inward*length-side*(width*.5)};
		if (!Geometry2D::IsClockwise(endPoints)) { endPoints.reverse(); }
		for (const auto& remainder : Geometry2D::Subtract(Polygon{endPoints},owned)) { if (remainder.area()>1e-7) { return reject(U"door end outside parcel"); } }
		for (const auto& remainder : Geometry2D::Subtract(path,owned))
		{
			if (remainder.area()<=1e-7) { continue; }
			for (const auto point : remainder.outer())
			{
				const double distance=point.dot(inward);
				if (distance<-.00001 || distance>kMaximumFrontageGap+.00001) { return reject(U"frontage gap exceeds bound"); }
			}
		}
		for (const auto& obstacle : obstacles)
		{
			if (obstacle.size()<3) { return reject(U"invalid obstacle points"); }
			Array<Vec2> points; for (const auto point : obstacle) { points << point-roadEnd; }
			if (!Geometry2D::IsClockwise(points)) { points.reverse(); }
			const Polygon blocked{points}; if (!blocked || path.intersects(blocked)) { return reject(U"transport or neighbor obstacle"); }
		}
		for (auto& point : strip) { point+=roadEnd; }
		return strip;
	}
	struct Entry { Vec2 point; double height=0; };
	/// @brief A small filled concrete ramp meets actual surface heights without burying in the yard.
	inline Optional<MeshData> ramp(Vec2 roadEnd,Entry entrance,double roadHeight,
		const std::function<double(Vec2)>& ground,double parcelLift,double width=kWidth,const Array<Vec2>& parcel={})
	{
		const double length=roadEnd.distanceFrom(entrance.point);
		if (!std::isfinite(roadHeight) || !std::isfinite(entrance.height) || !std::isfinite(length) || !std::isfinite(width) || !std::isfinite(parcelLift) || parcelLift<0 || parcelLift>.1 || length<.2 || length>25 || width<.9 || width>2) { return none; }
		const double grade=(entrance.height-roadHeight)/length;
		if (Abs(grade)>.12) { return none; }
		const Vec2 inward=(entrance.point-roadEnd)/length,side{inward.y,-inward.x};
		Array<Vec2> ownedPoints; for (const auto point : parcel) { ownedPoints << point-roadEnd; }
		if (!ownedPoints.isEmpty() && !Geometry2D::IsClockwise(ownedPoints)) { ownedPoints.reverse(); }
		const Polygon owned{ownedPoints}; if (!parcel.isEmpty() && !owned) { return none; }
		const auto floorOffset=[&](Vec2 point) { return parcel.isEmpty() || owned.contains(point-roadEnd) ? parcelLift : 0.0; };
		const int steps=Max(1,static_cast<int>(Ceil(length/.5)));
		Array<double> fractions;
		for (int i=0;i<=steps;++i) { fractions << static_cast<double>(i)/steps; }
		const auto cross=[](Vec2 a,Vec2 b) { return a.x*b.y-a.y*b.x; };
		for (int sideIndex=-1;sideIndex<=1;++sideIndex)
		{
			const Vec2 start=side*(sideIndex*width*.5),delta=inward*length;
			for (size_t i=0;i<ownedPoints.size();++i)
			{
				const Vec2 a=ownedPoints[i],edge=ownedPoints[(i+1)%ownedPoints.size()]-a;
				const double denominator=cross(delta,edge); if (Abs(denominator)<1e-12) { continue; }
				const double t=cross(a-start,edge)/denominator,u=cross(a-start,delta)/denominator;
				if (t>=0 && t<=1 && u>=0 && u<=1)
				{
					fractions << t << Max(0.0,t-.008/length) << Min(1.0,t+.008/length);
				}
			}
		}
		fractions.sort();

		struct Row { Vec3 left,right,leftBase,rightBase; };
		Array<Row> rows; std::array<double,3> previous{}; double previousFraction=-1;
		for (const double fraction : fractions)
		{
			if (previousFraction>=0 && fraction-previousFraction<1e-9) { continue; }
			const Vec2 point=roadEnd.lerp(entrance.point,fraction);
			const double top=Math::Lerp(roadHeight,entrance.height,fraction);
			std::array<double,3> heights;
			for (int j=0;j<3;++j)
			{
				const Vec2 sample=point+side*((j-1)*width*.5);
				heights[j]=ground(sample);
				if (!std::isfinite(heights[j]) || top<heights[j]+floorOffset(sample)+.001 || top-heights[j]>.25) { return none; }
				if (previousFraction>=0 && Abs(heights[j]-previous[j])>length*(fraction-previousFraction)*.12+.000001) { return none; }
			}
			if (Abs(heights[1]-heights[0])>width*.5*.12 || Abs(heights[2]-heights[1])>width*.5*.12) { return none; }
			previous=heights; previousFraction=fraction;
			const Vec2 left=point-side*(width*.5),right=point+side*(width*.5);
			rows << Row{{left.x,top,left.y},{right.x,top,right.y},
				{left.x,heights[0]+floorOffset(left),left.y},{right.x,heights[2]+floorOffset(right),right.y}};
		}
		MeshData mesh;
		const auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d,Vec3 normal)
		{
			const uint32 start=static_cast<uint32>(mesh.vertices.size());
			for (const auto point : {a,b,c,d}) { mesh.vertices << Vertex3D{Float3{point},Float3{normal},Float2{static_cast<float>(point.x*.25),static_cast<float>(point.z*.25)}}; }
			for (const auto triangle : {TriangleIndex32{start,start+1,start+2},TriangleIndex32{start,start+2,start+3}})
			{
				const Vec3 a=Vec3{mesh.vertices[triangle.i0].pos},b=Vec3{mesh.vertices[triangle.i1].pos},c=Vec3{mesh.vertices[triangle.i2].pos};
				if ((b-a).cross(c-a).lengthSq()>0) { mesh.indices << triangle; }
			}
		};
		const Vec3 normal=Vec3{-inward.x*grade,1,-inward.y*grade}.normalized();
		for (size_t i=1;i<rows.size();++i)
		{
			const auto& a=rows[i-1]; const auto& b=rows[i];
			quad(a.left,b.left,b.right,a.right,normal);
			quad(a.left,a.leftBase,b.leftBase,b.left,{-side.x,0,-side.y});
			quad(a.right,b.right,b.rightBase,a.rightBase,{side.x,0,side.y});
		}
		return mesh;
	}
}
