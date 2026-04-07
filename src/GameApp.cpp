#include "GameApp.hpp"
#include "asset/AssetRegistrar.hpp"

void GameApp::run()
{
	constexpr int kWindowWidth  = 1280;
	constexpr int kWindowHeight = 768;

	System::SetTerminationTriggers(UserAction::CloseButtonClicked);
	Window::Resize(kWindowWidth, kWindowHeight);
	Scene::SetBackground(ColorF{ 0.2, 0.3, 0.4 });
	Graphics::SetVSyncEnabled(false);
	Window::SetTitle(U"Pavecity");

	RegisterAssets();

	App manager;
	manager.add<TitleScene>(SceneState::Title);
	manager.add<GameScene>(SceneState::Game);

	// コマンドライン引数: --load <saveName> でタイトルを飛ばして直接ロード
	const auto args = System::GetCommandLineArgs();
	bool directLoad = false;
	for (size_t i = 0; i < args.size(); ++i)
	{
		if (args[i] == U"--load" && i + 1 < args.size())
		{
			auto data = manager.get();
			data->isNewGame   = false;
			data->saveName    = args[i + 1];
			data->sandboxMode = true;
			directLoad = true;
			break;
		}
	}

	manager.init(directLoad ? SceneState::Game : SceneState::Title, 0s);

	while (System::Update())
	{
		if (!manager.update())
			break;
	}
}
