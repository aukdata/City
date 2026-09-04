# include <Siv3D.hpp>
# include "src/road/BezierUtil.hpp"

namespace
{
	struct EdgeProjection
	{
		float arc = 0.0f;
		Vec3 position{ 0.0, 0.0, 0.0 };
		Vec3 tangent{ 1.0, 0.0, 0.0 };
		float distance = 0.0f;
	};

	float angleFromNodeAxis(const CubicBezier& bez)
	{
		const Vec3 axis = bez.p3 - bez.p0;
		return static_cast<float>(Math::Atan2(axis.z, axis.x));
	}

	float angleFromSampledTangent(const CubicBezier& bez, float t)
	{
		const Vec3 tan = bez.tangentAt(bez.totalLength * t);
		return static_cast<float>(Math::Atan2(tan.z, tan.x));
	}

	EdgeProjection projectPointToBezierXZ(const CubicBezier& bez, const Vec2& point)
	{
		EdgeProjection out;
		double bestDistSq = Math::Inf;
		int bestIndex = 0;

		constexpr int kSamples = 96;
		for (int i = 0; i <= kSamples; ++i)
		{
			const float arc = bez.totalLength * (static_cast<float>(i) / kSamples);
			const Vec3 pos = bez.positionAt(arc);
			const double dx = pos.x - point.x;
			const double dz = pos.z - point.y;
			const double distSq = dx * dx + dz * dz;
			if (distSq < bestDistSq)
			{
				bestDistSq = distSq;
				bestIndex = i;
				out.arc = arc;
				out.position = pos;
			}
		}

		float lo = bez.totalLength * (Max(bestIndex - 1, 0) / static_cast<float>(kSamples));
		float hi = bez.totalLength * (Min(bestIndex + 1, kSamples) / static_cast<float>(kSamples));
		for (int iter = 0; iter < 24; ++iter)
		{
			const float m1 = lo + (hi - lo) / 3.0f;
			const float m2 = hi - (hi - lo) / 3.0f;
			const Vec3 p1 = bez.positionAt(m1);
			const Vec3 p2 = bez.positionAt(m2);
			const double d1 = (p1.x - point.x) * (p1.x - point.x) + (p1.z - point.y) * (p1.z - point.y);
			const double d2 = (p2.x - point.x) * (p2.x - point.x) + (p2.z - point.y) * (p2.z - point.y);
			if (d1 < d2) hi = m2;
			else lo = m1;
		}

		out.arc = (lo + hi) * 0.5f;
		out.position = bez.positionAt(out.arc);
		out.tangent = bez.tangentAt(out.arc);
		out.distance = static_cast<float>(Math::Sqrt(
			(out.position.x - point.x) * (out.position.x - point.x)
			+ (out.position.z - point.y) * (out.position.z - point.y)));
		return out;
	}

	Vec2 worldToScreen(const Vec2& p, const RectF& area, const RectF& world)
	{
		const double u = (p.x - world.x) / world.w;
		const double v = (p.y - world.y) / world.h;
		return {
			area.x + area.w * u,
			area.y + area.h * v
		};
	}

	void drawBezier2D(const CubicBezier& bez, const RectF& area, const RectF& world, const ColorF& color, double thickness)
	{
		Array<Vec2> pts;
		for (int i = 0; i <= 160; ++i)
		{
			const float arc = bez.totalLength * (static_cast<float>(i) / 160);
			const Vec3 p = bez.positionAt(arc);
			pts << worldToScreen(Vec2{ p.x, p.z }, area, world);
		}
		for (size_t i = 1; i < pts.size(); ++i)
		{
			Line{ pts[i - 1], pts[i] }.draw(thickness, color);
		}
	}

	void drawBuildingFootprint(const Vec2& center, double size, float angle, const RectF& area, const RectF& world, const ColorF& color)
	{
		const Vec2 axes[4] = {
			{ -size * 0.5, -size * 0.5 },
			{  size * 0.5, -size * 0.5 },
			{  size * 0.5,  size * 0.5 },
			{ -size * 0.5,  size * 0.5 }
		};
		Array<Vec2> corners;
		const double c = Math::Cos(angle);
		const double s = Math::Sin(angle);
		for (const Vec2& p : axes)
		{
			const Vec2 rotated{
				center.x + p.x * c - p.y * s,
				center.y + p.x * s + p.y * c
			};
			corners << worldToScreen(rotated, area, world);
		}
		Polygon{ corners }.draw(ColorF{ color, 0.15 }).drawFrame(2.0, color);
	}

	void drawDirectionMarker(const Vec2& pos, float angle, double length, const RectF& area, const RectF& world, const ColorF& color)
	{
		const Vec2 a = worldToScreen(pos, area, world);
		const Vec2 dir = Vec2{ Math::Cos(angle), Math::Sin(angle) } * length;
		Line{ a - dir, a + dir }.draw(3.0, color);
		Circle{ a, 5.0 }.draw(color);
	}

	void drawLegendItem(const Font& font, const Vec2& pos, const ColorF& color, StringView label)
	{
		RectF{ pos.x, pos.y + 5, 18, 10 }.draw(color);
		font(label).draw(pos + Vec2{ 26, 0 }, Palette::White);
	}
}

