#pragma once
#include "GenerationSettings.hpp"
#include <Siv3D.hpp>

/// @brief 地形・成立史から街路と土地利用に共通の計画を構成する。
namespace UrbanMorphology
{
	enum class Origin : uint8 { Castle, Post, Temple, Port, Market, Industrial, Planned, Rural };
	enum class RuralForm : uint8 { Clustered, Dispersed, Valley };
	enum class District : uint8 { Countryside, OldTown, Station, Housing, Industry, Civic, PlannedHousing };
	enum class Generation : uint8 { Historic, Railway, Modern };

	/// @brief 海岸距離はサンプルした水域境界まで。地形だけから水域の海・湖を断定しない。
	struct Site
	{
		double elevation = 0;
		double relief = 0;
		double shoreDistance = 1e9;
		Vec2 shoreDirection{0,1};
		Vec2 contourAxis{1,0};
	};

	struct Plan
	{
		Origin origin = Origin::Rural;
		RuralForm ruralForm = RuralForm::Clustered;
		uint8 scale = 2;
		uint64 salt = 0;
		bool ready = false;
		bool frontageRoads = false;
		Array<Vec2> ruralHomes;
		Array<Line> fringeStreets; ///< 生成済み外縁街路の地区座標。沿道だけを住宅候補にする。
		Vec2 halfExtent{GenerationSettings::get().settlements_defaultExtentX,GenerationSettings::get().settlements_defaultExtentZ};
		Vec2 oldCore{0,0};
		Optional<Vec2> station;
		Optional<RectF> civic;
		RectF industry{0,0,0,0};
	};

	struct LandUse
	{
		District district = District::Countryside;
		Generation generation = Generation::Historic;
		double occupancy = 0;
		double frontage = GenerationSettings::get().settlements_countrysideFrontage;
	};

	inline uint64 mix(uint64 value)
	{
		value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
		value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
		return value ^ (value >> 31);
	}

	inline String originName(Origin origin)
	{
		switch (origin)
		{
		case Origin::Castle: return U"城下町";
		case Origin::Post: return U"宿場・街道町";
		case Origin::Temple: return U"門前町";
		case Origin::Port: return U"港町";
		case Origin::Market: return U"市場・在郷町";
		case Origin::Industrial: return U"工業都市";
		case Origin::Planned: return U"鉄道・計画市街地";
		default: return U"農村";
		}
	}

	template <class HeightSampler>
	Site inspectSite(const Vec2& center, const HeightSampler& height)
	{
		Site site;
		site.elevation = height(center);
		double low = site.elevation, high = site.elevation;
		const int kDirections = GenerationSettings::get().settlements_siteSampleDirections;
		const double kStep = GenerationSettings::get().settlements_siteSampleStep;
		for (int direction = 0; direction < kDirections; ++direction)
		{
			const double angle = Math::TwoPi * direction / kDirections;
			const Vec2 axis{Math::Cos(angle),Math::Sin(angle)};
			for (int step = 1; step <= GenerationSettings::get().settlements_siteSampleSteps; ++step)
			{
				const double distance = kStep * step;
				const double h = height(center + axis * distance);
				if (distance <= GenerationSettings::get().settlements_siteReliefRadius) { low = Min(low,h); high = Max(high,h); }
				if (h < GenerationSettings::get().settlements_siteWaterHeight)
				{
					double a = distance-kStep, b = distance;
					for (int iteration = 0; iteration < 10; ++iteration)
					{
						const double middle = (a+b)*0.5;
						if (height(center+axis*middle)<GenerationSettings::get().settlements_siteWaterHeight) { b=middle; }
						else { a=middle; }
					}
					if (b < site.shoreDistance) { site.shoreDistance=b; site.shoreDirection=axis; }
					break;
				}
			}
		}
		site.relief = high-low;
		const Vec2 gradient{height(center+Vec2{GenerationSettings::get().settlements_contourSampleRadius,0})-height(center-Vec2{GenerationSettings::get().settlements_contourSampleRadius,0}),
			height(center+Vec2{0,GenerationSettings::get().settlements_contourSampleRadius})-height(center-Vec2{0,GenerationSettings::get().settlements_contourSampleRadius})};
		if (gradient.lengthSq()>0.01) { site.contourAxis=Vec2{-gradient.y,gradient.x}.normalized(); }
		return site;
	}

