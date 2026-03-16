
#pragma once

/// @brief ゾーン種別（02_technical_spec.md §5、05_zoning_spec.md §2）
enum class ZoneType : uint8
{
	Unzoned        = 0,   ///< 未指定（既存建物は残る・新規開発不可）
	UrbanControl   = 1,   ///< 市街化調整区域（農地・公園のみ）
	LowResidential = 2,   ///< 低層住居専用（戸建て・低層マンション）
	Residential    = 3,   ///< 住居地域（住宅全般＋小規模店舗）
	Commercial     = 4,   ///< 商業地域（店舗・オフィス・マンション全般）
	Industrial     = 5,   ///< 工業地域（工場・倉庫）
	Agriculture    = 6,   ///< 農業地域（農地のみ）
};

/// @brief ゾーン種別の3D オーバーレイ用半透明カラーを返す
inline ColorF zoneColor(ZoneType z)
{
	switch (z)
	{
	case ZoneType::UrbanControl:   return ColorF{ 0.50, 0.80, 0.50, 0.45 };
	case ZoneType::LowResidential: return ColorF{ 0.95, 0.95, 0.20, 0.45 };
	case ZoneType::Residential:    return ColorF{ 1.00, 0.55, 0.10, 0.45 };
	case ZoneType::Commercial:     return ColorF{ 0.90, 0.15, 0.15, 0.45 };
	case ZoneType::Industrial:     return ColorF{ 0.65, 0.10, 0.85, 0.45 };
	case ZoneType::Agriculture:    return ColorF{ 0.20, 0.75, 0.20, 0.45 };
	default:                       return ColorF{ 0.00, 0.00, 0.00, 0.00 };
	}
}

/// @brief ゾーン種別の表示名を返す
inline StringView zoneName(ZoneType z)
{
	switch (z)
	{
	case ZoneType::UrbanControl:   return U"市街化調整区域";
	case ZoneType::LowResidential: return U"低層住居専用";
	case ZoneType::Residential:    return U"住居地域";
	case ZoneType::Commercial:     return U"商業地域";
	case ZoneType::Industrial:     return U"工業地域";
	case ZoneType::Agriculture:    return U"農業地域";
	default:                       return U"未指定";
	}
}
