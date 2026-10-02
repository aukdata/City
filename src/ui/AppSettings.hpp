#pragma once
#include <Siv3D.hpp>
#include <filesystem>
#include <fstream>
#if SIV3D_PLATFORM(WINDOWS)
#include <Siv3D/Windows/Windows.hpp>
#endif
#include "../render/RenderDistance.hpp"

/// @brief 起動時に復元する表示・効果音設定。壊れた設定は既定値へ戻す。
struct AppSettings
{
	static constexpr double kDefaultEffectVolume = .6;
	bool lowSpec = false; ///< 低負荷描画を使用する。
	double renderDistance = RenderDistance::kDefault; ///< 0 または追加描画距離 [m]。
	double effectVolume = kDefaultEffectVolume; ///< 効果音の音量 [0, 1]。

	/// @brief 非有限値や UI・描画で扱えない範囲を拒否する。
	[[nodiscard]] bool valid() const
	{
		return RenderDistance::valid(renderDistance)
			&& IsFinite(effectVolume) && InRange(effectVolume, 0.0, 1.0);
	}

	/// @brief 全項目を検証し、欠損・破損・範囲外の場合は全設定の既定値を返す。
	[[nodiscard]] static AppSettings load(FilePathView path = U"settings.json")
	{
		try
		{
			const JSON json = JSON::Load(path);
			if (!json || !json.isObject() || !json.contains(U"lowSpec")
				|| !json.contains(U"renderDistance") || !json.contains(U"effectVolume")
				|| !json[U"lowSpec"].isBool() || !json[U"renderDistance"].isNumber()
				|| !json[U"effectVolume"].isNumber())
			{
				return {};
			}
			const auto lowSpecValue = json[U"lowSpec"].getOpt<bool>();
			const auto distanceValue = json[U"renderDistance"].getOpt<double>();
			const auto volumeValue = json[U"effectVolume"].getOpt<double>();
			if (!lowSpecValue || !distanceValue || !volumeValue)
			{
				return {};
			}
			const AppSettings result{*lowSpecValue, *distanceValue, *volumeValue};
			return result.valid() ? result : AppSettings{};
		}
		catch (...)
		{
			return {};
		}
	}

	/// @brief 一時ファイルの書込み成功後だけ原子的に置換し、失敗時は旧設定を保持する。
	/// @details 同一親ディレクトリの .pending を排他的に確保する。残存時は上書きしない。
	[[nodiscard]] bool save(FilePathView path = U"settings.json") const
	{
		if (!valid() || path.empty())
		{
			return false;
		}
		try
		{
			const std::string utf8Path = String{path}.toUTF8();
			const std::filesystem::path finalPath{std::u8string{utf8Path.begin(), utf8Path.end()}};
			std::filesystem::path pendingPath = finalPath;
			pendingPath += ".pending";
			std::error_code error;
			if (!std::filesystem::create_directory(pendingPath, error) || error)
			{
				return false;
			}
			// 自分が確保した作業ディレクトリだけを片付ける。
			struct PendingCleanup
			{
				const std::filesystem::path& path;
				~PendingCleanup()
				{
					std::error_code cleanupError;
					std::filesystem::remove_all(path, cleanupError);
				}
			} cleanup{pendingPath};
			const std::filesystem::path temporaryPath = pendingPath / "settings.json";
			JSON json;
			json[U"lowSpec"] = lowSpec;
			json[U"renderDistance"] = renderDistance;
			json[U"effectVolume"] = effectVolume;
			const std::string content = json.format().toUTF8();
			std::ofstream stream{temporaryPath, std::ios::binary | std::ios::trunc};
			if (!stream)
			{
				return false;
			}
			stream.write(content.data(), static_cast<std::streamsize>(content.size()));
			stream.flush();
			if (!stream)
			{
				return false;
			}
			stream.close();
			if (stream.fail())
			{
				return false;
			}
#if SIV3D_PLATFORM(WINDOWS)
			// Windows の filesystem::rename は既存ファイルを置換できない。
			return ::MoveFileExW(temporaryPath.c_str(), finalPath.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
			std::filesystem::rename(temporaryPath, finalPath, error);
			return !error;
#endif
		}
		catch (...)
		{
			return false;
		}
	}
};
