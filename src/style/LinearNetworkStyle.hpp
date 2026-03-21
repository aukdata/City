#pragma once

/// @brief 線描画スタイル（実線・破線を統一定義）
/// RoadStyle の区画線やセンターラインのほか、将来のフェンス・ガードレール等でも共用できる。
struct LineMarkStyle
{
	ColorF color      = ColorF{ 1.0, 1.0, 1.0 };
	float  lineWidth  = 0.15f;   ///< 線幅 [m]
	float  dashLength = 0.0f;    ///< 破線の実部長 [m]（0 以下 = 実線）
	float  gapLength  = 0.0f;    ///< 破線の間隔長 [m]

	/// @brief 実線かどうかを返す
	bool isSolid() const noexcept { return dashLength <= 0.0f || gapLength <= 0.0f; }
};

/// @brief 線状ネットワークオブジェクト共通の路面スタイル
/// 道路・フェンス・ガードレール等、Bezier カーブに沿って敷設されるオブジェクトで共用できる。
struct LinearNetworkStyle
{
	String            surfaceTexturePath;               ///< テクスチャパス（空 = テクスチャなし）
	ColorF            surfaceColor   = ColorF{ 0.35 };  ///< 路面色（テクスチャのティント色兼フォールバック）
	float             surfaceTileV   = 0.05f;           ///< 走行方向の UV タイリング [1/m]（大きいほど細かい）
	Optional<Texture> surfaceTexture;                   ///< ロード後に格納されるテクスチャ
};
