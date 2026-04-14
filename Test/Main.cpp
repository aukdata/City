# include <Siv3D.hpp>

// 路面標示矢印テスト (2D確認)
// 各タイプのポリゴンを2D描画してスケールと形状を確認する

namespace
{
	const Array<Vec2> kPx_Straight = {
		Vec2{ 253.0, -26.5 }, Vec2{ 254.0, -10.5 }, Vec2{ 501.0, -10.5 },
		Vec2{ 501.0,   9.5 }, Vec2{ 254.0,   9.5 }, Vec2{ 253.0,  26.5 },
		Vec2{   0.0,  -5.5 },
	};
	const Array<Vec2> kPx_Left = {
		Vec2{  75.0,   0.0 }, Vec2{  89.0, -24.0 }, Vec2{ 113.0, -38.0 },
		Vec2{ 492.0, -38.0 }, Vec2{ 492.0, -19.0 }, Vec2{ 147.0, -19.0 },
		Vec2{ 135.0, -10.0 }, Vec2{ 134.0,   0.0 }, Vec2{ 208.0,   0.0 },
		Vec2{ 103.0,  38.0 }, Vec2{   0.0,   0.0 },
	};
	const Array<Vec2> kPx_StraightLeft = {
		Vec2{ 222.0, -46.5 }, Vec2{ 223.0, -28.5 }, Vec2{ 501.0, -28.5 },
		Vec2{ 501.0, -10.5 }, Vec2{ 428.0, -10.5 }, Vec2{ 416.0,  -6.5 },
		Vec2{ 409.0,   9.5 }, Vec2{ 483.0,   9.5 }, Vec2{ 373.0,  46.5 },
		Vec2{ 260.0,   9.5 }, Vec2{ 334.0,   9.5 }, Vec2{ 339.0,  -2.5 },
		Vec2{ 352.0, -10.5 }, Vec2{ 223.0, -10.5 }, Vec2{ 219.0,   8.5 },
		Vec2{   0.0, -18.5 },
	};

	constexpr double kPxXMax_Straight       = 501.0;
	constexpr double kPxYRange_Straight     = 53.0;
	constexpr double kPxXMax_Left           = 492.0;
	constexpr double kPxYRange_Left         = 76.0;
	constexpr double kPxXMax_StraightLeft   = 501.0;
	constexpr double kPxYRange_StraightLeft = 93.0;
	constexpr double kArrowLength_m         = 5.0;
	// 等方スケール (1px ≈ 1cm 両軸): 5.0 × pxYRange / pxXMax
	constexpr double kWidthStraight_m       = kArrowLength_m * kPxYRange_Straight    / kPxXMax_Straight;     // ≈ 0.529m
	constexpr double kWidthTurn_m           = kArrowLength_m * kPxYRange_Left        / kPxXMax_Left;         // ≈ 0.773m
	constexpr double kWidthCombined_m       = kArrowLength_m * kPxYRange_StraightLeft / kPxXMax_StraightLeft; // ≈ 0.928m

	Array<Vec2> normalize(const Array<Vec2>& px, double pxXMax,
	                      double widthMeters, double pxYRange, bool flipY)
	{
		const double scaleX = kArrowLength_m / pxXMax;
		const double scaleY = widthMeters / pxYRange;
		Array<Vec2> out;
		for (const auto& v : px)
			out << Vec2{ (pxXMax - v.x) * scaleX,
			             v.y * scaleY * (flipY ? -1.0 : 1.0) };
		if (!flipY) out.reverse();
		return out;
	}
}

void Main()
{
	Window::Resize(1280, 720);
	Scene::SetBackground(ColorF{ 0.15 });

	const Array<std::pair<String, Polygon>> polys = {
		{ U"Straight",      Polygon{ normalize(kPx_Straight,     kPxXMax_Straight,     kWidthStraight_m, kPxYRange_Straight,     false) } },
		{ U"Left",          Polygon{ normalize(kPx_Left,         kPxXMax_Left,         kWidthTurn_m,     kPxYRange_Left,         false) } },
		{ U"Right(flipY)",  Polygon{ normalize(kPx_Left,         kPxXMax_Left,         kWidthTurn_m,     kPxYRange_Left,         true)  } },
		{ U"StraightLeft",  Polygon{ normalize(kPx_StraightLeft, kPxXMax_StraightLeft, kWidthCombined_m, kPxYRange_StraightLeft, false) } },
		{ U"StraightRight", Polygon{ normalize(kPx_StraightLeft, kPxXMax_StraightLeft, kWidthCombined_m, kPxYRange_StraightLeft, true)  } },
	};

	for (const auto& [name, poly] : polys)
		Print << name << U" empty=" << poly.isEmpty() << U" verts=" << poly.outer().size();

	// 1m = 80px。各タイプを縦に 130px 間隔で並べる
	// ローカル X=[0,5m] → 画面 X=[50, 450]
	// ローカル Z(横)=[-halfW, +halfW] → 画面 Y (中心からのオフセット)
	constexpr double kScale  = 80.0;
	constexpr double kStartX = 50.0;  // ローカル X=0 の画面 X 位置
	constexpr double kStartY = 80.0;
	constexpr double kStepY  = 130.0;

	int frame = 0;
	while (System::Update())
	{
		for (int i = 0; i < static_cast<int>(polys.size()); ++i)
		{
			const auto& [name, poly] = polys[i];
			const double cy = kStartY + i * kStepY;

			// ラベル
			FontAsset(U"debug")(name + (poly.isEmpty() ? U" [EMPTY]" : U""))
				.draw(10, cy - 55, poly.isEmpty() ? Palette::Red : Palette::White);

			// 車線幅参考線 (±1.75m)
			const double laneHW = 1.75 * kScale;
			Line{ kStartX, cy - laneHW, kStartX + 5 * kScale, cy - laneHW }.draw(1.0, ColorF{ 0.8, 0.8, 0.0, 0.5 });
			Line{ kStartX, cy + laneHW, kStartX + 5 * kScale, cy + laneHW }.draw(1.0, ColorF{ 0.8, 0.8, 0.0, 0.5 });

			if (poly.isEmpty()) continue;

			// ローカル座標 → 画面座標変換して描画
			Array<Vec2> screen;
			for (const auto& v : poly.outer())
				screen << Vec2{ kStartX + v.x * kScale, cy + v.y * kScale };
			Polygon{ screen }.draw(ColorF{ 1.0, 1.0, 1.0, 0.75 });
			Polygon{ screen }.drawFrame(1.0, Palette::Yellow);
		}

		if (frame == 3) ScreenCapture::SaveCurrentFrame(U"arrow_test.png");
		if (frame > 3) break;
		++frame;
	}
}