void Main()
{
	Scene::Resize(1400, 900);
	Scene::SetBackground(ColorF{ 0.10, 0.12, 0.14 });

	const Font font{ FontMethod::MSDF, 20, Typeface::Medium };
	const Font mono{ FontMethod::MSDF, 16, Typeface::Regular };

	const CubicBezier bez{
		Vec3{ 100, 0, 120 },
		Vec3{ 230, 0, 330 },
		Vec3{ 430, 0, 10 },
		Vec3{ 560, 0, 220 }
	};

	constexpr float sampledT = 0.25f;
	const Vec2 buildingCenter{ 210, 150 };

	const float nodeAngle = angleFromNodeAxis(bez);
	const float sampledAngle = angleFromSampledTangent(bez, sampledT);
	const EdgeProjection projected = projectPointToBezierXZ(bez, buildingCenter);
	const float projectedAngle = static_cast<float>(Math::Atan2(projected.tangent.z, projected.tangent.x));
	const float projectedT = (bez.totalLength > 1e-6f) ? (projected.arc / bez.totalLength) : 0.0f;

	Logger << U"[BezierAngleTest] nodeAngleDeg={} sampledAngleDeg={} projectedAngleDeg={}"_fmt(
		Math::ToDegrees(nodeAngle),
		Math::ToDegrees(sampledAngle),
		Math::ToDegrees(projectedAngle));
	Logger << U"[BezierAngleTest] sampledT={} projectedT={} projectedDist={}"_fmt(
		sampledT, projectedT, projected.distance);

	const RectF worldRect{ 60, -20, 560, 360 };
	const RectF plotArea{ 40, 120, 820, 620 };

	int frame = 0;
	constexpr int kCaptureFrame = 3;
	while (System::Update())
	{
		RectF{ plotArea }.draw(ColorF{ 0.15, 0.18, 0.20 });
		drawBezier2D(bez, plotArea, worldRect, Palette::White, 8.0);

		const Vec3 sampledPos3 = bez.positionAt(bez.totalLength * sampledT);
		const Vec2 sampledPos{ sampledPos3.x, sampledPos3.z };
		const Vec2 projectedPos{ projected.position.x, projected.position.z };

		Circle{ worldToScreen(buildingCenter, plotArea, worldRect), 7.0 }.draw(Palette::Yellow);
		Circle{ worldToScreen(sampledPos, plotArea, worldRect), 7.0 }.draw(Palette::Skyblue);
		Circle{ worldToScreen(projectedPos, plotArea, worldRect), 7.0 }.draw(Palette::Orange);

		Line{ worldToScreen(buildingCenter, plotArea, worldRect), worldToScreen(sampledPos, plotArea, worldRect) }
			.draw(2.0, Palette::Skyblue);
		Line{ worldToScreen(buildingCenter, plotArea, worldRect), worldToScreen(projectedPos, plotArea, worldRect) }
			.draw(2.0, Palette::Orange);

		drawDirectionMarker(Vec2{ 120, 305 }, nodeAngle, 26.0, plotArea, worldRect, Palette::Red);
		drawDirectionMarker(sampledPos, sampledAngle, 30.0, plotArea, worldRect, Palette::Skyblue);
		drawDirectionMarker(projectedPos, projectedAngle, 30.0, plotArea, worldRect, Palette::Orange);

		drawBuildingFootprint(buildingCenter, 48.0, nodeAngle, plotArea, worldRect, Palette::Red);
		drawBuildingFootprint(buildingCenter, 48.0, sampledAngle, plotArea, worldRect, Palette::Skyblue);
		drawBuildingFootprint(buildingCenter, 48.0, projectedAngle, plotArea, worldRect, Palette::Orange);

		font(U"Bezier building frontage repro").draw(40, 28, Palette::White);
		mono(U"red: current GameScene endpoint axis").draw(40, 60, Palette::White);
		mono(U"blue: sampled tangent at t=0.25").draw(40, 82, Palette::White);
		mono(U"orange: closest-point tangent from building center").draw(40, 104, Palette::White);

		drawLegendItem(mono, Vec2{ 900, 160 }, Palette::Yellow, U"building center");
		drawLegendItem(mono, Vec2{ 900, 190 }, Palette::Skyblue, U"sampled frontage point");
		drawLegendItem(mono, Vec2{ 900, 220 }, Palette::Orange, U"projected frontage point");

		mono(U"nodeAngle   = {:6.2f} deg"_fmt(Math::ToDegrees(nodeAngle))).draw(900, 280, Palette::Red);
		mono(U"sampledT    = {:0.3f}"_fmt(sampledT)).draw(900, 310, Palette::Skyblue);
		mono(U"sampledAngle = {:6.2f} deg"_fmt(Math::ToDegrees(sampledAngle))).draw(900, 340, Palette::Skyblue);
		mono(U"projectedT  = {:0.3f}"_fmt(projectedT)).draw(900, 390, Palette::Orange);
		mono(U"projectedAngle = {:6.2f} deg"_fmt(Math::ToDegrees(projectedAngle))).draw(900, 420, Palette::Orange);
		mono(U"projectedDist = {:6.2f} m"_fmt(projected.distance)).draw(900, 450, Palette::Orange);
		mono(U"Expected root fix: use projectedT + Bezier tangent").draw(900, 510, Palette::White);

		if (frame == kCaptureFrame)
		{
			ScreenCapture::SaveCurrentFrame(U"bezier_building_frontage_repro.png");
		}
		if (frame > kCaptureFrame)
		{
			break;
		}
		++frame;
	}
}