	/// @brief 規模と成立史を独立に選ぶ。水際のない港・急傾斜の工業都市は選ばない。
	inline Origin chooseOrigin(uint8 scale, const Site& site, uint64 salt, bool nearRegionalCity)
	{
		const uint64 choice=mix(salt)%100;
		if (scale==2) { return Origin::Rural; }
		if (site.elevation<GenerationSettings::get().settlements_portMaximumHeight && site.shoreDistance>GenerationSettings::get().settlements_portMinimumShoreDistance && site.shoreDistance<GenerationSettings::get().settlements_portMaximumShoreDistance) { return Origin::Port; }
		if (site.relief>GenerationSettings::get().settlements_templeRelief && choice<GenerationSettings::get().settlements_templeChoiceThreshold) { return Origin::Temple; }
		if (site.relief<GenerationSettings::get().settlements_industrialMaximumRelief && choice<GenerationSettings::get().settlements_industrialChoiceThreshold) { return Origin::Industrial; }
		if (nearRegionalCity && choice<GenerationSettings::get().settlements_plannedChoiceThreshold) { return Origin::Planned; }
		if (scale==0) { return choice<GenerationSettings::get().settlements_castleChoiceThreshold ? Origin::Castle : Origin::Market; }
		return choice<GenerationSettings::get().settlements_postChoiceThreshold ? Origin::Post : (choice<GenerationSettings::get().settlements_marketChoiceThreshold ? Origin::Market : Origin::Temple);
	}

	inline Array<float> streetCoordinates(const Plan& plan, bool crossAxis);

	inline Plan makePlan(Origin origin, uint8 scale, const Site& site, uint64 salt, bool railway)
	{
		Plan plan;
		plan.origin=origin; plan.scale=scale; plan.salt=salt; plan.ready=true;
		const double size=scale==0 ? 1.0 : GenerationSettings::get().settlements_townScale;
		switch (origin)
		{
		case Origin::Castle: plan.halfExtent={GenerationSettings::get().settlements_castleExtentX,GenerationSettings::get().settlements_castleExtentZ}; break;
		case Origin::Post: plan.halfExtent={GenerationSettings::get().settlements_postExtentX,GenerationSettings::get().settlements_postExtentZ}; break;
		case Origin::Temple: plan.halfExtent={GenerationSettings::get().settlements_templeExtentX,GenerationSettings::get().settlements_templeExtentZ}; break;
		case Origin::Port: plan.halfExtent={GenerationSettings::get().settlements_portExtentX,Clamp(site.shoreDistance-GenerationSettings::get().settlements_portShoreSetback,GenerationSettings::get().settlements_portMinimumDepth,GenerationSettings::get().settlements_portMaximumDepth)/size}; break;
		case Origin::Market: plan.halfExtent={GenerationSettings::get().settlements_marketExtentX,GenerationSettings::get().settlements_marketExtentZ}; break;
		case Origin::Industrial: plan.halfExtent={GenerationSettings::get().settlements_industrialExtentX,GenerationSettings::get().settlements_industrialExtentZ}; break;
		case Origin::Planned: plan.halfExtent={GenerationSettings::get().settlements_plannedExtentX,GenerationSettings::get().settlements_plannedExtentZ}; break;
		case Origin::Rural:
			plan.ruralForm=site.relief>GenerationSettings::get().settlements_valleyRelief ? RuralForm::Valley : (mix(salt)%GenerationSettings::get().settlements_dispersedChoiceDivisor==0 ? RuralForm::Dispersed : RuralForm::Clustered);
			plan.halfExtent=plan.ruralForm==RuralForm::Valley ? Vec2{GenerationSettings::get().settlements_valleyExtentX,GenerationSettings::get().settlements_valleyExtentZ}
				: (plan.ruralForm==RuralForm::Dispersed ? Vec2{GenerationSettings::get().settlements_dispersedExtentX,GenerationSettings::get().settlements_dispersedExtentZ} : Vec2{GenerationSettings::get().settlements_clusterExtentX,GenerationSettings::get().settlements_clusterExtentZ});
			return plan;
		}
		plan.halfExtent*=size;
		const Vec2 extent=plan.halfExtent;
		plan.oldCore={-extent.x*GenerationSettings::get().settlements_oldCoreOffset,0};
		if (railway)
		{
			plan.station=Vec2{extent.x*GenerationSettings::get().settlements_stationOffsetX,-extent.y*GenerationSettings::get().settlements_stationOffsetZ};
		}
		if (origin==Origin::Castle)
		{
			plan.civic=RectF{-extent.x*GenerationSettings::get().settlements_castleCivicX,extent.y*GenerationSettings::get().settlements_castleCivicZ,extent.x*GenerationSettings::get().settlements_castleCivicWidth,extent.y*GenerationSettings::get().settlements_castleCivicDepth};
		}
		else if (origin==Origin::Temple)
		{
			plan.civic=RectF{-extent.x,-extent.y*GenerationSettings::get().settlements_templeCivicX,extent.x*GenerationSettings::get().settlements_templeCivicZ,extent.y*GenerationSettings::get().settlements_templeCivicWidth};
		}
		else if (origin==Origin::Planned)
		{
			plan.civic=RectF{-extent.x*GenerationSettings::get().settlements_plannedCivicX,extent.y*GenerationSettings::get().settlements_plannedCivicZ,extent.x*GenerationSettings::get().settlements_plannedCivicWidth,extent.y*GenerationSettings::get().settlements_plannedCivicDepth};
		}
		if (origin==Origin::Industrial)
		{
			plan.industry=RectF{extent.x*GenerationSettings::get().settlements_industrialZoneX,extent.y*GenerationSettings::get().settlements_industrialZoneZ,extent.x*GenerationSettings::get().settlements_industrialZoneWidth,extent.y*GenerationSettings::get().settlements_industrialZoneDepth};
		}
		else if (origin==Origin::Port)
		{
			plan.industry=RectF{-extent.x*GenerationSettings::get().settlements_portIndustryX,extent.y*GenerationSettings::get().settlements_portIndustryZ,extent.x*GenerationSettings::get().settlements_portIndustryWidth,extent.y*GenerationSettings::get().settlements_portIndustryDepth};
		}
		if (plan.station)
		{
			const auto blockCenter=[](const Array<float>& coordinates,double desired)
			{
				for (size_t i=1;i<coordinates.size();++i)
				{
					if (desired<=coordinates[i]) { return (coordinates[i-1]+coordinates[i])*0.5; }
				}
				return desired;
			};
			plan.station=Vec2{blockCenter(streetCoordinates(plan,false),plan.station->x),blockCenter(streetCoordinates(plan,true),plan.station->y)};
		}
		return plan;
	}

