#pragma once
#include <Siv3D.hpp>

/// @brief 起動中は不変の生成設定。値の正本は assets/generation/*.json。
namespace GenerationSettings
{
	struct Values
	{
#define GENERATION_SETTING(type, field, group, key, minimum, maximum) type field{};
#include "GenerationSettings.def"
#undef GENERATION_SETTING
	};
	/// @brief 必須キー・型・範囲・未知キーを検査して読み込む。失敗は項目名付き例外。
	Values load(FilePathView directory);
	/// @brief 初回のみロードする共有スナップショット。サンプルごとのファイル読込はしない。
	const Values& get();
}
