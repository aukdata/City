#include "AssetRegistrar.hpp"

void RegisterAssets()
{
	// ---- テクスチャ ----
	TextureAsset::Register(Asset::Grass, U"assets/textures/grass.png", TextureDesc::MippedSRGB);

	// ---- フォント（MSDF）----
	FontAsset::Register(Asset::TitleBold48, FontMethod::MSDF, 48, Typeface::Bold);
	FontAsset::Register(Asset::UI24,        FontMethod::MSDF, 24);
	FontAsset::Register(Asset::UI20,        FontMethod::MSDF, 20);
	FontAsset::Register(Asset::Small16,     FontMethod::MSDF, 16);
	FontAsset::Register(Asset::Panel14,     FontMethod::MSDF, 14);
	FontAsset::Register(Asset::PanelBold14, FontMethod::MSDF, 14, Typeface::Bold);
	FontAsset::Register(Asset::CJK24,       FontMethod::MSDF, 24, Typeface::CJK_Regular_JP);
	FontAsset::Register(Asset::CJK14,       FontMethod::MSDF, 14, Typeface::CJK_Regular_JP);

	// ---- フォント（TitleScene 用・非 MSDF）----
	FontAsset::Register(Asset::TitleBold46, 46, Typeface::Bold);
	FontAsset::Register(Asset::Sub16,       16);
	FontAsset::Register(Asset::Label17,     17);
	FontAsset::Register(Asset::List15,      15);
}
