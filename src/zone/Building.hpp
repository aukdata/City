#include "../gen/GenerationSettings.hpp"

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
	UrbanConvenience = 12, ///< 街中のコンビニ（上階付き・徒歩来店）
	RoadsideConvenience = 13, ///< 広い駐車場を持つコンビニ
	UrbanFuelStation = 14, ///< 市街地の小規模給油所
	RoadsideFuelStation = 15, ///< 郊外のセルフ給油所・洗車場
	RuralHouse = 16, ///< 庭・附属屋を持つ田舎の民家
	OfficeTower, ///< 高層オフィス
	CityHall, ///< 市役所
	ShoppingMall, ///< 郊外ショッピングモール
	Hospital, ///< 総合病院
	School, ///< 学校
	UrbanHousePair, ///< 都心の狭小な連棟住宅（2戸）
	VillageHouse, ///< 古い村の小さな民家
	IndustrialWarehouse, ///< 工業団地の大きな倉庫
	Count,
};

/// @brief 敷地内設備を含む専用モデルか。汎用の玄関装飾を重ねない。
inline bool isCompleteSiteBuilding(BuildingType type)
{
	return type >= BuildingType::UrbanConvenience && type <= BuildingType::School;
}

/// @brief 商業沿道サービス施設か。
inline bool isRoadsideServiceBuilding(BuildingType type)
{
	return type >= BuildingType::UrbanConvenience && type <= BuildingType::RoadsideFuelStation;
}

/// @brief 建物占有幅 [m]
inline float buildingFootprintXZ(BuildingType type = BuildingType::Detached)
{
	switch (type)
	{
	case BuildingType::Detached: return GenerationSettings::get().buildings_footprint_Detached;
	case BuildingType::UrbanHousePair: return 11.5f;
	case BuildingType::VillageHouse: return 14.0f;
	case BuildingType::IndustrialWarehouse: return 28.0f;
	case BuildingType::LowApartment: return GenerationSettings::get().buildings_footprint_LowApartment;
	case BuildingType::MidApartment:
	case BuildingType::HighApartment:
	case BuildingType::Office:
	return GenerationSettings::get().buildings_footprint_Office;
	case BuildingType::Shop: return GenerationSettings::get().buildings_footprint_Shop;
	case BuildingType::Factory: return GenerationSettings::get().buildings_footprint_Factory;
	case BuildingType::PublicFacility: return GenerationSettings::get().buildings_footprint_PublicFacility;
	case BuildingType::Parking: return GenerationSettings::get().buildings_footprint_Parking;
	case BuildingType::UrbanConvenience: return GenerationSettings::get().buildings_footprint_UrbanConvenience;
	case BuildingType::RoadsideConvenience: return GenerationSettings::get().buildings_footprint_RoadsideConvenience;
	case BuildingType::UrbanFuelStation: return GenerationSettings::get().buildings_footprint_UrbanFuelStation;
	case BuildingType::RoadsideFuelStation: return GenerationSettings::get().buildings_footprint_RoadsideFuelStation;
	case BuildingType::RuralHouse: return GenerationSettings::get().buildings_footprint_RuralHouse;
	case BuildingType::OfficeTower: return static_cast<float>(GenerationSettings::get().landmarks_footprint_OfficeTower);
	case BuildingType::CityHall: return static_cast<float>(GenerationSettings::get().landmarks_footprint_CityHall);
	case BuildingType::ShoppingMall: return static_cast<float>(GenerationSettings::get().landmarks_footprint_ShoppingMall);
	case BuildingType::Hospital: return static_cast<float>(GenerationSettings::get().landmarks_footprint_Hospital);
	case BuildingType::School: return static_cast<float>(GenerationSettings::get().landmarks_footprint_School);
	default: return GenerationSettings::get().buildings_footprint_default;
	}
}

/// @brief 探索範囲も最大敷地に追従させ、モールの外縁へ住宅を重ねない。
inline float maximumBuildingFootprint()
{
	static const float maximum=[]
	{
		float result=0;
		for (int value=1;value<static_cast<int>(BuildingType::Count);++value)
		{
			result=Max(result,buildingFootprintXZ(static_cast<BuildingType>(value)));
		}
		return result;
	}();
	return maximum;
}

/// @brief 住宅系建物かどうか
inline bool isResidentialBuildingType(BuildingType t)
{
	return t == BuildingType::Detached
	    || t == BuildingType::UrbanHousePair
	    || t == BuildingType::LowApartment
	    || t == BuildingType::MidApartment
	    || t == BuildingType::HighApartment
	    || t == BuildingType::RuralHouse
	    || t == BuildingType::VillageHouse;
}

