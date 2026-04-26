#pragma once
#include <fstream>

/// @brief デバッグログ（画面表示 + ファイル出力）
/// @details DebugRenderer がデバッグモード中に画面右下へ表示し、同時に App/debug.log へ出力する
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

	/// @brief ログファイルを開く
	/// @param path 実行時ワーキングディレクトリからの出力先
	/// @param append true の場合は追記、false の場合は起動ごとに上書き
	static void initialize(const FilePath& path = U"debug.log", bool append = false)
	{
		auto& output = file();
		if (output.is_open())
		{
			output.close();
		}

		const auto mode = append ? std::ios::app : std::ios::trunc;
		output.open(Unicode::ToUTF8(path), std::ios::out | mode);
		if (!output)
		{
			return;
		}

		output << "\xEF\xBB\xBF";
		output << "===== DebugLog started "
			   << Unicode::ToUTF8(DateTime::Now().format(U"yyyy-MM-dd HH:mm:ss"))
			   << " =====\n";
		output.flush();
	}

	/// @brief ログファイルを閉じる
	static void shutdown()
	{
		auto& output = file();
		if (output.is_open())
		{
			output << "===== DebugLog closed "
				   << Unicode::ToUTF8(DateTime::Now().format(U"yyyy-MM-dd HH:mm:ss"))
				   << " =====\n";
			output.close();
		}
	}

	/// @brief 1 行ログに追加する
	static void print(const String& text)
	{
		auto& e = entries();
		e << Entry{ text, Scene::Time() };
		while (static_cast<int>(e.size()) > MAX_LINES)
			e.pop_front();

		writeToFile(text);
	}

	/// @brief ログを全消去する
	static void clear() { entries().clear(); }

	/// @brief エントリリストを返す（DebugRenderer から参照する）
	static Array<Entry>& entries()
	{
		static Array<Entry> s_entries;
		return s_entries;
	}

private:
	static std::ofstream& file()
	{
		static std::ofstream s_file;
		return s_file;
	}

	static void writeToFile(const String& text)
	{
		auto& output = file();
		if (!output.is_open())
		{
			return;
		}

		output << '[' << Scene::Time() << "s] " << Unicode::ToUTF8(text) << '\n';
		output.flush();
	}
};

#ifndef DBG_LOG
# define DBG_LOG(message) DebugLog::print(message)
#endif
