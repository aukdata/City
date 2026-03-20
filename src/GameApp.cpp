#include "GameApp.hpp"

void GameApp::run()
{
	constexpr int kWindowWidth  = 1280;
	constexpr int kWindowHeight = 768;

	System::SetTerminationTriggers(UserAction::CloseButtonClicked);
	Window::Resize(kWindowWidth, kWindowHeight);
	Scene::SetBackground(ColorF{ 0.2, 0.3, 0.4 });
	Window::SetTitle(U"City Simulation");

	App manager;
	manager.add<TitleScene>(SceneState::Title);
	manager.add<GameScene>(SceneState::Game);
	manager.init(SceneState::Title, 0s);

	while (System::Update())
	{
		if (!manager.update())
			break;
	}
}
