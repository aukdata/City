#pragma once

/// @brief オンスクリーンデバッグログ（静的クラス）
/// @details DebugRenderer がデバッグモード中に画面右下へ表示する
class DebugLog
{
public:
	struct Entry
	{
		String text;
		double time = 0.0;  ///< Scene::Time() でのタイムスタンプ
	};

	static constexpr int    MAX_LINES     = 20;
	static constexpr double FADE_DURATION = 3.0;

	/// @brief 1 行ログに追加する
	static void print(const String& text)
	{
		auto& e = entries();
		e << Entry{ text, Scene::Time() };
		while (static_cast<int>(e.size()) > MAX_LINES)
			e.pop_front();
	}

	/// @brief ログを全消去する
	static void clear() { entries().clear(); }

	/// @brief エントリリストを返す（DebugRenderer から参照する）
	static Array<Entry>& entries()
	{
		static Array<Entry> s_entries;
		return s_entries;
	}
};
