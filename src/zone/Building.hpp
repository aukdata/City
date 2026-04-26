
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

/// @brief 建物占有幅 [m]（従来サイズ維持）
/// @details 旧実装では base(10m) * scale(5.0) = 50m だったため、固定 50m とする。
inline constexpr float kBuildingFootprintXZ = 50.0f;

/// @brief 建物占有幅 [m]
inline float buildingFootprintXZ()
{
	return kBuildingFootprintXZ;
}

/// @brief 住宅系建物かどうか
inline bool isResidentialBuildingType(BuildingType t)
{
	return t == BuildingType::Detached
	    || t == BuildingType::LowApartment
	    || t == BuildingType::MidApartment
	    || t == BuildingType::HighApartment;
}

/// @brief 住宅タイプ + グローバルセル座標から OBJ インデックス（0..9）を返す
inline uint8 residentialModelIndex(BuildingType t, int gx, int gz)
{
	const uint32 h = (static_cast<uint32>(gx) * 73856093u)
	               ^ (static_cast<uint32>(gz) * 19349663u);
	switch (t)
	{
	case BuildingType::Detached:      return 0;                            // 001
	case BuildingType::LowApartment:  return static_cast<uint8>(1 + h % 2); // 002-003
	case BuildingType::MidApartment:  return static_cast<uint8>(3 + h % 3); // 004-006
	case BuildingType::HighApartment: return static_cast<uint8>(6 + h % 4); // 007-010
	default:                          return 0;
	}
}

/// @brief OBJ で描画する建物種別かどうか
inline bool isObjBuildingType(BuildingType t)
{
	return isResidentialBuildingType(t)
	    || t == BuildingType::Shop
	    || t == BuildingType::Office;
}

/// @brief 建物タイプ + グローバルセル座標から OBJ ファイルの stem（拡張子なし）を返す
/// @example "residential_001", "shop_001", "office_001"
inline bool tryGetBuildingModelStem(BuildingType t, int gx, int gz, String& outStem)
{
	if (isResidentialBuildingType(t))
	{
		outStem = U"residential_{:03d}"_fmt(residentialModelIndex(t, gx, gz) + 1);
		return true;
	}
	if (t == BuildingType::Shop)
	{
		outStem = U"shop_001";
		return true;
	}
	if (t == BuildingType::Office)
	{
		outStem = U"office_001";
		return true;
	}
	return false;
}

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

/// @brief 建物種別の描画高さ [m] を返す
inline float buildingHeight(BuildingType type)
{
	switch (type)
	{
	case BuildingType::Detached:       return 4.0f;
	case BuildingType::LowApartment:   return 10.0f;
	case BuildingType::MidApartment:   return 24.0f;
	case BuildingType::HighApartment:  return 48.0f;
	case BuildingType::Shop:           return 4.0f;
	case BuildingType::Office:         return 16.0f;
	case BuildingType::Factory:        return 8.0f;
	case BuildingType::ParkBuilding:   return 0.5f;
	case BuildingType::PublicFacility: return 10.0f;
	case BuildingType::Parking:        return 2.5f;
	default:                           return 0.0f;
	}
}

/// @brief 建物種別の描画色を返す
inline ColorF buildingColor(BuildingType type)
{
	switch (type)
	{
	case BuildingType::Detached:       return ColorF{ 0.90, 0.82, 0.68 };
	case BuildingType::LowApartment:   return ColorF{ 0.65, 0.75, 0.90 };
	case BuildingType::MidApartment:   return ColorF{ 0.45, 0.58, 0.82 };
	case BuildingType::HighApartment:  return ColorF{ 0.30, 0.42, 0.75 };
	case BuildingType::Shop:           return ColorF{ 0.95, 0.78, 0.30 };
	case BuildingType::Office:         return ColorF{ 0.70, 0.75, 0.80 };
	case BuildingType::Factory:        return ColorF{ 0.50, 0.48, 0.46 };
	case BuildingType::ParkBuilding:   return ColorF{ 0.30, 0.70, 0.35 };
	case BuildingType::PublicFacility: return ColorF{ 0.80, 0.60, 0.85 };
	case BuildingType::Parking:        return ColorF{ 0.55, 0.55, 0.55 };
	default:                           return ColorF{ 0.60, 0.60, 0.60 };
	}
}

/// @brief チャンク内1ゾーンセルの建物データ
struct Building
{
	BuildingType type      = BuildingType::None;
	double       builtAt  = 0.0;              ///< 建設時刻 [ゲーム秒]
	float        angle    = 0.0f;             ///< 道路方向角 [rad] (XZ平面・Y軸回転)
	int32        edgeId   = -1;               ///< 接道している RoadEdge id（未設定は -1）
	float        edgeT    = 0.0f;             ///< 接道位置のパラメータ t (0..1)
};
