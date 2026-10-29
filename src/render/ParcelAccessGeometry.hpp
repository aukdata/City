#pragma once
#include <Siv3D.hpp>

/// @brief Small street-facing paths use the nearest real road boundary and never cross the carriageway.
namespace ParcelAccessGeometry
{
	/// @brief A narrow paved strip and its setback from the facade.
	struct Connection
	{
		Array<Vec2> polygon;
		double distance = 0;
	};
	/// @brief Connect a facade to a road only when that road is in front and within the normal setback.
	inline Optional<Connection> connect(Vec2 facade, Vec2 outward, double width, const Array<Vec2>& road)
	{
		if (road.size()<3 || outward.lengthSq()<.001 || width<=0) { return none; }
		outward.normalize();
		const auto cross = [](Vec2 a,Vec2 b) { return a.x*b.y-a.y*b.x; };
		double nearest = 30;
		bool found = false,inside = false;
		for (size_t i=0;i<road.size();++i)
		{
			const Vec2 a=road[i],span=road[(i+1)%road.size()]-a;
			const Vec2 end=road[(i+1)%road.size()];
			if ((a.y>facade.y)!=(end.y>facade.y) && facade.x<a.x+span.x*(facade.y-a.y)/span.y) { inside=!inside; }
			const double denominator=cross(outward,span);
			if (Abs(denominator)<1e-8) { continue; }
			const double distance=cross(a-facade,span)/denominator,fraction=cross(a-facade,outward)/denominator;
			if (distance>.15 && distance<nearest && fraction>=0 && fraction<=1) { nearest=distance;found=true; }
		}
		if (inside || !found) { return none; }
		const Vec2 half=Vec2{-outward.y,outward.x}*(width*.5),end=facade+outward*nearest;
		return Connection{{facade-half,facade+half,end+half,end-half},nearest};
	}
}
