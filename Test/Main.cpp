# include <Siv3D.hpp>

// ===== reference/204.ht*.gif から OpenCV で抽出した輪郭 =====
// 1px = 1単位, X 左端=0, Y 中心=0, 巻き順 CW (Siv3D 互換)

namespace ArrowContour
{
	static const Array<Vec2> kStraight = {
		Vec2{ 253.0, -26.5 }, Vec2{ 254.0, -10.5 }, Vec2{ 501.0, -10.5 },
		Vec2{ 501.0,   9.5 }, Vec2{ 254.0,   9.5 }, Vec2{ 253.0,  26.5 },
		Vec2{   0.0,  -5.5 },
	};

	static const Array<Vec2> kLeft = {
		Vec2{  75.0,   0.0 }, Vec2{  89.0, -24.0 }, Vec2{ 113.0, -38.0 },
		Vec2{ 492.0, -38.0 }, Vec2{ 492.0, -19.0 }, Vec2{ 147.0, -19.0 },
		Vec2{ 135.0, -10.0 }, Vec2{ 134.0,   0.0 }, Vec2{ 208.0,   0.0 },
		Vec2{ 103.0,  38.0 }, Vec2{   0.0,   0.0 },
	};

	static const Array<Vec2> kStraightLeft = {
		Vec2{ 222.0, -46.5 }, Vec2{ 223.0, -28.5 }, Vec2{ 501.0, -28.5 },
		Vec2{ 501.0, -10.5 }, Vec2{ 428.0, -10.5 }, Vec2{ 416.0,  -6.5 },
		Vec2{ 409.0,   9.5 }, Vec2{ 483.0,   9.5 }, Vec2{ 373.0,  46.5 },
		Vec2{ 260.0,   9.5 }, Vec2{ 334.0,   9.5 }, Vec2{ 339.0,  -2.5 },
		Vec2{ 352.0, -10.5 }, Vec2{ 223.0, -10.5 }, Vec2{ 219.0,   8.5 },
		Vec2{   0.0, -18.5 },
	};
}

/// @brief 2D Polygon を XZ 平面の MeshData に変換 (Y=0, normal=+Y)
MeshData PolygonToMeshDataXZ(const Polygon& polygon, double scale = 1.0)
{
	MeshData md;
	const auto& outerVerts = polygon.outer();
	const auto& triIndices = polygon.indices();

	md.vertices.reserve(outerVerts.size());
	for (const auto& v : outerVerts)
	{
		Vertex3D vert;
		vert.pos = Float3{ static_cast<float>(v.x * scale), 0.0f, static_cast<float>(v.y * scale) };
		vert.normal = Float3{ 0.0f, 1.0f, 0.0f };
		vert.tex = Float2{ static_cast<float>(v.x), static_cast<float>(v.y) };
		md.vertices << vert;
	}

	md.indices.reserve(triIndices.size());
	for (const auto& tri : triIndices)
	{
		md.indices << TriangleIndex32{ tri.i0, tri.i1, tri.i2 };
	}

	return md;
}

void Main()
{
	Scene::SetBackground(ColorF{ 0.45, 0.45, 0.5 });
	Window::Resize(1400, 900);

	const Font font{ 16 };

	const Polygon pStraight{ ArrowContour::kStraight };
	const Polygon pLeft{ ArrowContour::kLeft };
	const Polygon pStraightLeft{ ArrowContour::kStraightLeft };

	// MeshData 生成 (検証: Polygon の三角形分割を MeshData に詰める)
	const MeshData mdStraight = PolygonToMeshDataXZ(pStraight, 1.0);
	const MeshData mdLeft = PolygonToMeshDataXZ(pLeft, 1.0);
	const MeshData mdStraightLeft = PolygonToMeshDataXZ(pStraightLeft, 1.0);

	const String diag = U"Polygons -- Straight: {}v {}t / Left: {}v {}t / S+L: {}v {}t  |  MeshData OK"_fmt(
		pStraight.outer().size(), pStraight.indices().size(),
		pLeft.outer().size(), pLeft.indices().size(),
		pStraightLeft.outer().size(), pStraightLeft.indices().size()
	);

	const Texture refHt1{ U"example/reference/204.ht1.png" };
	const Texture refHt2{ U"example/reference/204.ht2.png" };
	const Texture refHt3{ U"example/reference/204.ht3.png" };

	int frame = 0;
	constexpr int kCaptureFrame = 5;

	while (System::Update())
	{
		font(diag).draw(20, 870, ColorF{ 1, 1, 0 });
		font(U"Reference (left)  vs  Generated Polygon (right)").draw(450, 5, ColorF{ 1 });

		auto drawRow = [&](int row, const Texture& ref, const Polygon& poly, const String& name)
		{
			const double yBase = 30 + row * 280;
			font(name).draw(20, static_cast<int32>(yBase), ColorF{ 1 });

			ref.draw(20, yBase + 25);

			const double offX = 700;
			const double offY = yBase + 25 + 218 / 2.0;
			Transformer2D t{ Mat3x2::Translate(offX, offY) };
			poly.draw(ColorF{ 1 });
		};

		drawRow(0, refHt2, pStraight, U"ht2: Straight");
		drawRow(1, refHt1, pLeft, U"ht1: Left Turn");
		drawRow(2, refHt3, pStraightLeft, U"ht3: Straight + Left");

		if (frame == kCaptureFrame)
		{
			ScreenCapture::SaveCurrentFrame(U"arrow_test.png");
		}
		if (frame > kCaptureFrame)
		{
			break;
		}
		++frame;
	}
}
