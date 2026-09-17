#include "SaveCatalog.hpp"
#include <filesystem>

namespace SaveCatalog
{
	namespace
	{
		bool validName(StringView name)
		{
			if (name.empty() || name == U"." || name == U".." || name.back() == U'.' || name.back() == U' ')
			{
				return false;
			}
			for (char32 character : name)
			{
				if (character < 32 || StringView{U"/\\:*?\"<>|"}.contains(character))
				{
					return false;
				}
			}
			return true;
		}
	} // namespace
	Array<String> list(FilePathView root)
	{
		Array<String> result;
		std::error_code error;
		const std::filesystem::path native{String{root}.toWstr()};
		const std::filesystem::directory_iterator end;
		for (std::filesystem::directory_iterator it{native, error}; !error && it != end; it.increment(error))
		{
			const auto name = Unicode::FromWstring(it->path().filename().wstring());
			if (validName(name) && it->is_directory(error) && !it->is_symlink(error) &&
				std::filesystem::is_regular_file(it->path() / L"meta.json", error))
			{
				result << name;
			}
		}
		result.sort();
		return result;
	}
	SaveResult remove(FilePathView root, StringView name)
	{
		if (!validName(name))
		{
			return SaveResult::failed(SaveError::InvalidPath, U"保存名が不正です");
		}
		std::error_code error;
		const auto directory = std::filesystem::canonical(std::filesystem::path{String{root}.toWstr()}, error);
		if (error)
		{
			return SaveResult::failed(SaveError::InvalidPath, U"保存フォルダーがありません");
		}
		const auto requested = directory / String{name}.toWstr();
		const auto target = std::filesystem::canonical(requested, error);
		if (error || target.parent_path() != directory || std::filesystem::is_symlink(requested, error) ||
			!std::filesystem::is_regular_file(target / L"meta.json", error))
		{
			return SaveResult::failed(SaveError::InvalidPath, U"保存フォルダー直下のセーブだけを削除できます");
		}
		// 間接参照を含む保存は削除しない。ユーザーの別フォルダーへ処理を広げない。
		const std::filesystem::recursive_directory_iterator end;
		for (std::filesystem::recursive_directory_iterator it{target, error}; !error && it != end; it.increment(error))
		{
			if (it->is_symlink(error))
			{
				return SaveResult::failed(SaveError::InvalidPath, U"リンクを含むセーブは削除できません");
			}
		}
		if (error)
		{
			return SaveResult::failed(SaveError::InvalidPath, U"セーブを確認できません");
		}
		std::filesystem::remove_all(target, error);
		if (error)
		{
			return SaveResult::failed(
				SaveError::WriteFailed, U"削除できませんでした: " + Unicode::FromUTF8(error.message()));
		}
		return SaveResult::succeeded(Unicode::FromWstring(target.wstring()));
	}
} // namespace SaveCatalog
