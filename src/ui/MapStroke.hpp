#pragma once
#include <Siv3D.hpp>

/// @brief 表示範囲へ倍精度で切り詰める地図線。巨大な画面座標をGPUへ渡さない。
namespace MapStroke
{
	inline void draw(Vec2 a, Vec2 b, RectF viewport, double width, ColorF color, bool tunnel = false, double phase = 0)
	{
		const Vec2 delta = b - a;
		const double length = delta.length();
		if (length < 1e-6 || !IsFinite(length)) { return; }
		const RectF bounds = viewport.stretched(width);
		double first = 0, last = 1;
		const auto clip = [&](double p, double q)
		{
			if (Abs(p) < 1e-12) { return q >= 0; }
			const double t = q / p;
			if (p < 0) { first = Max(first, t); } else { last = Min(last, t); }
			return first <= last;
		};
		if (!clip(-delta.x, a.x-bounds.x) || !clip(delta.x, bounds.rightX()-a.x)
			|| !clip(-delta.y, a.y-bounds.y) || !clip(delta.y, bounds.bottomY()-a.y)) { return; }
		if (!tunnel) { Line{a+delta*first,a+delta*last}.draw(width,color);return; }
		constexpr double kDash = 9, kPeriod = 15;
		const double begin = first*length, end = last*length;
		for (double s = Floor((begin+phase)/kPeriod)*kPeriod-phase; s < end; s += kPeriod)
		{
			const double lo=Max(begin,s),hi=Min(end,s+kDash);
			if (hi>lo) { Line{a+delta*(lo/length),a+delta*(hi/length)}.draw(LineStyle::Uncapped,width,color); }
		}
	}
}
