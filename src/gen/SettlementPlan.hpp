#pragma once
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
		Vec2 halfExtent{220,160};
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
		double frontage = 24;
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
		constexpr int kDirections = 16;
		constexpr double kStep = 100;
		for (int direction = 0; direction < kDirections; ++direction)
		{
			const double angle = Math::TwoPi * direction / kDirections;
			const Vec2 axis{Math::Cos(angle),Math::Sin(angle)};
			for (int step = 1; step <= 12; ++step)
			{
				const double distance = kStep * step;
				const double h = height(center + axis * distance);
				if (distance <= 600) { low = Min(low,h); high = Max(high,h); }
				if (h < 0.5)
				{
					double a = distance-kStep, b = distance;
					for (int iteration = 0; iteration < 10; ++iteration)
					{
						const double middle = (a+b)*0.5;
						if (height(center+axis*middle)<0.5) { b=middle; }
						else { a=middle; }
					}
					if (b < site.shoreDistance) { site.shoreDistance=b; site.shoreDirection=axis; }
					break;
				}
			}
		}
		site.relief = high-low;
		const Vec2 gradient{height(center+Vec2{160,0})-height(center-Vec2{160,0}),
			height(center+Vec2{0,160})-height(center-Vec2{0,160})};
		if (gradient.lengthSq()>0.01) { site.contourAxis=Vec2{-gradient.y,gradient.x}.normalized(); }
		return site;
	}

	/// @brief 規模と成立史を独立に選ぶ。水際のない港・急傾斜の工業都市は選ばない。
	inline Origin chooseOrigin(uint8 scale, const Site& site, uint64 salt, bool nearRegionalCity)
	{
		const uint64 choice=mix(salt)%100;
		if (scale==2) { return Origin::Rural; }
		if (site.elevation<65 && site.shoreDistance>90 && site.shoreDistance<650) { return Origin::Port; }
		if (site.relief>35 && choice<50) { return Origin::Temple; }
		if (site.relief<25 && choice<19) { return Origin::Industrial; }
		if (nearRegionalCity && choice<48) { return Origin::Planned; }
		if (scale==0) { return choice<75 ? Origin::Castle : Origin::Market; }
		return choice<62 ? Origin::Post : (choice<83 ? Origin::Market : Origin::Temple);
	}

	inline Array<float> streetCoordinates(const Plan& plan, bool crossAxis);

	inline Plan makePlan(Origin origin, uint8 scale, const Site& site, uint64 salt, bool railway)
	{
		Plan plan;
		plan.origin=origin; plan.scale=scale; plan.salt=salt; plan.ready=true;
		const double size=scale==0 ? 1.0 : 0.52;
		switch (origin)
		{
		case Origin::Castle: plan.halfExtent={1000,1000}; break;
		case Origin::Post: plan.halfExtent={1100,360}; break;
		case Origin::Temple: plan.halfExtent={950,560}; break;
		case Origin::Port: plan.halfExtent={1100,Clamp(site.shoreDistance-45.0,160.0,480.0)/size}; break;
		case Origin::Market: plan.halfExtent={920,760}; break;
		case Origin::Industrial: plan.halfExtent={1040,850}; break;
		case Origin::Planned: plan.halfExtent={960,800}; break;
		case Origin::Rural:
			plan.ruralForm=site.relief>28 ? RuralForm::Valley : (mix(salt)%3==0 ? RuralForm::Dispersed : RuralForm::Clustered);
			plan.halfExtent=plan.ruralForm==RuralForm::Valley ? Vec2{520,135}
				: (plan.ruralForm==RuralForm::Dispersed ? Vec2{580,380} : Vec2{230,170});
			return plan;
		}
		plan.halfExtent*=size;
		const Vec2 extent=plan.halfExtent;
		plan.oldCore={-extent.x*0.25,0};
		if (railway)
		{
			plan.station=Vec2{extent.x*0.48,-extent.y*0.26};
		}
		if (origin==Origin::Castle)
		{
			plan.civic=RectF{-extent.x*0.62,extent.y*0.17,extent.x*0.40,extent.y*0.35};
		}
		else if (origin==Origin::Temple)
		{
			plan.civic=RectF{-extent.x,-extent.y*0.30,extent.x*0.34,extent.y*0.60};
		}
		else if (origin==Origin::Planned)
		{
			plan.civic=RectF{-extent.x*0.52,extent.y*0.2,extent.x*0.34,extent.y*0.34};
		}
		if (origin==Origin::Industrial)
		{
			plan.industry=RectF{extent.x*0.08,extent.y*0.08,extent.x*0.88,extent.y*0.88};
		}
		else if (origin==Origin::Port)
		{
			plan.industry=RectF{-extent.x*0.90,extent.y*0.53,extent.x*1.80,extent.y*0.47};
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

	inline bool contains(const Plan& plan, Vec2 point, double margin=0)
	{
		return Abs(point.x)<=plan.halfExtent.x+margin && Abs(point.y)<=plan.halfExtent.y+margin;
	}

	inline LandUse sample(const Plan& plan, Vec2 point)
	{
		if (!contains(plan,point,22)) { return {}; }
		if (plan.civic && plan.civic->contains(point)) { return {District::Civic,Generation::Historic,1.0,36}; }
		if (plan.industry.w>0 && plan.industry.contains(point)) { return {District::Industry,Generation::Modern,0.92,40}; }
		if (plan.origin==Origin::Rural)
		{
			if (plan.ruralForm==RuralForm::Dispersed)
			{
				if (!plan.ruralHomes.isEmpty())
				{
					for (const Vec2 home : plan.ruralHomes)
					{
						if (point.distanceFrom(home)<42) { return {District::Housing,Generation::Historic,0.92,28}; }
					}
					return {};
				}
				const double width=plan.halfExtent.x/Max(1.0,std::round(plan.halfExtent.x/230.0));
				const double depth=plan.halfExtent.y/Max(1.0,std::round(plan.halfExtent.y/190.0));
				const double x=point.x-std::round(point.x/width)*width;
				const double z=point.y-std::round(point.y/depth)*depth;
				return Abs(x)<35 && Abs(z)<38 ? LandUse{District::Housing,Generation::Historic,0.64,42} : LandUse{};
			}
			const bool housing=plan.ruralForm==RuralForm::Valley ? Abs(point.y)<90
				: (Square(point.x/240.0)+Square(point.y/175.0)<1.0);
			return housing ? LandUse{District::Housing,Generation::Historic,0.84,23} : LandUse{};
		}
		if (plan.station)
		{
			const double radius=plan.scale==0 ? 240.0 : 150.0;
			if ((point-*plan.station).length()<radius) { return {District::Station,Generation::Railway,0.98,15}; }
			const Vec2 corner{plan.station->x,plan.oldCore.y};
			const auto distanceToStreet=[&](Vec2 a,Vec2 b)
			{
				const Vec2 span=b-a;
				const double t=Clamp((point-a).dot(span)/Max(1.0,span.lengthSq()),0.0,1.0);
				return point.distanceFrom(a+span*t);
			};
			if (Min(distanceToStreet(plan.oldCore,corner),distanceToStreet(corner,*plan.station))<55)
			{
				return {District::OldTown,Generation::Railway,0.96,14};
			}
		}
		const bool commercialAxis=Abs(point.y)<65 && Abs(point.x)<plan.halfExtent.x*0.90;
		const bool marketCross=plan.origin==Origin::Market && Abs(point.x)<65;
		if (commercialAxis || marketCross) { return {District::OldTown,Generation::Historic,0.98,14}; }
		if (plan.origin==Origin::Planned || (point.x>plan.halfExtent.x*0.25 && point.y<-plan.halfExtent.y*0.25))
		{
			return {District::PlannedHousing,Generation::Modern,0.94,23};
		}
		return {District::Housing,Generation::Historic,0.96,17};
	}

	/// @brief 街区の用途に応じて街路間隔を変える。各交差点にはノイズを加えない。
	inline Array<float> streetCoordinates(const Plan& plan, bool crossAxis)
	{
		const double extent=crossAxis ? plan.halfExtent.y : plan.halfExtent.x;
		double spacing=crossAxis ? 85.0 : 108.0;
		if (plan.origin==Origin::Post || plan.origin==Origin::Temple || plan.origin==Origin::Port) { spacing=crossAxis ? 75.0 : 120.0; }
		if (plan.origin==Origin::Industrial || plan.origin==Origin::Planned) { spacing=crossAxis ? 135.0 : 150.0; }
		if (plan.origin==Origin::Rural)
		{
			spacing=plan.ruralForm==RuralForm::Dispersed ? (crossAxis ? 190.0 : 230.0) : (crossAxis ? 170.0 : 145.0);
		}
		const int cells=Max(2,static_cast<int>(std::round(2*extent/spacing/2))*2);
		Array<float> coordinates;
		for (int cell=0;cell<=cells;++cell)
		{
			const double t=2.0*cell/cells-1.0;
			// Keep the central merchant blocks shallower and outer residential blocks deeper.
			const bool historic=plan.origin!=Origin::Industrial && plan.origin!=Origin::Planned && plan.origin!=Origin::Rural;
			const double position=historic ? extent*(0.84*t+0.16*t*t*t) : extent*t;
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
			&& middle.y<plan.industry.y+plan.industry.h*0.75) { return false; }
		return true;
	}
}
