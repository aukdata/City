// 案内標識背景色 vs 国道おにぎりアイコン背景色の一致検証
// リファクタ後（kDefaultBgColor 公開 + resolveBgColor 統一）でも
// 描画される看板の青が national_route.png の青と視覚・ピクセル両方で一致すること
# include <Siv3D.hpp>
# include "src/asset/AssetRegistrar.hpp"
# include "src/road/GuideSign.hpp"
# include "src/road/RoadTypes.hpp"

namespace
{
	GuideSignPlacement MakeSampleSign()
	{
		GuideSignPlacement g;
		g.id = 1;
		g.kind = GuideSignKind::DirectionDistance;
		g.bgColor = ColorF{ 0, 0, 0, 0 };  // alpha = 0 → resolveBgColor がデフォルトを返す
		g.widthOverride = 3.0f;
		g.heightOverride = 1.2f;

		// 左に上矢印、右中央におにぎり、右に地名と距離を並べる（視覚比較用）
		SignElement arrow;
		arrow.kind = SignElementKind::Arrow;
		arrow.posX = 0.12f;
		arrow.posY = 0.50f;
		arrow.arrowAngle = 0.0f;
		arrow.arrowLength = 180.0f;
		g.elements << arrow;

		SignElement onigiri;
		onigiri.kind = SignElementKind::RouteNumber;
		onigiri.posX = 0.35f;
		onigiri.posY = 0.50f;
		onigiri.value = 4.0f;
		onigiri.scale = 1.0f;
		g.elements << onigiri;

		SignElement destName;
		destName.kind = SignElementKind::DestName;
		destName.posX = 0.55f;
		destName.posY = 0.50f;
		destName.text = U"東京";
		destName.reading = U"Tokyo";
		g.elements << destName;

		SignElement destDist;
		destDist.kind = SignElementKind::DestDistance;
		destDist.posX = 0.90f;
		destDist.posY = 0.50f;
		destDist.value = 50.0f;
		g.elements << destDist;

		return g;
	}

	ColorF SampleTextureCenterColor(const Image& img)
	{
		const int32 cx = img.width() / 2;
		const int32 cy = img.height() / 2;
		return ColorF{ img[cy][cx] };
	}
}

void Main()
{
	Window::Resize(1200, 700);
	Scene::SetBackground(ColorF{ 0.25, 0.25, 0.28 });
	RegisterAssets();

	const Font& fontJa = FontAsset(Asset::CJK32Bold);
	const Font& fontNum = FontAsset(Asset::Arial24);

	const Texture nationalRouteTex{ U"../../App/assets/signs/guide/national_route.png" };

	const GuideSignPlacement sign = MakeSampleSign();
	const auto boardSize = GuideSign::computeBoardSizeFor(sign);
	const Size texSize = GuideSign::guideSignTexSize(boardSize.width, boardSize.height);

	const ColorF resolvedBg = GuideSign::resolveBgColor(sign.bgColor);
	Logger << U"=== GuideSign refactor verification ===";
	Logger << U"kDefaultBgColor  = RGB("
		<< static_cast<int32>(GuideSign::kDefaultBgColor.r * 255 + 0.5) << U","
		<< static_cast<int32>(GuideSign::kDefaultBgColor.g * 255 + 0.5) << U","
		<< static_cast<int32>(GuideSign::kDefaultBgColor.b * 255 + 0.5) << U")";
	Logger << U"resolved bgColor = RGB("
		<< static_cast<int32>(resolvedBg.r * 255 + 0.5) << U","
		<< static_cast<int32>(resolvedBg.g * 255 + 0.5) << U","
		<< static_cast<int32>(resolvedBg.b * 255 + 0.5) << U")";
	Logger << U"boardSize = " << boardSize.width << U" x " << boardSize.height << U" [m]";
	Logger << U"texSize   = " << texSize;

	// national_route PNG の中央ピクセル色を実測
	{
		const Image img{ U"../../App/assets/signs/guide/national_route.png" };
		const ColorF iconCenter = SampleTextureCenterColor(img);
		Logger << U"national_route.png center pixel = RGB("
			<< static_cast<int32>(iconCenter.r * 255 + 0.5) << U","
			<< static_cast<int32>(iconCenter.g * 255 + 0.5) << U","
			<< static_cast<int32>(iconCenter.b * 255 + 0.5) << U")";
	}

	RenderTexture signRt{ static_cast<uint32>(texSize.x), static_cast<uint32>(texSize.y),
	                      resolvedBg, TextureFormat::R8G8B8A8_Unorm_SRGB };

	int frame = 0;
	constexpr int kCaptureFrame = 5;
	while (System::Update())
	{
		// 案内標識テクスチャを合成（フォントのロード完了を待って毎フレーム再生成）
		if (fontJa && fontNum)
		{
			const ScopedRenderTarget2D target{ signRt };
			const ScopedRenderStates2D blend{ BlendState::Default2D };
			Rect{ 0, 0, texSize }.draw(resolvedBg);
			GuideSign::renderContents(sign, texSize, fontJa, fontNum);
			Graphics2D::Flush();
		}

		// 描画レイアウト: 左=合成した案内標識 / 右上=national_route.png / 下=色スウォッチ比較
		const double scale = Min(700.0 / texSize.x, 420.0 / texSize.y);
		signRt.scaled(scale).draw(40, 60);
		fontNum(U"GuideSign (resolveBgColor default)").draw(40, 30, ColorF{ 1 });

		nationalRouteTex.resized(280).draw(820, 60);
		fontNum(U"national_route.png").draw(820, 30, ColorF{ 1 });

		// 色スウォッチ: リファクタ後の kDefaultBgColor と PNG 中央色を並べる
		const double swY = 520;
		Rect{ 40, static_cast<int32>(swY), 300, 80 }.draw(GuideSign::kDefaultBgColor);
		fontNum(U"kDefaultBgColor").draw(40, swY + 85, ColorF{ 1 });

		{
			const Image img{ U"../../App/assets/signs/guide/national_route.png" };
			const ColorF iconBg = SampleTextureCenterColor(img);
			Rect{ 420, static_cast<int32>(swY), 300, 80 }.draw(iconBg);
			fontNum(U"PNG center pixel").draw(420, swY + 85, ColorF{ 1 });
		}

		fontNum(U"Expected: visually identical (RGB 21,87,161)").draw(40, swY + 120, ColorF{ 1, 1, 0.5 });

		if (frame == kCaptureFrame)
		{
			ScreenCapture::SaveCurrentFrame(U"guide_sign_bgcolor.png");

			// 合成した看板テクスチャの左上隅（文字・矢印が無い背景領域）の実際のピクセル値を Logger で確認
			Image signImg;
			signRt.readAsImage(signImg);
			if (signImg)
			{
				const ColorF corner = ColorF{ signImg[5][5] };
				Logger << U"rendered sign corner pixel = RGB("
					<< static_cast<int32>(corner.r * 255 + 0.5) << U","
					<< static_cast<int32>(corner.g * 255 + 0.5) << U","
					<< static_cast<int32>(corner.b * 255 + 0.5) << U")";
			}
		}
		if (frame > kCaptureFrame) break;
		++frame;
	}
}
