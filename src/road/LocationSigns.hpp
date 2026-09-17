#pragma once
#include "BezierUtil.hpp"

/// @brief 市町村名は住所データを参照し、看板のための地名を重複して保存しない。
namespace LocationSigns
{
	struct Boundary { float arc; String before,after; };
	inline Array<Boundary> boundaries(const CubicBezier& curve,const std::function<String(Vec2)>& lookup)
	{
		Array<Boundary> result;
		if (!lookup) { return result; }
		const auto name=[&](float arc)
		{
			const Vec3 point=arc>curve.totalLength ? curve.p3+curve.tangent(1).normalized()*(arc-curve.totalLength) : curve.positionAt(arc);
			return lookup({point.x,point.z});
		};
		float previous=0;
		String before=name(0);
		const int count=Max(1,static_cast<int>(Ceil(curve.totalLength/24)));
		for (int index=1;index<=count;++index)
		{
			const float arc=index==count ? curve.totalLength+.2f : curve.totalLength*index/count;
			const String after=name(arc);
			if (!before.isEmpty() && !after.isEmpty() && after!=before)
			{
				float low=previous,high=arc;
				for (int pass=0;pass<12;++pass) { const float middle=(low+high)*.5f; if (name(middle)==before) { low=middle; } else { high=middle; } }
				if (result.isEmpty() || high-result.back().arc>100) { result << Boundary{Min(high,curve.totalLength),before,after}; }
			}
			before=after; previous=arc;
		}
		return result;
	}
}
