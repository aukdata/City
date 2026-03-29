#include "TitleScene.hpp"

TitleScene::TitleScene(const InitData& init)
	: IScene{ init }
	, m_selectedSeed{ getData().seed }
	, m_sandboxMode{ getData().sandboxMode }
{
	m_seedTextState.text = Format(m_selectedSeed);
	scanSaves();
}

void TitleScene::scanSaves() const
{
	m_saveNames.clear();
	const FilePath savesDir = U"saves";
	if (!FileSystem::Exists(savesDir)) return;

	for (const auto& entry : FileSystem::DirectoryContents(savesDir, Recursive::No))
	{
		if (!FileSystem::IsDirectory(entry)) continue;

		// DirectoryContents は末尾 / 付きパスを返すため除去してから名前を取得
		String path = entry;
		while (path.ends_with(U'/') || path.ends_with(U'\\'))
			path.pop_back();
		const String name = FileSystem::FileName(path);
		if (name.isEmpty()) continue;

		if (FileSystem::Exists(entry + U"meta.json") || FileSystem::Exists(entry + U"/meta.json"))
			m_saveNames << name;
	}
	m_saveNames.sort();
}

void TitleScene::update()
{
	if (m_startRequested)
	{
		getData().seed        = m_selectedSeed;
		getData().sandboxMode = m_sandboxMode;
		getData().saveName.clear();
		changeScene(SceneState::Game, 0s);
	}

	if (m_loadRequested && m_selectedSave >= 0
	    && m_selectedSave < static_cast<int>(m_saveNames.size()))
	{
		getData().saveName    = m_saveNames[m_selectedSave];
		getData().sandboxMode = m_sandboxMode;
		changeScene(SceneState::Game, 0s);
	}
}

void TitleScene::draw() const
{
	static const Font titleFont{ 46, Typeface::Bold };
	static const Font subFont  { 16 };
	static const Font labelFont{ 17 };
	static const Font listFont { 15 };

	const int W = Scene::Width();
	const int H = Scene::Height();

	// ---- 背景 ----
	Rect{ 0, 0, W, H }.draw(ColorF{ 0.07, 0.11, 0.16 });

	// ---- タイトル ----
	titleFont(U"City Simulation").drawAt(W * 0.5, 80, ColorF{ 0.90, 0.95, 1.00 });
	subFont(U"プロシージャル都市生成シミュレーター").drawAt(W * 0.5, 135, ColorF{ 0.52, 0.63, 0.74 });

	// ============ 左側: 新規生成 ============
	const int colL = 80;

	labelFont(U"-- 新規生成 --").draw(Vec2{ colL, 185 }, ColorF{ 0.78, 0.86, 0.93 });

	// シード値
	labelFont(U"シード値").draw(Vec2{ colL, 218 }, ColorF{ 0.78, 0.86, 0.93 });
	SimpleGUI::TextBox(m_seedTextState, Vec2{ colL, 244 }, 220);

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

	if (SimpleGUI::Button(U"ランダム", Vec2{ colL + 230, 244 }, 100))
	{
		m_selectedSeed       = static_cast<uint64>(Random(10000000, 99999999));
		m_seedTextState.text = Format(m_selectedSeed);
	}

	// サンドボックスモード
	SimpleGUI::CheckBox(m_sandboxMode, U"サンドボックスモード",
	                    Vec2{ colL, 290 }, 300);

	// 生成開始ボタン
	if (SimpleGUI::Button(U"生成開始", Vec2{ colL, 340 }, 200))
	{
		m_startRequested = true;
	}

	// ============ 右側: セーブデータロード ============
	const int colR = W / 2 + 40;

	labelFont(U"-- セーブデータ --").draw(Vec2{ colR, 185 }, ColorF{ 0.78, 0.86, 0.93 });

	if (m_saveNames.isEmpty())
	{
		listFont(U"セーブデータがありません").draw(Vec2{ colR, 220 }, ColorF{ 0.5 });
	}
	else
	{
		constexpr int kItemH = 28;
		constexpr int kListW = 300;
		const int listY = 218;

		for (int i = 0; i < static_cast<int>(m_saveNames.size()); ++i)
		{
			const int iy = listY + i * kItemH;
			const RectF r{ static_cast<double>(colR), static_cast<double>(iy),
			               static_cast<double>(kListW), static_cast<double>(kItemH - 2) };
			const bool selected = (m_selectedSave == i);
			const bool hover = r.mouseOver();

			r.draw(selected ? ColorF{ 0.25, 0.40, 0.65 }
			       : (hover ? ColorF{ 0.20, 0.25, 0.35 } : ColorF{ 0.12, 0.14, 0.20 }));

			listFont(m_saveNames[i]).draw(Vec2{ colR + 8, iy + 4 },
				selected ? ColorF{ 1.0 } : ColorF{ 0.8 });

			if (hover && MouseL.down())
				m_selectedSave = i;
		}

		// ロードボタン
		const bool canLoad = (m_selectedSave >= 0);
		if (SimpleGUI::Button(U"ロード", Vec2{ colR, listY + static_cast<int>(m_saveNames.size()) * kItemH + 10 },
		                      200, canLoad))
		{
			m_loadRequested = true;
		}
	}

	// 更新ボタン
	if (SimpleGUI::Button(U"更新", Vec2{ colR + 220, 185 }, 80))
	{
		scanSaves();
	}
}
