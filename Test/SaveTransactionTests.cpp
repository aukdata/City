#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/save/SaveTransaction.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	constexpr uint32 kTestMagic = 0x54565343u;
	constexpr uint16 kCurrentVersion = 1;

	struct TestHeader
	{
		uint32 magic = kTestMagic;
		uint16 version = kCurrentVersion;
		uint32 payloadSize = 0;
		uint32 checksum = 0;
	};

	std::filesystem::path toNativePath(FilePathView path)
	{
		return std::filesystem::path{ String{ path }.toWstr() };
	}

	FilePath toFilePath(const std::filesystem::path& path)
	{
		return Unicode::FromWstring(path.wstring());
	}

	uint32 checksum(const std::string& data)
	{
		constexpr uint32 kOffsetBasis = 2166136261u;
		constexpr uint32 kPrime = 16777619u;
		uint32 value = kOffsetBasis;
		for (const unsigned char byte : data)
		{
			value ^= byte;
			value *= kPrime;
		}
		return value;
	}

	SaveResult writeFixture(FilePathView directory, const std::string& payload,
		uint16 version = kCurrentVersion)
	{
		const std::filesystem::path filePath =
			toNativePath(directory) / L"global" / L"fixture.bin";
		std::error_code error;
		std::filesystem::create_directories(filePath.parent_path(), error);
		if (error)
		{
			return SaveResult::failed(SaveError::WriteFailed,
				U"テスト用ディレクトリを作成できません", directory);
		}

		std::ofstream stream{ filePath, std::ios::binary | std::ios::trunc };
		if (!stream)
		{
			return SaveResult::failed(SaveError::WriteFailed,
				U"テスト用ファイルを作成できません", directory);
		}
		const TestHeader header{
			kTestMagic, version, static_cast<uint32>(payload.size()), checksum(payload)
		};
		stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
		stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
		if (!stream)
		{
			return SaveResult::failed(SaveError::WriteFailed,
				U"テスト用ファイルを書き込めません", directory);
		}
		return SaveResult::succeeded(directory);
	}

	SaveResult readFixture(FilePathView directory, std::string& payload)
	{
		const std::filesystem::path filePath =
			toNativePath(directory) / L"global" / L"fixture.bin";
		std::ifstream stream{ filePath, std::ios::binary };
		if (!stream)
		{
			return SaveResult::failed(SaveError::MissingData, U"fixture.binがありません", directory);
		}

		TestHeader header;
		stream.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!stream || header.magic != kTestMagic)
		{
			return SaveResult::failed(SaveError::CorruptData, U"ヘッダーが破損しています", directory);
		}
		if (header.version != kCurrentVersion)
		{
			return SaveResult::failed(SaveError::UnsupportedVersion, U"未対応バージョンです", directory);
		}

		constexpr uint32 kMaximumPayloadSize = 1024 * 1024;
		if (header.payloadSize > kMaximumPayloadSize)
		{
			return SaveResult::failed(SaveError::CorruptData,
				U"payloadSizeが上限を超えています", directory);
		}
		payload.assign(header.payloadSize, '\0');
		stream.read(payload.data(), static_cast<std::streamsize>(payload.size()));
		if (!stream || checksum(payload) != header.checksum)
		{
			return SaveResult::failed(SaveError::CorruptData,
				U"ペイロードが破損しています", directory);
		}
		return SaveResult::succeeded(directory);
	}

	SaveResult verifyFixture(FilePathView directory)
	{
		std::string payload;
		return readFixture(directory, payload);
	}

	std::filesystem::path createCaseRoot(StringView caseName)
	{
		const std::filesystem::path root = std::filesystem::current_path()
			/ L"TestArtifacts" / String{ caseName }.toWstr();
		std::error_code error;
		std::filesystem::remove_all(root, error);
		error.clear();
		std::filesystem::create_directories(root, error);
		return root;
	}
}

