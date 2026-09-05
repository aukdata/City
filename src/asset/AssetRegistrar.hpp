#pragma once
#include <Siv3D.hpp>

/// @brief アセット名定数
namespace Asset
{
	// 描画系はこの名前定数だけを参照し、実ファイルパスや登録方法は RegisterAssets 側へ閉じ込める。
	// ---- テクスチャ ----
	constexpr StringView Grass            = U"Tex_Grass";
	constexpr StringView Ground           = U"Tex_Ground";
	constexpr StringView NationalRoadSign = U"Tex_NationalRoadSign";  ///< 案内・国道おにぎり（地）
	constexpr StringView StopSign         = U"Tex_StopSign";           ///< 規制・一時停止「止まれ」
	constexpr StringView YieldSign        = U"Tex_YieldSign";          ///< 規制・徐行
	constexpr StringView NoEntrySign      = U"Tex_NoEntrySign";        ///< 規制・車両進入禁止 (303)
	constexpr StringView DirectionalRestrictionSign = U"Tex_DirectionalRestrictionSign"; ///< 規制・指定方向外進行禁止 (311)

	// ---- フォント ----
	constexpr StringView TitleBold48   = U"Font_TitleBold48";
	constexpr StringView UI24          = U"Font_UI24";
	constexpr StringView UI20          = U"Font_UI20";
	constexpr StringView Small16       = U"Font_Small16";
	constexpr StringView Panel14       = U"Font_Panel14";
	constexpr StringView PanelBold14   = U"Font_PanelBold14";
	constexpr StringView CJK24         = U"Font_CJK24";
	constexpr StringView CJK14         = U"Font_CJK14";
	constexpr StringView CJK32Bold     = U"Font_CJK32Bold";  ///< 案内標識の地名用（太字）
	constexpr StringView Arial24       = U"Font_Arial24";  ///< 国道標識の号数用 (Arial Bold)

	// TitleScene 用（非 MSDF）
	constexpr StringView TitleBold46   = U"Font_TitleBold46";
	constexpr StringView Sub16         = U"Font_Sub16";
	constexpr StringView Label17       = U"Font_Label17";
	constexpr StringView List15        = U"Font_List15";
}

/// @brief TextureAsset / FontAsset を一括登録する（Main 起動直後に1回呼ぶ）
void RegisterAssets();
