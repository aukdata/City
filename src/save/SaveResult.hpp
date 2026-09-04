#pragma once
#include <Siv3D.hpp>

/// @brief セーブ・ロード処理の失敗理由
enum class SaveError : uint8
{
	None,
	InvalidPath,
	TemporaryDirectoryCreationFailed,
	WriteFailed,
	VerificationFailed,
	UnsupportedVersion,
	CorruptData,
	MissingData,
	BackupFailed,
	CommitFailed,
	RollbackFailed,
};

/// @brief セーブ・ロード処理の詳細な結果
struct SaveResult
{
	bool success = false;
	SaveError error = SaveError::WriteFailed;
	String message;
	FilePath path;

	[[nodiscard]] explicit operator bool() const
	{
		return success;
	}

	[[nodiscard]] static SaveResult succeeded(FilePathView resultPath = U"")
	{
		return { true, SaveError::None, U"", FilePath{ resultPath } };
	}

	[[nodiscard]] static SaveResult failed(
		SaveError reason, StringView detail, FilePathView failedPath = U"")
	{
		return { false, reason, String{ detail }, FilePath{ failedPath } };
	}
};
