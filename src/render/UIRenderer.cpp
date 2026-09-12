#include "UIRenderer.hpp"
#include "../asset/AssetRegistrar.hpp"

namespace
{
	ColorF moneyColor(double value)
	{
		return (value >= 0.0) ? ColorF{ Palette::Lightgreen } : ColorF{ Palette::Tomato };
	}

	ColorF goodRatioColor(double value, double good, double warning)
	{
		if (value >= good)
		{
			return ColorF{ Palette::Lightgreen };
		}
		if (value >= warning)
		{
			return ColorF{ 1.0, 0.82, 0.25 };
		}
		return ColorF{ Palette::Tomato };
	}

	ColorF congestionColor(double value)
	{
		if (value < 0.35)
		{
			return ColorF{ Palette::Lightgreen };
		}
		if (value < 0.65)
		{
			return ColorF{ 1.0, 0.82, 0.25 };
		}
		return ColorF{ Palette::Tomato };
	}

	String signedMoneyText(double value)
	{
		return (value >= 0.0)
			? U"+{:.2f}億円"_fmt(value)
			: U"{:.2f}億円"_fmt(value);
	}

	String clippedText(StringView text, size_t maxChars)
	{
		String s{ text };
		if (s.size() <= maxChars)
		{
			return s;
		}
		if (maxChars <= 3)
		{
			return s.substr(0, maxChars);
		}
		return s.substr(0, maxChars - 3) + U"...";
	}
}

void UIRenderer::updateLayout(const CityHudStats& stats)
{
	m_left.expanded={10,10,360,268};
	const int active=Max(1,Min(3,static_cast<int>(stats.activeEventSummaries.size())));
	const int notices=Max(1,Min(2,static_cast<int>(stats.notificationSummaries.size())));
	const Vec2 position=Scene::Width()>=750 ? Vec2{Scene::Width()-370.0,10} : Vec2{10,m_left.bounds().br().y+10};
	m_right.expanded={position,360,52.0+(active+notices)*18+212};
}

void UIRenderer::handleInput()
{
	if (MouseL.down()) { m_left.click(Cursor::PosF()); m_right.click(Cursor::PosF()); }
}