void registerSaveTransactionTests(TestRunner& runner)
{
	runner.add(U"SaveTransaction.RoundTrip", [](TestContext& context)
	{
		const std::filesystem::path root = createCaseRoot(U"round_trip");
		const FilePath slot = toFilePath(root / L"slot");
		const std::string expected = "city-save-round-trip";
		const SaveResult result = SaveTransaction::commit(slot,
			[&](const FilePath& temporaryDirectory)
			{
				return writeFixture(temporaryDirectory, expected);
			}, verifyFixture);
		context.expect(static_cast<bool>(result), U"正常な一時セーブはコミットできます: error={} {} path={}"_fmt(static_cast<int>(result.error),result.message,result.path));

		std::string actual;
		const SaveResult loadResult = readFixture(slot, actual);
		context.expect(static_cast<bool>(loadResult), U"コミット後のセーブを読み戻せます: {}"_fmt(loadResult.message));
		context.expect(actual == expected, U"読み戻したペイロードが一致します");
	});

	runner.add(U"SaveTransaction.UnsupportedVersionRejected", [](TestContext& context)
	{
		const std::filesystem::path root = createCaseRoot(U"unsupported_version");
		const FilePath slot = toFilePath(root / L"slot");
		constexpr uint16 kFutureVersion = kCurrentVersion + 1;
		const SaveResult result = SaveTransaction::commit(slot,
			[](const FilePath& temporaryDirectory)
			{
				return writeFixture(temporaryDirectory, "future", kFutureVersion);
			}, verifyFixture);
		context.expect(!result, U"未対応バージョンはコミットされません");
		context.expectEqual(static_cast<int64>(result.error),
			static_cast<int64>(SaveError::UnsupportedVersion), U"失敗理由を区別できます");
		context.expect(!std::filesystem::exists(toNativePath(slot)), U"不正な新規セーブを残しません");
	});

	runner.add(U"SaveTransaction.CorruptionRejected", [](TestContext& context)
	{
		const std::filesystem::path root = createCaseRoot(U"corruption");
		const FilePath slot = toFilePath(root / L"slot");
		const SaveResult result = SaveTransaction::commit(slot,
			[](const FilePath& temporaryDirectory)
			{
				const SaveResult written = writeFixture(temporaryDirectory, "valid-before-corruption");
				if (!written)
				{
					return written;
				}
				const std::filesystem::path filePath =
					toNativePath(temporaryDirectory) / L"global" / L"fixture.bin";
				std::ofstream stream{ filePath, std::ios::binary | std::ios::trunc };
				stream.write("bad", 3);
				return stream ? SaveResult::succeeded(temporaryDirectory)
					: SaveResult::failed(SaveError::WriteFailed,
						U"破損データを書けません", temporaryDirectory);
			}, verifyFixture);
		context.expect(!result, U"破損データはコミットされません");
		context.expectEqual(static_cast<int64>(result.error),
			static_cast<int64>(SaveError::CorruptData), U"破損として報告されます");
	});

	runner.add(U"SaveTransaction.WriterFailurePreservesExistingSave", [](TestContext& context)
	{
		const std::filesystem::path root = createCaseRoot(U"writer_failure");
		const FilePath slot = toFilePath(root / L"slot");
		const SaveResult initial = writeFixture(slot, "existing");
		context.expect(static_cast<bool>(initial), U"既存セーブの準備に成功します");
		const SaveResult result = SaveTransaction::commit(slot,
			[](const FilePath& temporaryDirectory)
			{
				writeFixture(temporaryDirectory, "partial");
				return SaveResult::failed(SaveError::WriteFailed,
					U"injected failure", temporaryDirectory);
			}, verifyFixture);
		context.expect(!result, U"書き込み失敗は呼び出し側へ返ります");

		std::string actual;
		const SaveResult loadResult = readFixture(slot, actual);
		context.expect(static_cast<bool>(loadResult), U"既存セーブは読み込めます");
		context.expect(actual == "existing", U"既存セーブの内容を保持します");
	});

	runner.add(U"SaveTransaction.VerificationFailurePreservesExistingSave", [](TestContext& context)
	{
		const std::filesystem::path root = createCaseRoot(U"verification_failure");
		const FilePath slot = toFilePath(root / L"slot");
		writeFixture(slot, "existing");
		const SaveResult result = SaveTransaction::commit(slot,
			[](const FilePath& temporaryDirectory)
			{
				return writeFixture(temporaryDirectory, "replacement");
			},
			[](const FilePath& temporaryDirectory)
			{
				return SaveResult::failed(SaveError::VerificationFailed,
					U"injected failure", temporaryDirectory);
			});
		context.expect(!result, U"検証失敗は呼び出し側へ返ります");

		std::string actual;
		readFixture(slot, actual);
		context.expect(actual == "existing", U"検証失敗時は既存セーブを保持します");
	});
}
