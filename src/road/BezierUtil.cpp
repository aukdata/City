#include "BezierUtil.hpp"

CubicBezier::CubicBezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3)
	: p0(p0), p1(p1), p2(p2), p3(p3)
{
	buildTable();
}

void CubicBezier::buildTable()
{
	arcTable.resize(SAMPLES + 1);
	arcTable[0] = 0.0f;

	for (int i = 1; i <= SAMPLES; ++i)
	{
		const float tPrev = static_cast<float>(i - 1) / SAMPLES;
		const float tCurr = static_cast<float>(i) / SAMPLES;
		const Vec3 prev = evaluate(tPrev);
		const Vec3 curr = evaluate(tCurr);
		arcTable[i] = arcTable[i - 1] + static_cast<float>((curr - prev).length());
	}

	totalLength = arcTable[SAMPLES];
}

Vec3 CubicBezier::evaluate(float t) const
{
	const float u  = 1.0f - t;
	const float u2 = u * u;
	const float u3 = u2 * u;
	const float t2 = t * t;
	const float t3 = t2 * t;

	return u3 * p0
		+ 3.0f * u2 * t * p1
		+ 3.0f * u  * t2 * p2
		+ t3 * p3;
}

Vec3 CubicBezier::tangent(float t) const
{
	const float u  = 1.0f - t;
	const float u2 = u * u;
	const float t2 = t * t;

	const Vec3 d = 3.0f * u2 * (p1 - p0)
		+ 6.0f * u * t * (p2 - p1)
		+ 3.0f * t2 * (p3 - p2);

	const double len = d.length();
	if (len < 1e-10)
		return Vec3::Right();

	return d / static_cast<float>(len);
}

float CubicBezier::tFromArcLength(float s) const
{
	if (s <= 0.0f)
		return 0.0f;
	if (s >= totalLength)
		return 1.0f;

	// 二分探索で arcTable[lo] <= s < arcTable[hi] となる区間を探す
	int lo = 0;
	int hi = SAMPLES;
	while (hi - lo > 1)
	{
		const int mid = (lo + hi) / 2;
		if (arcTable[mid] <= s)
			lo = mid;
		else
			hi = mid;
	}

	// lo と hi の間で線形補間
	const float sLo = arcTable[lo];
	const float sHi = arcTable[hi];
	const float tLo = static_cast<float>(lo) / SAMPLES;
	const float tHi = static_cast<float>(hi) / SAMPLES;

	const float denom = sHi - sLo;
	if (denom < 1e-10f)
		return tLo;

	const float alpha = (s - sLo) / denom;
	return tLo + alpha * (tHi - tLo);
}