/// @brief 住宅タイプ + グローバルセル座標から OBJ インデックス（0..17）を返す
inline uint8 residentialModelIndex(BuildingType t, int gx, int gz)
{
	const uint32 h = (static_cast<uint32>(gx) * 73856093u)
	               ^ (static_cast<uint32>(gz) * 19349663u);
	static constexpr uint8 kDetachedVariants[] = { 0, 1, 2, 3, 10, 11, 12, 13 };
	static constexpr uint8 kLowApartmentVariants[] = { 4, 5, 14, 15 };
	static constexpr uint8 kMidApartmentVariants[] = { 6, 7, 16 };
	static constexpr uint8 kHighApartmentVariants[] = { 8, 9, 17 };
	switch (t)
	{
	case BuildingType::Detached:      return kDetachedVariants[h % std::size(kDetachedVariants)];
	case BuildingType::LowApartment:  return kLowApartmentVariants[h % std::size(kLowApartmentVariants)];
	case BuildingType::MidApartment:  return kMidApartmentVariants[h % std::size(kMidApartmentVariants)];
	case BuildingType::HighApartment: return kHighApartmentVariants[h % std::size(kHighApartmentVariants)];
	default:                          return 0;
	}
}

/// @brief 建物タイプ + グローバルセル座標から OBJ バリエーション番号を返す
inline uint8 buildingModelVariant(BuildingType t, int gx, int gz)
{
	const uint32 h = (static_cast<uint32>(gx) * 73856093u)
	               ^ (static_cast<uint32>(gz) * 19349663u);
	if (t == BuildingType::OfficeTower) { return static_cast<uint8>(h % GenerationSettings::get().landmarks_towerVariants); }
	if (t == BuildingType::RuralHouse || t == BuildingType::VillageHouse) { return static_cast<uint8>(h % GenerationSettings::get().buildings_ruralHouseVariants); }
	if (isResidentialBuildingType(t)) { return residentialModelIndex(t, gx, gz); }
	const uint32 kShopVariantCount = GenerationSettings::get().buildings_shopVariants;
	const uint32 kFacilityVariantCount = GenerationSettings::get().buildings_factoryVariants;
	if (t == BuildingType::Shop)
	{
		// These two assets are six-storey blocks; retail lots use the low-rise set.
		const uint32 variant=h%kShopVariantCount;
		return static_cast<uint8>((variant==6u || variant==7u) ? variant-6u : variant);
	}
	if (t == BuildingType::PublicFacility)
	{
		const uint32 kPublicVariantCount = GenerationSettings::get().buildings_publicVariants;
		return static_cast<uint8>(h % kPublicVariantCount);
	}
	if (t == BuildingType::Parking)
	{
		const uint32 kParkingVariantCount = GenerationSettings::get().buildings_parkingVariants;
		return static_cast<uint8>(h % kParkingVariantCount);
	}
	if (t == BuildingType::Office) { return static_cast<uint8>(h % GenerationSettings::get().buildings_officeVariants); }
	if (t == BuildingType::Factory)
	{
		return static_cast<uint8>(h % kFacilityVariantCount);
	}
	return 0;
}

/// @brief OBJ で描画する建物種別かどうか
inline bool isObjBuildingType(BuildingType t)
{
	return (isResidentialBuildingType(t) && t != BuildingType::UrbanHousePair)
	    || t == BuildingType::Shop
	    || t == BuildingType::Office
	    || t == BuildingType::PublicFacility
	    || t == BuildingType::Factory
	    || t == BuildingType::Parking
	    || isCompleteSiteBuilding(t);
}

/// @brief 建物タイプ + グローバルセル座標から OBJ ファイルの stem（拡張子なし）を返す
/// @example "residential_001", "shop_001", "office_001"
inline bool tryGetBuildingModelStemForVariant(BuildingType type, uint8 variant, String& outStem)
{
	if (type == BuildingType::UrbanHousePair) { return false; }
	StringView prefix;
	switch (type)
	{
	case BuildingType::RuralHouse:
	case BuildingType::VillageHouse: prefix = U"rural_house"; break;
	case BuildingType::UrbanConvenience: prefix = U"convenience_urban"; break;
	case BuildingType::RoadsideConvenience: prefix = U"convenience_roadside"; break;
	case BuildingType::UrbanFuelStation: prefix = U"fuel_urban"; break;
	case BuildingType::RoadsideFuelStation: prefix = U"fuel_roadside"; break;
	case BuildingType::Shop: prefix = U"shop"; break;
	case BuildingType::Factory: prefix = U"factory"; break;
	case BuildingType::PublicFacility: prefix = U"public"; break;
	case BuildingType::Parking: prefix = U"parking"; break;
	case BuildingType::Office: prefix = U"office"; break;
	case BuildingType::OfficeTower: prefix = U"office_tower"; break;
	case BuildingType::CityHall: prefix = U"city_hall"; break;
	case BuildingType::ShoppingMall: prefix = U"shopping_mall"; break;
	case BuildingType::Hospital: prefix = U"hospital"; break;
	case BuildingType::School: prefix = U"school"; break;
	default:
		if (!isResidentialBuildingType(type)) { return false; }
		prefix = U"residential";
		break;
	}
	outStem = U"{}_{:03d}"_fmt(prefix, variant + 1);
	return true;
}

/// @brief 座標ハッシュでモデルを選び、描画と配置に共通の名前を返す。
inline bool tryGetBuildingModelStem(BuildingType type, int gx, int gz, String& outStem)
{
	return tryGetBuildingModelStemForVariant(type, buildingModelVariant(type, gx, gz), outStem);
}

