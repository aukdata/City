#pragma once
#include <Siv3D.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

/// @brief スレッドセーフな診断ログ。画面用の履歴と UTF-8 ファイルを同時に更新する。
class DebugLog
{
public:
	struct Entry
	{
		String text;
		double time = 0.0; ///< Scene::Time() と同じ原点の経過秒。
	};

	static constexpr int MAX_LINES = 20;
	static constexpr double FADE_DURATION = 3.0;

	/// @brief ログを開き、画面履歴と時計を新しいセッションへ切り替える。
	static void initialize(const FilePath& path = U"debug.log", bool append = false)
	{
		auto& state = storage();
		std::lock_guard lock(state.mutex);
		state.output.close();
		state.output.clear();
		state.entries.clear();
		state.startedAt = Clock::now();
		state.sceneOrigin = Scene::Time();
		state.output.open(std::filesystem::path{path.toWstr()},
			std::ios::out | (append ? std::ios::app : std::ios::trunc));
		if (!state.output) { return; }
		state.output.seekp(0, std::ios::end);
		if (state.output.tellp() == std::streampos{0}) { state.output << "\xEF\xBB\xBF"; }
		state.output << "===== DebugLog started "
			<< Unicode::ToUTF8(DateTime::Now().format(U"yyyy-MM-dd HH:mm:ss")) << " =====\n";
		state.output.flush();
	}

	/// @brief ログを閉じる。シーンとワーカの終了後に呼ぶ。
	static void shutdown()
	{
		auto& state = storage();
		std::lock_guard lock(state.mutex);
		if (!state.output.is_open()) { return; }
		state.output << "===== DebugLog closed "
			<< Unicode::ToUTF8(DateTime::Now().format(U"yyyy-MM-dd HH:mm:ss")) << " =====\n";
		state.output.close();
	}

	/// @brief 1行を追加する。ワーカは Siv3D の可変時計へアクセスしない。
	static void print(const String& text)
	{
		auto& state = storage();
		std::lock_guard lock(state.mutex);
		const double time = state.sceneOrigin + std::chrono::duration<double>(Clock::now() - state.startedAt).count();
		state.entries << Entry{text, time};
		while (state.entries.size() > static_cast<size_t>(MAX_LINES)) { state.entries.pop_front(); }
		if (state.output.is_open())
		{
			state.output << '[' << time << "s] " << Unicode::ToUTF8(text) << '\n';
			state.output.flush();
		}
	}

	/// @brief 画面履歴を消去する。ファイルの記録は保持する。
	static void clear()
	{
		auto& state = storage();
		std::lock_guard lock(state.mutex);
		state.entries.clear();
	}

	/// @brief 描画中に変更されない履歴のコピーを返す。
	static Array<Entry> entries()
	{
		auto& state = storage();
		std::lock_guard lock(state.mutex);
		return state.entries;
	}

private:
	using Clock = std::chrono::steady_clock;
	struct Storage
	{
		std::mutex mutex;
		std::ofstream output;
		Array<Entry> entries;
		Clock::time_point startedAt = Clock::now();
		double sceneOrigin = 0.0;
	};

	static Storage& storage()
	{
		static Storage state;
		return state;
	}
};

#ifndef DBG_LOG
# define DBG_LOG(message) DebugLog::print(message)
#endif
