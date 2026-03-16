
#pragma once
#include "ZoneTypes.hpp"

/// @brief 建物種別（05_zoning_spec.md §1）
enum class BuildingType : uint8
{
	None           = 0,
	Detached       = 1,   ///< 戸建て住宅（収容 3人）
	LowApartment   = 2,   ///< 低層マンション（収容 20人）
	MidApartment   = 3,   ///< 中層マンション（収容 80人）
	HighApartment  = 4,   ///< 高層マンション（収容 300人）
	Shop           = 5,   ///< 店舗
	Office         = 6,   ///< オフィスビル
	Factory        = 7,   ///< 工場
	Farmland       = 8,   ///< 農地
	ParkBuilding   = 9,   ///< 公園
	PublicFacility = 10,  ///< 公共施設
	Parking        = 11,  ///< 駐車場
};

/// @brief 建物の収容人口を返す（住宅系のみ正値、05_zoning_spec.md §1）
inline int buildingCapacity(BuildingType t)
{
	switch (t)
	{
	case BuildingType::Detached:      return 3;
	case BuildingType::LowApartment:  return 20;
	case BuildingType::MidApartment:  return 80;
	case BuildingType::HighApartment: return 300;
	default:                          return 0;
	}
}

/// @brief チャンク内1ゾーンセルの建物データ
struct Building
{
	BuildingType type    = BuildingType::None;
	uint8        stage   = 0;      ///< 成長段階 0〜3
	double       builtAt = 0.0;    ///< 建設時刻 [ゲーム秒]
};