void UIRenderer::render(const GameClock& clock, int vehicleCount, StringView modeText,
                        const Economy& economy, const CityHudStats& stats)
{
	updateLayout(stats);
	// 常時参照する HUD 情報だけを画面四隅へ集約し、操作中でも視線移動を短く保つ。
	const auto& font      = FontAsset(Asset::UI24);
	const auto& smallFont = FontAsset(Asset::Small16);

	const RectF mainPanel=m_left.bounds();
	m_left.draw(font,clock.timeString());
	if (!m_left.collapsed)
	{
		double y=16.0;	y += 24.0;
		font(clock.speedString()).draw(16, Vec2{ 18, y }, Palette::Yellow);
		y += 24.0;
		smallFont(U"車両数: {}台 / 人口: {}人"_fmt(vehicleCount, economy.population))
			.draw(12, Vec2{ 18, y }, Palette::White);
		y += 18.0;

		const ColorF fundsColor = (economy.funds >= 0.0) ? ColorF{ Palette::Lightgreen } : ColorF{ Palette::Tomato };
		smallFont(U"資金: {:.1f}億円"_fmt(economy.funds)).draw(12, Vec2{ 18, y }, fundsColor);
		y += 22.0;

		Line{ Vec2{ 18, y }, Vec2{ mainPanel.x + mainPanel.w - 18.0, y } }
			.draw(ColorF{ 1.0, 1.0, 1.0, 0.16 });
		y += 8.0;

		smallFont(U"月次収入: +{:.2f}億円"_fmt(stats.monthlyIncome))
			.draw(12, Vec2{ 18, y }, Palette::Lightgreen);
		y += 18.0;
		smallFont(U"月次支出: -{:.2f}億円"_fmt(stats.monthlyExpense))
			.draw(12, Vec2{ 18, y }, Palette::Tomato);
		y += 18.0;
		smallFont(U"月次収支: {}"_fmt(signedMoneyText(stats.monthlyBalance)))
			.draw(12, Vec2{ 18, y }, moneyColor(stats.monthlyBalance));
		y += 22.0;

		const double happiness = Clamp(economy.happiness, 0.0, 1.0);
		smallFont(U"幸福度: {:.0f}%"_fmt(happiness * 100.0))
			.draw(12, Vec2{ 18, y }, goodRatioColor(happiness, 0.70, 0.45));
		y += 18.0;

		const double housingPercent = stats.housingFulfillment * 100.0;
		const ColorF housingColor = (economy.population <= 0)
			? ColorF{ Palette::White }
			: goodRatioColor(stats.housingFulfillment, 1.0, 0.75);
		smallFont(U"住宅: {}人分 / 充足率 {:.0f}%"_fmt(stats.housingCapacity, housingPercent))
			.draw(12, Vec2{ 18, y }, housingColor);
		y += 18.0;

		if (stats.observedVehicleCount > 0)
		{
			smallFont(U"交通: 平均 {:.1f}km/h ({:.0f}%) / 低速 {}台"_fmt(
				stats.averageSpeedKmh, stats.averageSpeedRatio * 100.0, stats.slowVehicleCount))
				.draw(12, Vec2{ 18, y }, goodRatioColor(stats.averageSpeedRatio, 0.65, 0.35));
		}
		else
		{
			smallFont(U"交通: 速度集計なし").draw(12, Vec2{ 18, y }, Palette::White);
		}
		y += 18.0;

		smallFont(U"混雑: 平均 {:.0f}% / 最大 {:.0f}% / 区間 {}"_fmt(
			stats.averageCongestion * 100.0, stats.maxCongestion * 100.0, stats.congestedEdgeCount))
			.draw(12, Vec2{ 18, y }, congestionColor(stats.maxCongestion));

	}
	const RectF eventPanel=m_right.bounds();
	m_right.draw(font,U"街の動き・地図");
	if (!m_right.collapsed)
	{
		double eventY = eventPanel.y + 36.0;
		if (stats.activeEventSummaries.isEmpty())
		{
			smallFont(U"発生中: なし").draw(12, Vec2{ eventPanel.x + 10.0, eventY }, Palette::White);
			eventY += 18.0;
		}
		else
		{
			const int count = Min(3, static_cast<int>(stats.activeEventSummaries.size()));
			for (int i = 0; i < count; ++i)
			{
				const String prefix = (i == 0) ? U"発生中: " : U"        ";
				smallFont(prefix + clippedText(stats.activeEventSummaries[i], 28))
					.draw(12, Vec2{ eventPanel.x + 10.0, eventY }, ColorF{ 1.0, 0.88, 0.42 });
				eventY += 18.0;
			}
		}

		if (stats.notificationSummaries.isEmpty())
		{
			smallFont(U"通知: なし").draw(12, Vec2{ eventPanel.x + 10.0, eventY }, Palette::White);
		}
		else
		{
			const int count = Min(2, static_cast<int>(stats.notificationSummaries.size()));
			for (int i = 0; i < count; ++i)
			{
				const String prefix = (i == 0) ? U"通知: " : U"      ";
				smallFont(prefix + clippedText(stats.notificationSummaries[i], 30))
					.draw(12, Vec2{ eventPanel.x + 10.0, eventY }, ColorF{ 0.70, 0.90, 1.0 });
				eventY += 18.0;
			}
		}

	}

	if (!modeText.empty())
	{
		font(modeText).drawAt(20, Vec2{ Scene::Width() / 2.0, Scene::Height() - 30.0 }, Palette::White);
	}

	smallFont(
		U"WASD:移動  右ドラッグ:回転  ホイール:ズーム  F:カメラ切替\n"
		U"R:道路  Z:ゾーン塗り  G:地形  X:線路  B:バス路線\n"
		U"Tab:ゾーン表示  T:車両生成  0:一時停止  1-3:速度"
	).draw(12, Vec2{ Scene::Width() - 430.0, Scene::Height() - 62.0 }, Palette::White);
}
