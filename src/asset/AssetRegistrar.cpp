#include "AssetRegistrar.hpp"

void RegisterAssets()
{
	// 起動時に共有アセットを一括登録し、各描画系が名前参照だけで再利用できる状態にする。
	// ---- テクスチャ ----
	TextureAsset::Register(Asset::Grass,       U"assets/terrain/grass.png",  TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::Ground,      U"assets/terrain/ground.jpg", TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::SparseGrass, U"assets/third_party/polyhaven/sparse_grass/sparse_grass_diff_1k.jpg", TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::Sand,        U"assets/third_party/polyhaven/sand_01/sand_01_diff_1k.jpg", TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::CoastSand,   U"assets/third_party/polyhaven/coast_sand_01/coast_sand_01_diff_1k.jpg", TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::Asphalt, U"assets/third_party/polyhaven/asphalt_floor/asphalt_floor_diff_1k.jpg", TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::Concrete,    U"assets/third_party/polyhaven/concrete/concrete_diff_1k.jpg", TextureDesc::MippedSRGB);
	// NationalRoadSign は SRGB RenderTexture 上で案内標識の背景（Rect::draw でクリア色塗り）と
	// 合成される。PNG 物理バイト (21,87,161) を linear として扱う Mipped（非 SRGB）にすることで、
	// 背景（Rect.draw）と PNG が同じ明るめの青で 3D 表示されるようにする
	TextureAsset::Register(Asset::NationalRoadSign, U"assets/signs/guide/national_route.png", TextureDesc::Mipped);
	TextureAsset::Register(Asset::StopSign,         U"assets/signs/regulatory/stop.png",      TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::YieldSign,        U"assets/signs/regulatory/yield.png",     TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::NoEntrySign,      U"assets/signs/regulatory/no_entry.png",  TextureDesc::MippedSRGB);
	TextureAsset::Register(Asset::DirectionalRestrictionSign,
	                       U"assets/signs/regulatory/directional_restriction.png",            TextureDesc::MippedSRGB);

	// ---- フォント（MSDF）----
	FontAsset::Register(Asset::TitleBold48, FontMethod::MSDF, 48, Typeface::Bold);
	FontAsset::Register(Asset::UI24,        FontMethod::MSDF, 24);
	FontAsset::Register(Asset::UI20,        FontMethod::MSDF, 20);
	FontAsset::Register(Asset::Small16,     FontMethod::MSDF, 16);
	FontAsset::Register(Asset::Panel14,     FontMethod::MSDF, 14);
	FontAsset::Register(Asset::PanelBold14, FontMethod::MSDF, 14, Typeface::Bold);
	FontAsset::Register(Asset::CJK24,       FontMethod::MSDF, 24, Typeface::CJK_Regular_JP);
	FontAsset::Register(Asset::CJK14,       FontMethod::MSDF, 14, Typeface::CJK_Regular_JP);
	// 案内標識の地名（YuGothB.ttc は msdfgen クラッシュのため CJK_Regular_JP を使用）
	FontAsset::Register(Asset::CJK32Bold,   FontMethod::MSDF, 32, Typeface::CJK_Regular_JP);
	// Arial Bold（国道標識の号数表示用）— Windows 標準フォントを直接読み込む
	FontAsset::Register(Asset::Arial24,     FontMethod::MSDF, 24, U"C:/Windows/Fonts/arialbd.ttf");

	// ---- フォント（TitleScene 用・非 MSDF）----
	FontAsset::Register(Asset::TitleBold46, 46, Typeface::Bold);
	FontAsset::Register(Asset::Sub16,       16);
	FontAsset::Register(Asset::Label17,     17);
	FontAsset::Register(Asset::List15,      15);
}
