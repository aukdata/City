#pragma once
#include <Siv3D.hpp>
#include "../zone/Building.hpp"
#include "StreetProfile.hpp"

namespace UrbanMorphology
{
	struct Plan;
	struct Site;
	struct LandUse;
} // namespace UrbanMorphology

/// @brief 成立起源と分離した現代の都市骨格。同じ計画を街路・用途・駅で共有する。
namespace UrbanStructure
{
	enum class Type : uint8
	{
		None,
		Metropolitan,
		HistoricGrid,
		TransitCorridor,
		CoastalHubs,
		PlannedGrid,
		ConstrainedLinear,
		RegionalHub
	};
	enum class CenterRole : uint8
	{
		Business,
		Shopping,
		Waterfront
	};
	struct Center
	{
		Vec2 position;
		double radius = 0;
		CenterRole role = CenterRole::Business;
		bool rail = false;
	};
	struct Profile
	{
		String name;
#define URBAN_FIELD(type, name, minimum, maximum) type name{};
#include "UrbanStructureFields.def"
#undef URBAN_FIELD
		Array<Center> centers; ///< 設定では半幅を1とする座標、Plan内では地区座標[m]。
		Array<RectF> greenAreas;
	};
	Array<Profile> load(FilePathView path);
	const Profile& profile(Type type);
	StringView id(Type type);
	Type fromId(StringView value);
	Type choose(const UrbanMorphology::Site& site, uint64 salt);
	void apply(UrbanMorphology::Plan& plan, Type type);
	/// @brief 地形による縮小後も、駅を道路交差点の直上へ戻さない。
	void alignCenters(UrbanMorphology::Plan& plan);
	double intensity(const UrbanMorphology::Plan& plan, Vec2 point);
	UrbanMorphology::LandUse sample(const UrbanMorphology::Plan& plan, Vec2 point);
	Array<float> streetCoordinates(const UrbanMorphology::Plan& plan, bool crossAxis);
	GeneratedStreet::Role streetRole(const UrbanMorphology::Plan& plan, const Array<float>& x, const Array<float>& z,
		int colA, int rowA, int colB, int rowB);
	bool allowStreet(const UrbanMorphology::Plan& plan, const Array<float>& x, const Array<float>& z, int colA,
		int rowA, int colB, int rowB);
	void adaptBuilding(Building& building, const UrbanMorphology::Plan& plan, Vec2 local, uint32 roll);
	Array<Vec2> stationPositions(const UrbanMorphology::Plan& plan);
	/// @brief 保存時は生成結果を保持し、再読込時に乱数や最新設定で配置を変えない。
	JSON saveLayout(const UrbanMorphology::Plan& plan);
	void restoreLayout(UrbanMorphology::Plan& plan, const JSON& state);
} // namespace UrbanStructure
