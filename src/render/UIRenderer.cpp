
#include "UIRenderer.hpp"

UIRenderer::UIRenderer()
	: m_font{ FontMethod::MSDF, 24 }
	, m_smallFont{ FontMethod::MSDF, 16 }
{
}

void UIRenderer::render(const GameClock& clock, int vehicleCount, StringView modeText,
                        const Economy& economy)
{
	// 左上: 時刻・速度・経済情報
	Rect{ 10, 10, 300, 120 }.draw(ColorF{ 0, 0, 0, 0.5 });
	m_font(clock.timeString()).draw(16, Vec2{ 18, 16 }, Palette::White);
	m_font(clock.speedString()).draw(16, Vec2{ 18, 40 }, Palette::Yellow);
	m_smallFont(U"Vehicles: {}"_fmt(vehicleCount)).draw(12, Vec2{ 18, 64 }, Palette::White);

	// 資金・人口
	const ColorF fundsColor = (economy.funds >= 0.0) ? Palette::Lightgreen : Palette::Tomato;
	m_smallFont(U"資金: {:.1f}億円"_fmt(economy.funds)).draw(12, Vec2{ 18, 82 }, fundsColor);
	m_smallFont(U"人口: {}人"_fmt(economy.population)).draw(12, Vec2{ 18, 100 }, Palette::White);

	// 下中央: 編集モード
	if (!modeText.empty())
	{
		m_font(modeText).drawAt(20, Vec2{ Scene::Width() / 2.0, Scene::Height() - 30.0 }, Palette::White);
	}

	// 右下: 操作ガイド
	m_smallFont(
		U"WASD:移動  右ドラッグ:回転  ホイール:ズーム  F:カメラ切替\n"
		U"R:道路  Z:ゾーン塗り  G:地形  X:線路  B:バス路線\n"
		U"Tab:ゾーン表示  T:車両生成  0:一時停止  1-3:速度"
	).draw(12, Vec2{ Scene::Width() - 430.0, Scene::Height() - 62.0 }, Palette::White);
}