	/// @brief 測定した造成可能範囲へ全ての用途・核を同時に縮める。
	inline void rescale(Plan& plan, double factor)
	{
		plan.halfExtent*=factor; plan.oldCore*=factor;
		if (plan.station) { *plan.station*=factor; }
		const auto scaleRectangle=[&](RectF& area)
		{
			area.x*=factor; area.y*=factor; area.w*=factor; area.h*=factor;
		};
		if (plan.civic) { scaleRectangle(*plan.civic); }
		scaleRectangle(plan.industry);
	}

	inline bool inCore(const Plan& plan, Vec2 point, double margin=0)
	{
		return Abs(point.x)<=plan.halfExtent.x+margin && Abs(point.y)<=plan.halfExtent.y+margin;
	}

	inline double fringeDistance(const Plan& plan, Vec2 point)
	{
		double distance=1e9;
		for (const auto& street : plan.fringeStreets)
		{
			const Vec2 span=street.end-street.begin;
			const double t=Clamp((point-street.begin).dot(span)/Max(1.0,span.lengthSq()),0.0,1.0);
			distance=Min(distance,point.distanceFrom(street.begin+span*t));
		}
		return distance;
	}

	inline double coverageRadius(const Plan& plan)
	{
		double radius=plan.halfExtent.length();
		for (const auto& street : plan.fringeStreets)
		{
			radius=Max(radius,Max(street.begin.length(),street.end.length())+GenerationSettings::get().settlements_coverageMargin);
		}
		return radius;
	}

	inline bool contains(const Plan& plan, Vec2 point, double margin=0)
	{
		return inCore(plan,point,margin) || fringeDistance(plan,point)<=GenerationSettings::get().settlements_fringeWidth+margin;
	}

