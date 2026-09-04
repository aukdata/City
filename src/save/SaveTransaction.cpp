#include "SaveTransaction.hpp"
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <system_error>

namespace
{
	std::filesystem::path toNativePath(FilePathView path)
	{
		return std::filesystem::path{ String{ path }.toWstr() };
	}

	FilePath toFilePath(const std::filesystem::path& path)
	{
		return Unicode::FromWstring(path.wstring());
	}

	String errorMessage(StringView operation, const std::error_code& error)
	{
		return U"{}: {}"_fmt(operation, Unicode::FromUTF8(error.message()));
	}

	uint64 nextTransactionId()
	{
		static std::atomic<uint64> sequence{ 0 };
		const uint64 clockValue = static_cast<uint64>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		return clockValue ^ sequence.fetch_add(1, std::memory_order_relaxed);
	}

	SaveResult verifyContainsFile(const FilePath& directory)
	{
		std::error_code error;
		const std::filesystem::path nativeDirectory = toNativePath(directory);
		std::filesystem::recursive_directory_iterator iterator{ nativeDirectory, error };
		if (error)
		{
			return SaveResult::failed(SaveError::VerificationFailed,
				errorMessage(U"一時セーブを走査できません", error), directory);
		}

		const std::filesystem::recursive_directory_iterator end;
		for (; iterator != end; iterator.increment(error))
		{
			if (error)
			{
				return SaveResult::failed(SaveError::VerificationFailed,
					errorMessage(U"一時セーブを走査できません", error), directory);
			}
			if (iterator->is_regular_file(error) && !error)
			{
				return SaveResult::succeeded(directory);
			}
		}

		return SaveResult::failed(SaveError::MissingData,
			U"一時セーブにファイルがありません", directory);
	}

	bool removeDirectory(const std::filesystem::path& directory)
	{
		std::error_code error;
		std::filesystem::remove_all(directory, error);
		return !error;
	}
}

SaveResult SaveTransaction::commit(FilePathView finalDirectory,
	const WriteCallback& writer, const VerifyCallback& verifier)
{
	if (finalDirectory.isEmpty() || !writer)
	{
		return SaveResult::failed(SaveError::InvalidPath,
			U"保存先または書き込み処理が指定されていません", finalDirectory);
	}

	const std::filesystem::path finalPath = toNativePath(finalDirectory).lexically_normal();
	const std::filesystem::path parentPath = finalPath.parent_path();
	const std::filesystem::path fileName = finalPath.filename();
	if (parentPath.empty() || fileName.empty())
	{
		return SaveResult::failed(SaveError::InvalidPath,
			U"保存先には親ディレクトリと保存名が必要です", finalDirectory);
	}

	const std::wstring suffix = std::to_wstring(nextTransactionId());
	const std::filesystem::path temporaryPath =
		parentPath / (fileName.wstring() + L".tmp_" + suffix);
	const std::filesystem::path backupPath =
		parentPath / (fileName.wstring() + L".backup_" + suffix);

	std::error_code error;
	std::filesystem::create_directories(parentPath, error);
	if (error)
	{
		return SaveResult::failed(SaveError::TemporaryDirectoryCreationFailed,
			errorMessage(U"保存先の親ディレクトリを作成できません", error), toFilePath(parentPath));
	}
	if (!std::filesystem::create_directory(temporaryPath, error) || error)
	{
		return SaveResult::failed(SaveError::TemporaryDirectoryCreationFailed,
			errorMessage(U"一時セーブディレクトリを作成できません", error), toFilePath(temporaryPath));
	}

	const FilePath temporaryDirectory = toFilePath(temporaryPath);
	SaveResult writeResult;
	try
	{
		writeResult = writer(temporaryDirectory);
	}
	catch (const std::exception& exception)
	{
		removeDirectory(temporaryPath);
		return SaveResult::failed(SaveError::WriteFailed,
			U"セーブ書き込み処理で例外が発生しました: {}"_fmt(
				Unicode::FromUTF8(exception.what())), temporaryDirectory);
	}
	catch (...)
	{
		removeDirectory(temporaryPath);
		return SaveResult::failed(SaveError::WriteFailed,
			U"セーブ書き込み処理で不明な例外が発生しました", temporaryDirectory);
	}
	if (!writeResult)
	{
		removeDirectory(temporaryPath);
		return writeResult;
	}

	SaveResult verifyResult;
	try
	{
		verifyResult = verifier
			? verifier(temporaryDirectory)
			: verifyContainsFile(temporaryDirectory);
	}
	catch (const std::exception& exception)
	{
		removeDirectory(temporaryPath);
		return SaveResult::failed(SaveError::VerificationFailed,
			U"セーブ検証処理で例外が発生しました: {}"_fmt(
				Unicode::FromUTF8(exception.what())), temporaryDirectory);
	}
	catch (...)
	{
		removeDirectory(temporaryPath);
		return SaveResult::failed(SaveError::VerificationFailed,
			U"セーブ検証処理で不明な例外が発生しました", temporaryDirectory);
	}
	if (!verifyResult)
	{
		removeDirectory(temporaryPath);
		return verifyResult;
	}

	const bool hadExistingSave = std::filesystem::exists(finalPath, error);
	if (error)
	{
		removeDirectory(temporaryPath);
		return SaveResult::failed(SaveError::BackupFailed,
			errorMessage(U"既存セーブを確認できません", error), toFilePath(finalPath));
	}

	if (hadExistingSave)
	{
		std::filesystem::rename(finalPath, backupPath, error);
		if (error)
		{
			removeDirectory(temporaryPath);
			return SaveResult::failed(SaveError::BackupFailed,
				errorMessage(U"既存セーブをバックアップできません", error), toFilePath(finalPath));
		}
	}

	std::filesystem::rename(temporaryPath, finalPath, error);
	if (error)
	{
		const String commitError = errorMessage(U"一時セーブを正式保存先へ移動できません", error);
		if (hadExistingSave)
		{
			error.clear();
			std::filesystem::rename(backupPath, finalPath, error);
			if (error)
			{
				return SaveResult::failed(SaveError::RollbackFailed,
					U"{} / {}"_fmt(commitError,
						errorMessage(U"既存セーブの復元にも失敗しました", error)), toFilePath(finalPath));
			}
		}
		removeDirectory(temporaryPath);
		return SaveResult::failed(SaveError::CommitFailed, commitError, toFilePath(finalPath));
	}

	if (hadExistingSave)
	{
		removeDirectory(backupPath);
	}
	return SaveResult::succeeded(toFilePath(finalPath));
}
