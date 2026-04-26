#include "GameApp.hpp"
#include "asset/AssetRegistrar.hpp"
#include "debug/DebugLog.hpp"

void GameApp::run()
{
	// 起動時にウィンドウ・描画設定・共有アセットを初期化し、その後 SceneManager へ制御を渡す。
	constexpr int kWindowWidth  = 1280;
	constexpr int kWindowHeight = 768;

	System::SetTerminationTriggers(UserAction::CloseButtonClicked);
	Window::Resize(kWindowWidth, kWindowHeight);
	Scene::SetBackground(ColorF{ 0.2, 0.3, 0.4 });
	Graphics::SetVSyncEnabled(false);
	Window::SetTitle(U"Pavecity");

	RegisterAssets();
	DebugLog::initialize(U"debug.log");
	DebugLog::print(U"[GameApp] started");

	App manager;
	manager.add<TitleScene>(SceneState::Title);
	manager.add<GameScene>(SceneState::Game);

	// コマンドライン指定があればタイトル画面を介さず、開始条件だけ SceneData へ直接流し込む。
	const auto args = System::GetCommandLineArgs();
	bool directStart = false;
	for (size_t i = 0; i < args.size(); ++i)
	{
		if (args[i] == U"--new")
		{
			auto data = manager.get();
			data->isNewGame = true;
			data->saveName.clear();
			directStart = true;
			DebugLog::print(U"[GameApp] direct start: new game");
			break;
		}

		if (args[i] == U"--load" && i + 1 < args.size())
		{
			auto data = manager.get();
			data->isNewGame   = false;
			data->saveName    = args[i + 1];
			data->sandboxMode = true;
			directStart = true;
			DebugLog::print(U"[GameApp] direct start: load '{}'"_fmt(data->saveName));
			break;
		}
	}

	// 初期シーン確定後は SceneManager の更新ループだけを回し、各シーンへ処理を委譲する。
	manager.init(directStart ? SceneState::Game : SceneState::Title, 0s);

	while (System::Update())
	{
		if (!manager.update())
			break;
	}

	DebugLog::print(U"[GameApp] stopped");
	DebugLog::shutdown();
}
