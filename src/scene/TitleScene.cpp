#include "TitleScene.hpp"
#include "../asset/AssetRegistrar.hpp"
#include "../save/SaveCatalog.hpp"

TitleScene::TitleScene(const InitData& init)
	: IScene{init}, m_selectedSeed{getData().seed}, m_sandboxMode{getData().sandboxMode},
	  m_options{getData().generation}
{
	m_seedTextState.text = Format(m_selectedSeed);
	scanSaves();
}

void TitleScene::scanSaves() const
{
	String selected;
	if (m_saves.selected >= 0 && m_saves.selected < static_cast<int>(m_saves.names.size()))
	{
		selected = m_saves.names[m_saves.selected];
	}
	m_saves.names = SaveCatalog::list(U"saves");
	m_saves.selected = -1;
	for (size_t i = 0; i < m_saves.names.size(); ++i)
	{
		if (m_saves.names[i] == selected)
		{
			m_saves.selected = static_cast<int>(i);
		}
	}
	m_saves.first =
		Clamp(m_saves.first, 0, Max(0, static_cast<int>(m_saves.names.size()) - StartScreenControls::kVisibleSaves));
}

void TitleScene::update()
{
	if (m_pendingSettings)
	{
		if (m_pendingSettings->save())
		{
			getData().lowSpec = m_pendingSettings->lowSpec;
			getData().renderDistance = m_pendingSettings->renderDistance;
			getData().effectVolume = m_pendingSettings->effectVolume;
			m_settings.close();
		}
		else { m_settings.error = U"設定を保存できません。保存先の空き容量・権限を確認してください"; }
		m_pendingSettings.reset();
	}
	if (m_settings.visible) { return; }
	if (m_deleteRequested)
	{
		m_deleteRequested = false;
		const auto result = SaveCatalog::remove(U"saves", m_saves.confirming);
		m_saves.error = result.success ? U"" : result.message;
		m_saves.confirming.clear();
		scanSaves();
	}
	if (m_startRequested)
	{
		getData().seed = m_selectedSeed;
		getData().sandboxMode = m_sandboxMode;
		getData().generation = m_options;
		getData().isNewGame = true;
		getData().saveName.clear();
		changeScene(SceneState::Game, 0s);
		return;
	}
	if (m_loadRequested && m_saves.selected >= 0 && m_saves.selected < static_cast<int>(m_saves.names.size()))
	{
		getData().saveName = m_saves.names[m_saves.selected];
		getData().sandboxMode = m_sandboxMode;
		getData().isNewGame = false;
		changeScene(SceneState::Game, 0s);
	}
}

void TitleScene::draw() const
{
	const auto& title = FontAsset(Asset::TitleBold46);
	const auto& font = FontAsset(Asset::Small16);
	const int width = Scene::Width();
	if (m_settings.visible)
	{
		Scene::Rect().draw(ColorF{.07, .11, .16});
		if (m_settings.draw(FontAsset(Asset::CJK14), Scene::Size()) == SettingsPanel::Action::Apply)
		{
			m_pendingSettings = m_settings.value();
		}
		return;
	}
	const double left = Max(24, width / 2 - 370), right = width / 2 + 24;
	Scene::Rect().draw(ColorF{.07, .11, .16});
	title(U"Pavecity").drawAt(width * .5, 60, ColorF{.9, .95, 1});
	font(U"日本のまちと交通をつくる").drawAt(width * .5, 104, ColorF{.6, .73, .8});
	font(U"新しい街").draw(left, 147, ColorF{.92});
	font(U"シード値").draw(left, 178, ColorF{.8});
	SimpleGUI::TextBox(m_seedTextState, {left, 202}, 224);
	const auto parsed = ParseOpt<uint64>(m_seedTextState.text);
	if (parsed)
	{
		m_selectedSeed = *parsed;
	}
	if (SimpleGUI::Button(U"ランダム", {left + 236, 202}, 110))
	{
		m_selectedSeed = Random<uint64>(10000000, 99999999);
		m_seedTextState.text = Format(m_selectedSeed);
	}
	SimpleGUI::CheckBox(m_sandboxMode, U"サンドボックス", {left, 249}, 346);
	font(U"生成するもの").draw(left, 296, ColorF{.92});
	const Vec2 options{left, 326};
	StartScreenControls::selectOption(options, m_options, Cursor::PosF(), MouseL.down());
	StartScreenControls::drawOptions(options, m_options, font);
	if (SimpleGUI::Button(U"生成開始", {left, 492}, 346, parsed.has_value()))
	{
		m_startRequested = true;
	}
	font(U"地形・海は常に生成します").draw(13, Vec2{left, 537}, ColorF{.6, .7, .75});
	font(U"灰色の項目は道路・建物などが必要です").draw(13, Vec2{left, 560}, ColorF{.6, .7, .75});
	font(U"セーブデータ").draw(right, 147, ColorF{.92});
	if (SimpleGUI::Button(U"更新", {right + 256, 141}, 84, m_saves.confirming.isEmpty()))
	{
		scanSaves();
	}
	const Vec2 saves{right, 182};
	const auto action =
		StartScreenControls::interactSaves(saves, m_saves, Cursor::PosF(), MouseL.down(), Mouse::Wheel());
	m_loadRequested = action == StartScreenControls::Action::Load;
	m_deleteRequested = action == StartScreenControls::Action::Delete;
	StartScreenControls::drawSaves(saves, m_saves, font);
	if (SimpleGUI::Button(U"設定", {width - 150.0, 24}, 120))
	{
		m_seedTextState.active = false;
		m_settings.open({getData().lowSpec, getData().renderDistance, getData().effectVolume});
	}
}
