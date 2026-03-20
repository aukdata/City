#include "TitleScene.hpp"

TitleScene::TitleScene(const InitData& init)
	: IScene{ init }
	, m_selectedSeed{ getData().seed }
	, m_selectedTerrain{ getData().terrain }
	, m_sandboxMode{ getData().sandboxMode }
{
	m_seedTextState.text = Format(m_selectedSeed);
}

void TitleScene::update()
{
	if (m_startRequested)
	{
		getData().seed        = m_selectedSeed;
		getData().terrain     = m_selectedTerrain;
		getData().sandboxMode = m_sandboxMode;
		// フェードなしで即座にゲームシーンへ遷移（マップ生成があるため）
		changeScene(SceneState::Game, 0s);
	}
}

void TitleScene::draw() const
{
	static const Font titleFont{ 46, Typeface::Bold };
	static const Font subFont  { 16 };
	static const Font labelFont{ 17 };
	static const Font cardFont { 17, Typeface::Bold };
	static const Font descFont { 13 };

	const int W = Scene::Width();
	const int H = Scene::Height();

	// ---- 背景 ----
	Rect{ 0, 0, W, H }.draw(ColorF{ 0.07, 0.11, 0.16 });

	// ---- タイトル ----
	titleFont(U"City Simulation").drawAt(W * 0.5, 80, ColorF{ 0.90, 0.95, 1.00 });
	subFont(U"プロシージャル都市生成シミュレーター").drawAt(W * 0.5, 135, ColorF{ 0.52, 0.63, 0.74 });

	// ---- シード値 ----
	labelFont(U"シード値").draw(Vec2{ 200, 195 }, ColorF{ 0.78, 0.86, 0.93 });
	SimpleGUI::TextBox(m_seedTextState, Vec2{ 200, 222 }, 280);

	// テキストボックスの内容を uint64 に変換する
	{
		uint64 val  = 0;
		bool   valid = !m_seedTextState.text.isEmpty();
		for (char32 c : m_seedTextState.text)
		{
			if (c >= U'0' && c <= U'9')
				val = val * 10 + (c - U'0');
			else { valid = false; break; }
		}
		if (valid)
			m_selectedSeed = val;
	}

	if (SimpleGUI::Button(U"ランダム", Vec2{ 498, 222 }, 120))
	{
		m_selectedSeed       = static_cast<uint64>(Random(10000000, 99999999));
		m_seedTextState.text = Format(m_selectedSeed);
	}

	// ---- 地形タイプ選択 ----
	labelFont(U"地形タイプ").draw(Vec2{ 200, 286 }, ColorF{ 0.78, 0.86, 0.93 });

	struct TerrainInfo
	{
		TerrainType type;
		String      name;
		String      kana;
		String      desc;
		ColorF      baseColor;
	};
	static const TerrainInfo kInfos[] = {
		{ TerrainType::Basin,
		  U"山間盆地",   U"さんかんぼんち",
		  U"四方を山に囲まれた盆地\n川が中央を縦断する",
		  ColorF{ 0.17, 0.32, 0.25 } },
		{ TerrainType::Coastal,
		  U"沿岸平野",   U"えんがんへいや",
		  U"片側が海、反対側が山地\n港町が核になる",
		  ColorF{ 0.08, 0.24, 0.40 } },
		{ TerrainType::RiverFan,
		  U"河川扇状地", U"かせんせんじょうち",
		  U"山から流れ出る扇状地\n橋が重要インフラになる",
		  ColorF{ 0.30, 0.26, 0.10 } },
		{ TerrainType::Hills,
		  U"丘陵台地",   U"きゅうりょうだいち",
		  U"緩やかな丘が連続する地形\n集落は丘の上に分散する",
		  ColorF{ 0.16, 0.30, 0.16 } },
	};

	constexpr int kCardW = 238, kCardH = 160, kGap = 18;
	const int totalW     = 4 * kCardW + 3 * kGap;
	const int cardStartX = (W - totalW) / 2;
	const int cardY      = 318;

	for (int i = 0; i < 4; ++i)
	{
		const int       cx       = cardStartX + i * (kCardW + kGap);
		const RoundRect card     { static_cast<double>(cx), static_cast<double>(cardY),
		                           static_cast<double>(kCardW), static_cast<double>(kCardH), 8.0 };
		const bool      selected = (kInfos[i].type == m_selectedTerrain);

		card.draw(selected ? kInfos[i].baseColor * 2.2 : kInfos[i].baseColor);
		card.drawFrame(2.5, selected
			? ColorF{ 1.0, 0.88, 0.25 }
			: ColorF{ 0.28, 0.34, 0.42 });

		if (card.leftClicked())
			m_selectedTerrain = kInfos[i].type;

		cardFont(kInfos[i].name).draw(Arg::topLeft = Vec2{ cx + 14, cardY + 12 },
		                              ColorF{ 0.95, 0.95, 0.95 });
		descFont(kInfos[i].kana).draw(Arg::topLeft = Vec2{ cx + 14, cardY + 38 },
		                              ColorF{ 0.62, 0.70, 0.77 });
		descFont(kInfos[i].desc).draw(Arg::topLeft = Vec2{ cx + 14, cardY + 60 },
		                              ColorF{ 0.80, 0.85, 0.87 });
		if (selected)
		{
			descFont(U"✓ 選択中").draw(Arg::topLeft = Vec2{ cx + 14, cardY + 128 },
			                           ColorF{ 1.0, 0.88, 0.25 });
		}
	}

	// ---- サンドボックスモード チェックボックス ----
	{
		constexpr int kCbW = 360;
		SimpleGUI::CheckBox(m_sandboxMode, U"サンドボックスモード",
		                    Vec2{ (W - kCbW) / 2, 495 }, kCbW);
	}

	// ---- 生成開始ボタン ----
	constexpr int kBtnW = 220;
	if (SimpleGUI::Button(U"生成開始", Vec2{ (W - kBtnW) / 2, 540 }, kBtnW))
	{
		m_startRequested = true;
	}
}
