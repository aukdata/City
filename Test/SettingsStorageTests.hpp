#pragma once
#include <Siv3D.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include "TestRunner.hpp"
#include "src/ui/AppSettings.hpp"

/// @brief 設定永続化の独立したファイルフィクスチャ。
namespace SettingsStorageTest
{
	inline FilePath casePath(StringView name)
	{
		const FilePath directory = U"TestArtifacts/settings_storage/" + String{name};
		FileSystem::Remove(directory);
		FileSystem::CreateDirectories(directory);
		return directory + U"/settings.json";
	}

	inline bool writeText(FilePathView path, const std::string& content)
	{
		const std::string utf8Path = String{path}.toUTF8();
		std::ofstream stream{std::filesystem::path{std::u8string{utf8Path.begin(), utf8Path.end()}}, std::ios::binary};
		stream << content;
		stream.close();
		return !stream.fail();
	}

	inline void expectDefaults(TestContext& context, const AppSettings& settings)
	{
		context.expect(settings.valid() && !settings.lowSpec, U"Defaults are valid and use normal rendering");
		context.expectNear(settings.renderDistance, RenderDistance::kDefault, 0, U"Default rendering distance is restored");
		context.expectNear(settings.effectVolume, AppSettings::kDefaultEffectVolume, 0, U"Default effects volume is restored");
	}
}

/// @brief 設定の復元・検証・原子的置換を常設テストへ追加する。
inline void registerSettingsStorageTests(TestRunner& runner)
{
	runner.add(U"SettingsStorage.DefaultsAndRoundTrip", [](TestContext& context)
	{
		const FilePath path = SettingsStorageTest::casePath(U"round_trip");
		SettingsStorageTest::expectDefaults(context, AppSettings::load(path));
		const AppSettings first{true, 1500, .25};
		context.expect(first.save(path), U"Non-default settings can be saved");
		const auto restored = AppSettings::load(path);
		context.expect(restored.lowSpec, U"Low-spec mode survives a restart");
		context.expectNear(restored.renderDistance, 1500, 0, U"Rendering distance survives a restart");
		context.expectNear(restored.effectVolume, .25, 0, U"Effects volume survives a restart");
		context.expect(AppSettings{}.save(path), U"A second save replaces the existing file");
		SettingsStorageTest::expectDefaults(context, AppSettings::load(path));
		context.expect(!FileSystem::Exists(path + U".pending"), U"Successful saves leave no temporary directory");
	});

	runner.add(U"SettingsStorage.CorruptAndInvalidFallback", [](TestContext& context)
	{
		const FilePath path = SettingsStorageTest::casePath(U"invalid");
		for (const std::string content : {
			"broken json", "[]", "{}",
			R"({"lowSpec":true,"renderDistance":99,"effectVolume":0.5})",
			R"({"lowSpec":true,"renderDistance":20001,"effectVolume":0.5})",
			R"({"lowSpec":true,"renderDistance":500,"effectVolume":-0.1})",
			R"({"lowSpec":true,"renderDistance":500,"effectVolume":1.1})",
			R"({"lowSpec":"true","renderDistance":500,"effectVolume":0.5})",
			R"({"lowSpec":true,"renderDistance":"500","effectVolume":0.5})",
			R"({"lowSpec":true,"renderDistance":500,"effectVolume":null})"})
		{
			context.expect(SettingsStorageTest::writeText(path, content), U"Invalid settings fixture is writable");
			SettingsStorageTest::expectDefaults(context, AppSettings::load(path));
		}
	});

	runner.add(U"SettingsStorage.ValidationAndFailedSavePreserveFile", [](TestContext& context)
	{
		const FilePath path = SettingsStorageTest::casePath(U"failure");
		const AppSettings original{true, 900, .75};
		context.expect(original.save(path), U"The original settings file exists");
		for (const double invalidDistance : {-1.0, 99.0, 20001.0,
			std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		{
			AppSettings invalid = original;
			invalid.renderDistance = invalidDistance;
			context.expect(!invalid.valid() && !invalid.save(path), U"Invalid distance is rejected before writing");
		}
		for (const double invalidVolume : {-0.1, 1.1,
			std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		{
			AppSettings invalid = original;
			invalid.effectVolume = invalidVolume;
			context.expect(!invalid.valid() && !invalid.save(path), U"Invalid volume is rejected before writing");
		}
		context.expect(FileSystem::CreateDirectories(path + U".pending"), U"A blocked staging directory is created");
		const FilePath marker = path + U".pending/owner.txt";
		context.expect(SettingsStorageTest::writeText(marker, "do not remove"), U"The other writer's marker exists");
		context.expect(!AppSettings{}.save(path), U"A blocked staging directory fails safely");
		context.expect(FileSystem::Exists(marker), U"Another writer's files are never removed");
		const auto restored = AppSettings::load(path);
		context.expect(restored.lowSpec, U"Failed saves preserve the old rendering mode");
		context.expectNear(restored.renderDistance, original.renderDistance, 0, U"Failed saves preserve old distance");
		context.expectNear(restored.effectVolume, original.effectVolume, 0, U"Failed saves preserve old volume");
		context.expect(!original.save(U""), U"An empty path is rejected");
		context.expect(!original.save(path + U"/missing/settings.json"), U"An unwritable parent fails safely");
	});

	runner.add(U"SettingsStorage.CommitFailureAndBoundaryValues", [](TestContext& context)
	{
		const FilePath path = SettingsStorageTest::casePath(U"commit_failure");
		context.expect(FileSystem::CreateDirectories(path), U"The target is deliberately a directory");
		const FilePath marker = path + U"/keep.txt";
		context.expect(SettingsStorageTest::writeText(marker, "keep"), U"The destination marker exists");
		context.expect(!AppSettings{}.save(path), U"File replacement refuses a directory destination");
		context.expect(FileSystem::Exists(marker), U"A failed commit preserves destination contents");
		context.expect(!FileSystem::Exists(path + U".pending"), U"Failed commits clean their own temporary files");
		const FilePath boundaryPath = SettingsStorageTest::casePath(U"boundaries");
		for (const double distance : {0.0, RenderDistance::kMinimum, RenderDistance::kMaximum})
		{
			for (const double volume : {0.0, 1.0})
			{
				const AppSettings settings{false, distance, volume};
				context.expect(settings.valid() && settings.save(boundaryPath), U"Boundary values remain valid and saveable");
				const auto restored = AppSettings::load(boundaryPath);
				context.expectNear(restored.renderDistance, distance, 0, U"Boundary distance round-trips exactly");
				context.expectNear(restored.effectVolume, volume, 0, U"Boundary volume round-trips exactly");
			}
		}
	});
}