	inline LandUse sample(const Plan& plan, Vec2 point)
	{
		if (!contains(plan,point,GenerationSettings::get().settlements_coreMargin)) { return {}; }
		if (!inCore(plan,point,GenerationSettings::get().settlements_coreMargin))
		{
			const double depth=Max(Abs(point.x)-plan.halfExtent.x,Abs(point.y)-plan.halfExtent.y);
			const double occupancy=GenerationSettings::get().settlements_fringeOccupancy*std::exp(-depth/(plan.scale==0 ? GenerationSettings::get().settlements_cityFringeDecayDistance : GenerationSettings::get().settlements_townFringeDecayDistance));
			return fringeDistance(plan,point)<GenerationSettings::get().settlements_fringeWidth ? LandUse{District::Housing,Generation::Modern,occupancy,GenerationSettings::get().settlements_fringeFrontage} : LandUse{};
		}
		if (plan.civic && plan.civic->contains(point)) { return {District::Civic,Generation::Historic,GenerationSettings::get().settlements_civicOccupancy,GenerationSettings::get().settlements_civicFrontage}; }
		if (plan.industry.w>0 && plan.industry.contains(point)) { return {District::Industry,Generation::Modern,GenerationSettings::get().settlements_industryOccupancy,GenerationSettings::get().settlements_industryFrontage}; }
		if (plan.origin==Origin::Rural)
		{
			if (plan.ruralForm==RuralForm::Dispersed)
			{
				if (!plan.ruralHomes.isEmpty())
				{
					for (const Vec2 home : plan.ruralHomes)
					{
						if (point.distanceFrom(home)<GenerationSettings::get().settlements_ruralHomeRadius) { return {District::Housing,Generation::Historic,GenerationSettings::get().settlements_dispersedRoadHomeOccupancy,GenerationSettings::get().settlements_dispersedRoadHomeFrontage}; }
					}
					return {};
				}
				const double width=plan.halfExtent.x/Max(1.0,std::round(plan.halfExtent.x/GenerationSettings::get().settlements_dispersedHomeSpacingX));
				const double depth=plan.halfExtent.y/Max(1.0,std::round(plan.halfExtent.y/GenerationSettings::get().settlements_dispersedHomeSpacingZ));
				const double x=point.x-std::round(point.x/width)*width;
				const double z=point.y-std::round(point.y/depth)*depth;
				return Abs(x)<GenerationSettings::get().settlements_dispersedHomeHalfX && Abs(z)<GenerationSettings::get().settlements_dispersedHomeHalfZ ? LandUse{District::Housing,Generation::Historic,GenerationSettings::get().settlements_dispersedHomeOccupancy,GenerationSettings::get().settlements_dispersedHomeFrontage} : LandUse{};
			}
			const bool housing=plan.ruralForm==RuralForm::Valley ? Abs(point.y)<GenerationSettings::get().settlements_valleyHomeHalfWidth
				: (Square(point.x/GenerationSettings::get().settlements_clusterHousingHalfX)+Square(point.y/GenerationSettings::get().settlements_clusterHousingHalfZ)<1.0);
			return housing ? LandUse{District::Housing,Generation::Historic,GenerationSettings::get().settlements_villageHomeOccupancy,GenerationSettings::get().settlements_villageHomeFrontage} : LandUse{};
		}
		if (plan.station)
		{
			const double radius=plan.scale==0 ? GenerationSettings::get().settlements_cityStationRadius : GenerationSettings::get().settlements_townStationRadius;
			if ((point-*plan.station).length()<radius) { return {District::Station,Generation::Railway,GenerationSettings::get().settlements_stationOccupancy,GenerationSettings::get().settlements_stationFrontage}; }
			const Vec2 corner{plan.station->x,plan.oldCore.y};
			const auto distanceToStreet=[&](Vec2 a,Vec2 b)
			{
				const Vec2 span=b-a;
				const double t=Clamp((point-a).dot(span)/Max(1.0,span.lengthSq()),0.0,1.0);
				return point.distanceFrom(a+span*t);
			};
			if (Min(distanceToStreet(plan.oldCore,corner),distanceToStreet(corner,*plan.station))<GenerationSettings::get().settlements_stationStreetRadius)
			{
				return {District::OldTown,Generation::Railway,GenerationSettings::get().settlements_stationStreetOccupancy,GenerationSettings::get().settlements_stationStreetFrontage};
			}
		}
		const bool commercialAxis=Abs(point.y)<GenerationSettings::get().settlements_commercialAxisWidth && Abs(point.x)<plan.halfExtent.x*GenerationSettings::get().settlements_commercialAxisLengthRatio;
		const bool marketCross=plan.origin==Origin::Market && Abs(point.x)<GenerationSettings::get().settlements_marketCrossWidth;
		if (commercialAxis || marketCross) { return {District::OldTown,Generation::Historic,GenerationSettings::get().settlements_merchantStreetOccupancy,GenerationSettings::get().settlements_merchantStreetFrontage}; }
		const double edge=Max(Abs(point.x)/plan.halfExtent.x,Abs(point.y)/plan.halfExtent.y);
		const double density=1.0-GenerationSettings::get().settlements_edgeDensityReduction*Clamp((edge-GenerationSettings::get().settlements_edgeDensityStart)/GenerationSettings::get().settlements_edgeDensityRange,0.0,1.0);
		if (plan.origin==Origin::Planned || (point.x>plan.halfExtent.x*GenerationSettings::get().settlements_plannedHousingStartX && point.y<-plan.halfExtent.y*GenerationSettings::get().settlements_plannedHousingStartZ))
		{
			return {District::PlannedHousing,Generation::Modern,GenerationSettings::get().settlements_plannedHomeOccupancy*density,GenerationSettings::get().settlements_plannedHomeFrontage};
		}
		return {District::Housing,Generation::Historic,GenerationSettings::get().settlements_historicHomeOccupancy*density,GenerationSettings::get().settlements_historicHomeFrontage};
	}

