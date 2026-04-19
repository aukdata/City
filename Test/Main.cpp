// 路面標示矢印 TOML を reference/204.ht{1,2,3}.gif 上にオーバーレイ
// 参考画像座標系を基準に、画像の実測シャフト幅で TOML y をスケール（shaft が必ず重なる）
// Right/StraightRight は ht1/ht3 をそのまま表示し、TOML y は +y 側（上方向）に出るので
// 下側 hook ではなく上側 hook を描くことで Left のミラーになっていることを確認する
# include <Siv3D.hpp>

namespace
{
	Array<Vec2> LoadArrowVerts(FilePathView path)
	{
		Array<Vec2> v;
		const TOMLReader t{ path };
		if (!t) return v;
		for (const auto& row : t[U"verts"].arrayView())
		{
			if (row.arrayCount() < 2) continue;
			const auto a = row.arrayView();
			v << Vec2{ a[0].get<double>(), a[1].get<double>() };
		}
		return v;
	}

	struct Item
	{
		String label;
		String tomlFile;
		String refImage;
		double refTipX;     // image 内 arrow tip の x (bbox 左端)
		double refCenterY;  // image 内 shaft 中心 y
		double imgWidthPx;  // arrow 全長の image px 幅
		double shaftPx;     // image 内の shaft 太さ (px)
	};
}

void Main()
{
	Window::Resize(1400, 1600);
	Scene::SetBackground(ColorF{ 0.92, 0.92, 0.92 });

	// 画素実測 (cv2 で確認済):
	//   ht1.gif: bbox x=[92,584] y=[77,96] at tail, shaft 19px, tip y=115
	//   ht2.gif: bbox x=[83,584] y=[97,115] at tail, shaft 18px, tip y=101
	//   ht3.gif: bbox x=[85,586] y=[97,115] at tail, shaft 18px, tip y=106
	const Array<Item> items = {
		{ U"Straight (ht2)",        U"../../App/assets/road_markings/straight.toml",      U"../../reference/204.ht2.gif", 83,  106, 501, 18 },
		{ U"Left (ht1)",            U"../../App/assets/road_markings/left.toml",          U"../../reference/204.ht1.gif", 92,   86.5, 492, 19 },
		{ U"Right (ht1 ref)",       U"../../App/assets/road_markings/right.toml",         U"../../reference/204.ht1.gif", 92,   86.5, 492, 19 },
		{ U"StraightLeft (ht3)",    U"../../App/assets/road_markings/straightleft.toml",  U"../../reference/204.ht3.gif", 85,  105,   501, 18 },
		{ U"StraightRight (ht3)",   U"../../App/assets/road_markings/straightright.toml", U"../../reference/204.ht3.gif", 85,  105,   501, 18 },
	};

	Array<Array<Vec2>> vertsList;
	for (const auto& it : items) vertsList << LoadArrowVerts(it.tomlFile);

	Array<Texture> texList;
	for (const auto& it : items) texList << Texture{ it.refImage };

	const Font font{ 18, Typeface::Bold };

	int frame = 0;
	while (System::Update())
	{
		Scene::SetBackground(ColorF{ 0.92, 0.92, 0.92 });

		for (size_t i = 0; i < items.size(); ++i)
		{
			const auto& it = items[i];
			const double baseY = 10 + i * 260;
			const double baseX = 40;

			font(it.label).draw(baseX, baseY, ColorF{ 0.0 });

			if (!texList[i]) continue;

			const double imgX = baseX + 100;
			const double imgY = baseY + 30;

			texList[i].draw(imgX, imgY);

			// TOML の shaft 15cm が image の shaft(18-19px) に一致するように y をスケール
			// x は arrow 全長 5m が imgWidthPx に一致するようにスケール
			const double xScale = it.imgWidthPx / 5.0;                     // px per TOML m
			const double yScale = it.shaftPx / (2.0 * 0.075);              // px per TOML m (shaft half 0.075m)

			Array<Vec2> screen;
			for (const auto& v : vertsList[i])
			{
				// TOML tip (+2.5) → image refTipX, tail (-2.5) → refTipX + imgWidthPx
				const double sx = imgX + it.refTipX + (2.5 - v.x) * xScale;
				const double sy = imgY + it.refCenterY - v.y * yScale;
				screen << Vec2{ sx, sy };
			}

			if (screen.size() >= 3)
			{
				const Polygon poly{ screen };
				if (poly)
				{
					poly.draw(ColorF{ 1.0, 0.1, 0.1, 0.45 });
					poly.drawFrame(1.5, ColorF{ 0.85, 0.0, 0.0 });
				}
				else
				{
					// Polygon 化失敗時は頂点を線で結んで描画
					for (size_t k = 0; k + 1 < screen.size(); ++k)
						Line{ screen[k], screen[k+1] }.draw(2, ColorF{ 0.85, 0, 0 });
					Line{ screen.back(), screen.front() }.draw(2, ColorF{ 0.85, 0, 0 });
				}
				for (const auto& p : screen)
					Circle{ p, 2.5 }.draw(ColorF{ 0.1, 0.1, 0.8 });
			}
		}

		if (frame == 3)
			ScreenCapture::SaveCurrentFrame(U"arrow_overlay.png");
		if (frame > 3) break;
		++frame;
	}
	System::Exit();
}
