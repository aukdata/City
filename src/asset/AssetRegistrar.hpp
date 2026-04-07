#pragma once
#include <Siv3D.hpp>

/// @brief アセット名定数
namespace Asset
{
	// ---- テクスチャ ----
	constexpr StringView Grass = U"Tex_Grass";

	// ---- フォント ----
	constexpr StringView TitleBold48   = U"Font_TitleBold48";
	constexpr StringView UI24          = U"Font_UI24";
	constexpr StringView UI20          = U"Font_UI20";
	constexpr StringView Small16       = U"Font_Small16";
	constexpr StringView Panel14       = U"Font_Panel14";
	constexpr StringView PanelBold14   = U"Font_PanelBold14";
	constexpr StringView CJK24         = U"Font_CJK24";
	constexpr StringView CJK14         = U"Font_CJK14";

	// TitleScene 用（非 MSDF）
	constexpr StringView TitleBold46   = U"Font_TitleBold46";
	constexpr StringView Sub16         = U"Font_Sub16";
	constexpr StringView Label17       = U"Font_Label17";
	constexpr StringView List15        = U"Font_List15";
}

/// @brief TextureAsset / FontAsset を一括登録する（Main 起動直後に1回呼ぶ）
void RegisterAssets();
