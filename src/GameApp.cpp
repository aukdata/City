#include "GameApp.hpp"
#include "GameLaunchOptions.hpp"
#include "ui/KeyboardActions.hpp"
#include "ui/AppSettings.hpp"
#include "asset/AssetRegistrar.hpp"
#include "debug/DebugLog.hpp"

namespace
{
	/// @brief Split framework presentation from input/update work for frame pacing diagnostics.
	class FramePipelineProbe final : public IAddon
	{
	public:
		void draw() const override { m_timer.restart(); }
		void postPresent() override
		{
			m_present << m_timer.msF();
			m_timer.restart();
		}
		bool update() override
		{
			if (!m_present.isEmpty()) { m_input << m_timer.msF(); }
			if (m_input.size() >= 120)
			{
				m_present.sort();
				m_input.sort();
				DebugLog::print(U"[FramePipeline] presentP50={:.2f} inputP50={:.2f}"_fmt(m_present[m_present.size() / 2], m_input[60]));
				m_present.clear();
				m_input.clear();
			}
			return true;
		}
	private:
		mutable Stopwatch m_timer;
		Array<double> m_present, m_input;
	};
}

void GameApp::run()
{
	// 起動時にウィンドウ・描画設定・共有アセットを初期化し、その後 SceneManager へ制御を渡す。
	constexpr int kWindowWidth  = 1280;
	constexpr int kWindowHeight = 768;

	System::SetTerminationTriggers(UserAction::CloseButtonClicked);
	Window::Resize(kWindowWidth, kWindowHeight);
	Scene::SetBackground(ColorF{ 0.2, 0.3, 0.4 });
	Graphics::SetVSyncEnabled(true);
	Window::SetTitle(U"Pavecity");

	RegisterAssets();
	DebugLog::initialize(U"debug.log");
	DebugLog::print(U"[GameApp] started");

	App manager;
	const auto preferences = AppSettings::load();
	manager.get()->lowSpec = preferences.lowSpec;
	manager.get()->renderDistance = preferences.renderDistance;
	manager.get()->effectVolume = preferences.effectVolume;
	manager.add<TitleScene>(SceneState::Title);
	manager.add<GameScene>(SceneState::Game);

	// コマンドライン指定があればタイトル画面を介さず、開始条件だけ SceneData へ直接流し込む。
	const auto args = System::GetCommandLineArgs();
	const auto options = GameLaunch::parseCommandLine(args, *manager.get());
	bool directStart = options.directStart;
	const bool captureCityRenders = options.captureCityRenders;
	const bool captureRoadRenders = options.captureRoadRenders;
	const bool uncapped = options.uncapped;
	const auto seedOverride = options.seedOverride;

	if (captureCityRenders)
	{
		Addon::Register<FramePipelineProbe>(U"FramePipelineProbe");
		Window::Resize(1920, 1080);
		auto data = manager.get();
		if (data->saveName.isEmpty()) { data->isNewGame = true; }
		data->sandboxMode = false;
		data->captureCityRenders = true;
		data->captureRoadRenders = captureRoadRenders;
		if (seedOverride)
		{
			data->seed = *seedOverride;
		}
		directStart = true;
		DebugLog::print(U"[GameApp] capture-city mode enabled");
	}
	else if (seedOverride)
	{
		auto data = manager.get();
		data->seed = *seedOverride;
	}

	Graphics::SetVSyncEnabled(!uncapped);

	// 初期シーン確定後は SceneManager の更新ループだけを回し、各シーンへ処理を委譲する。
	manager.init(directStart ? SceneState::Game : SceneState::Title, 0s);

	Array<double> systemTimes;
	while (true)
	{
		const Stopwatch systemTimer{ StartImmediately::Yes };
		if (!System::Update()) { break; }
		GameInput::beginFrame();
		if (captureCityRenders) { systemTimes << systemTimer.msF(); }
		if (!manager.update()) { break; }
		if (systemTimes.size() >= 120)
		{
			systemTimes.sort();
			const auto& state = Window::GetState();
			DebugLog::print(U"[PresentTiming] systemP50={:.2f} systemP95={:.2f} focused={} minimized={} vsync={}"_fmt(
				systemTimes[60], systemTimes[114], state.focused, state.minimized, Graphics::IsVSyncEnabled()));
			systemTimes.clear();
		}
	}

	DebugLog::print(U"[GameApp] stopped");
	DebugLog::shutdown();
}
