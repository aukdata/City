#include "TitleScene.hpp"

TitleScene::TitleScene(const InitData& init)
	: IScene{ init }
	, m_selectedSeed{ getData().seed }
	, m_sandboxMode{ getData().sandboxMode }
{
	m_seedTextState.text = Format(m_selectedSeed);
}

void TitleScene::update()
{
	if (m_startRequested)
	{
		getData().seed        = m_selectedSeed;
		getData().sandboxMode = m_sandboxMode;
		changeScene(SceneState::Game, 0s);
	}
}

void TitleScene::draw() const
{
	static const Font titleFont{ 46, Typeface::Bold };
	static const Font subFont  { 16 };
	static const Font labelFont{ 17 };

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

	// ---- サンドボックスモード チェックボックス ----
	{
		constexpr int kCbW = 360;
		SimpleGUI::CheckBox(m_sandboxMode, U"サンドボックスモード",
		                    Vec2{ (W - kCbW) / 2, 310 }, kCbW);
	}

	// ---- 生成開始ボタン ----
	constexpr int kBtnW = 220;
	if (SimpleGUI::Button(U"生成開始", Vec2{ (W - kBtnW) / 2, 370 }, kBtnW))
	{
		m_startRequested = true;
	}
}
