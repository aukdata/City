
#include "UIRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"

void UIRenderer::render(const GameClock& clock, int vehicleCount, StringView modeText,
                        const Economy& economy)
{
	const auto& font      = FontAsset(Asset::UI24);
	const auto& smallFont = FontAsset(Asset::Small16);

	// 左上: 時刻・速度・経済情報
	Rect{ 10, 10, 300, 120 }.draw(ColorF{ 0, 0, 0, 0.5 });
	font(clock.timeString()).draw(16, Vec2{ 18, 16 }, Palette::White);
	font(clock.speedString()).draw(16, Vec2{ 18, 40 }, Palette::Yellow);
	smallFont(U"車両数: {}"_fmt(vehicleCount)).draw(12, Vec2{ 18, 64 }, Palette::White);

	// 資金・人口
	const ColorF fundsColor = (economy.funds >= 0.0) ? Palette::Lightgreen : Palette::Tomato;
	smallFont(U"資金: {:.1f}億円"_fmt(economy.funds)).draw(12, Vec2{ 18, 82 }, fundsColor);
	smallFont(U"人口: {}人"_fmt(economy.population)).draw(12, Vec2{ 18, 100 }, Palette::White);

	// 下中央: 編集モード
	if (!modeText.empty())
	{
		font(modeText).drawAt(20, Vec2{ Scene::Width() / 2.0, Scene::Height() - 30.0 }, Palette::White);
	}

	// 右下: 操作ガイド
	smallFont(
		U"WASD:移動  右ドラッグ:回転  ホイール:ズーム  F:カメラ切替\n"
		U"R:道路  Z:ゾーン塗り  G:地形  X:線路  B:バス路線\n"
		U"Tab:ゾーン表示  T:車両生成  0:一時停止  1-3:速度"
	).draw(12, Vec2{ Scene::Width() - 430.0, Scene::Height() - 62.0 }, Palette::White);
}