	/// @brief 街区の用途に応じて街路間隔を変える。各交差点にはノイズを加えない。
	inline Array<float> streetCoordinates(const Plan& plan, bool crossAxis)
	{
		const double extent=crossAxis ? plan.halfExtent.y : plan.halfExtent.x;
		double spacing=crossAxis ? GenerationSettings::get().settlements_historicSpacingCross : GenerationSettings::get().settlements_historicSpacingAlong;
		if (plan.origin==Origin::Post || plan.origin==Origin::Temple || plan.origin==Origin::Port) { spacing=crossAxis ? GenerationSettings::get().settlements_linearTownSpacingCross : GenerationSettings::get().settlements_linearTownSpacingAlong; }
		if (plan.origin==Origin::Industrial || plan.origin==Origin::Planned) { spacing=crossAxis ? GenerationSettings::get().settlements_modernSpacingCross : GenerationSettings::get().settlements_modernSpacingAlong; }
		if (plan.origin==Origin::Rural)
		{
			spacing=plan.ruralForm==RuralForm::Dispersed ? (crossAxis ? GenerationSettings::get().settlements_dispersedSpacingCross : GenerationSettings::get().settlements_dispersedSpacingAlong) : (crossAxis ? GenerationSettings::get().settlements_villageSpacingCross : GenerationSettings::get().settlements_villageSpacingAlong);
		}
		const int cells=Max(2,static_cast<int>(std::round(2*extent/spacing/2))*2);
		Array<float> coordinates;
		for (int cell=0;cell<=cells;++cell)
		{
			const double t=2.0*cell/cells-1.0;
			// Keep the central merchant blocks shallower and outer residential blocks deeper.
			const bool historic=plan.origin!=Origin::Industrial && plan.origin!=Origin::Planned && plan.origin!=Origin::Rural;
			const double position=historic ? extent*(GenerationSettings::get().settlements_historicGridLinearWeight*t+GenerationSettings::get().settlements_historicGridCubicWeight*t*t*t) : extent*t;
			coordinates << static_cast<float>(position);
		}
		return coordinates;
	}

	inline bool allowStreet(const Plan& plan, Vec2 a, Vec2 b)
	{
		const Vec2 middle=(a+b)*0.5;
		if (plan.civic && (plan.civic->contains(a) || plan.civic->contains(middle) || plan.civic->contains(b))) { return false; }
		// Combine industrial parcels into large blocks by omitting internal cross streets.
		if (plan.industry.w>0 && plan.industry.contains(middle) && Abs(a.y-b.y)<0.01
			&& middle.y<plan.industry.y+plan.industry.h*GenerationSettings::get().settlements_industrialStreetOmissionDepth) { return false; }
		return true;
	}
}