/// @brief 建物の収容人口を返す（住宅系のみ正値、05_zoning_spec.md §1）
inline int buildingCapacity(BuildingType t)
{
	switch (t)
	{
	case BuildingType::RuralHouse:    return GenerationSettings::get().buildings_capacity_RuralHouse;
	case BuildingType::VillageHouse:  return GenerationSettings::get().buildings_capacity_RuralHouse;
	case BuildingType::Detached:      return GenerationSettings::get().buildings_capacity_Detached;
	case BuildingType::UrbanHousePair: return GenerationSettings::get().buildings_capacity_Detached * 2;
	case BuildingType::LowApartment:  return GenerationSettings::get().buildings_capacity_LowApartment;
	case BuildingType::MidApartment:  return GenerationSettings::get().buildings_capacity_MidApartment;
	case BuildingType::HighApartment: return GenerationSettings::get().buildings_capacity_HighApartment;
	default:                          return 0;
	}
}

/// @brief 建物種別の描画高さ [m] を返す
inline float buildingHeight(BuildingType type)
{
	switch (type)
	{
	case BuildingType::Detached:       return GenerationSettings::get().buildings_height_Detached;
	case BuildingType::UrbanHousePair: return 7.0f;
	case BuildingType::LowApartment:   return GenerationSettings::get().buildings_height_LowApartment;
	case BuildingType::MidApartment:   return GenerationSettings::get().buildings_height_MidApartment;
	case BuildingType::HighApartment:  return GenerationSettings::get().buildings_height_HighApartment;
	case BuildingType::Shop:           return GenerationSettings::get().buildings_height_Shop;
	case BuildingType::Office:         return GenerationSettings::get().buildings_height_Office;
	case BuildingType::Factory:        return GenerationSettings::get().buildings_height_Factory;
	case BuildingType::ParkBuilding:   return GenerationSettings::get().buildings_height_ParkBuilding;
	case BuildingType::PublicFacility: return GenerationSettings::get().buildings_height_PublicFacility;
	case BuildingType::Parking:        return GenerationSettings::get().buildings_height_Parking;
	case BuildingType::UrbanConvenience: return GenerationSettings::get().buildings_height_UrbanConvenience;
	case BuildingType::RoadsideConvenience: return GenerationSettings::get().buildings_height_RoadsideConvenience;
	case BuildingType::UrbanFuelStation:
	case BuildingType::RoadsideFuelStation: return GenerationSettings::get().buildings_height_RoadsideFuelStation;
	case BuildingType::RuralHouse: return GenerationSettings::get().buildings_height_RuralHouse;
	case BuildingType::VillageHouse: return 7.0f;
	case BuildingType::IndustrialWarehouse: return 10.0f;
	case BuildingType::OfficeTower: return static_cast<float>(GenerationSettings::get().landmarks_height_OfficeTower);
	case BuildingType::CityHall: return static_cast<float>(GenerationSettings::get().landmarks_height_CityHall);
	case BuildingType::ShoppingMall: return static_cast<float>(GenerationSettings::get().landmarks_height_ShoppingMall);
	case BuildingType::Hospital: return static_cast<float>(GenerationSettings::get().landmarks_height_Hospital);
	case BuildingType::School: return static_cast<float>(GenerationSettings::get().landmarks_height_School);
	default:                           return 0.0f;
	}
}

/// @brief 建物種別の描画色を返す
inline ColorF buildingColor(BuildingType type)
{
	switch (type)
	{
	case BuildingType::Detached:       return ColorF{ 0.74, 0.68, 0.58 };
	case BuildingType::UrbanHousePair: return ColorF{ 0.70, 0.67, 0.61 };
	case BuildingType::VillageHouse: return ColorF{ 0.70, 0.65, 0.57 };
	case BuildingType::IndustrialWarehouse: return ColorF{ 0.66, 0.68, 0.69 };
	case BuildingType::LowApartment:   return ColorF{ 0.66, 0.67, 0.63 };
	case BuildingType::MidApartment:   return ColorF{ 0.56, 0.59, 0.62 };
	case BuildingType::HighApartment:  return ColorF{ 0.48, 0.53, 0.58 };
	case BuildingType::Shop:           return ColorF{ 0.70, 0.59, 0.44 };
	case BuildingType::Office:         return ColorF{ 0.58, 0.62, 0.64 };
	case BuildingType::Factory:        return ColorF{ 0.48, 0.47, 0.43 };
	case BuildingType::Farmland:       return ColorF{ 0.58, 0.63, 0.39 };
	case BuildingType::ParkBuilding:   return ColorF{ 0.36, 0.52, 0.32 };
	case BuildingType::PublicFacility: return ColorF{ 0.62, 0.58, 0.54 };
	case BuildingType::Parking:        return ColorF{ 0.44, 0.45, 0.43 };
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
	float        offsetX  = 0.0f;             ///< ゾーンセル中心からの X オフセット [m]
	float        offsetZ  = 0.0f;             ///< ゾーンセル中心からの Z オフセット [m]
};
