#pragma once
#include <Siv3D.hpp>
#include <functional>
#include "SaveResult.hpp"

/// @brief ディレクトリ単位の原子的なセーブ差し替え
/// @details 正式保存先と同じ親に一時ディレクトリを作り、検証後にリネームする。
class SaveTransaction
{
public:
	using WriteCallback = std::function<SaveResult(const FilePath& temporaryDirectory)>;
	using VerifyCallback = std::function<SaveResult(const FilePath& temporaryDirectory)>;

	/// @brief セーブデータを一時出力し、検証後に正式保存先へ反映する
	[[nodiscard]] static SaveResult commit(
		FilePathView finalDirectory,
		const WriteCallback& writer,
		const VerifyCallback& verifier = {});
};
