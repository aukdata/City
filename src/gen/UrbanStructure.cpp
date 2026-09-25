#include "UrbanStructure.hpp"
#include "SettlementPlan.hpp"
#include <cmath>
#include <type_traits>

namespace
{
	using namespace UrbanStructure;
	const Array<String> typeIds{U"none", U"metropolitan", U"historic_grid", U"transit_corridor", U"coastal_hubs",
		U"planned_grid", U"constrained_linear", U"regional_hub"};

	void require(bool valid, StringView message)
	{
		if (!valid)
		{
			throw Error{U"都市構造設定: " + String{message}};
		}
	}
	double number(const JSON& object, StringView key, double minimum, double maximum)
	{
		const auto value = object[key].getOpt<double>();
		require(value && std::isfinite(*value) && *value >= minimum && *value <= maximum, key);
		return *value;
	}
	void keys(const JSON& object, const HashSet<String>& allowed)
	{
		require(object.isObject() && object.size() == allowed.size(), U"項目数またはオブジェクト形式");
		for (const auto item : object)
		{
			require(allowed.contains(item.key), U"未知の項目: " + item.key);
		}
	}
	double segmentDistance(Vec2 point, Vec2 a, Vec2 b)
	{
		const Vec2 span = b - a;
		return point.distanceFrom(a + span * Clamp((point - a).dot(span) / Max(1.0, span.lengthSq()), 0.0, 1.0));
	}
	int nearestIndex(const Array<float>& coordinates, double desired)
	{
		int best = 0;
		for (int i = 1; i < static_cast<int>(coordinates.size()); ++i)
		{
			if (Abs(coordinates[i] - desired) < Abs(coordinates[best] - desired))
			{
				best = i;
			}
		}
		return best;
	}
} // namespace

namespace UrbanStructure
{
	Array<Profile> load(FilePathView path)
	{
		const JSON source = JSON::Load(path);
		require(source && source.isObject() && source.size() == typeIds.size() - 1, U"urbanStructures.json");
		for (const auto item : source)
		{
			require(fromId(item.key) != Type::None, U"未知の都市型: " + item.key);
		}
		Array<Profile> result(typeIds.size());
		for (size_t index = 1; index < typeIds.size(); ++index)
		{
			const auto item = source[typeIds[index]];
			HashSet<String> allowed{U"name", U"centers", U"greenAreas"};
			auto& target = result[index];
#define URBAN_FIELD(type, name, minimum, maximum)                                                                      \
	{                                                                                                                  \
		const String key = Unicode::FromUTF8(#name);                                                                   \
		const double value = number(item, key, minimum, maximum);                                                      \
		require(!std::is_integral_v<type> || std::trunc(value) == value, key);                                         \
		target.name = static_cast<type>(value);                                                                        \
		allowed.insert(key);                                                                                           \
	}
#include "UrbanStructureFields.def"
#undef URBAN_FIELD
			keys(item, allowed);
			target.name = item[U"name"].get<String>();
			require(!target.name.isEmpty(), U"表示名");
			require(target.minimumRelief <= target.maximumRelief, U"地形の範囲");
			require(
				target.coreHighShare + target.coreOfficeShare + target.coreMidShare <= 1, U"中心の建物配分は合計1以下");
			require(target.innerDetachedShare <= target.outerDetachedShare, U"中心と郊外の戸建て配分");
			const auto centers = item[U"centers"];
			require(centers.isArray() && centers.size() >= 1 && centers.size() <= 8, U"中心は1〜8箇所");
			for (const auto entry : centers)
			{
				const auto value = entry.value;
				keys(value, {U"x", U"z", U"radius", U"role", U"rail"});
				const double role = number(value, U"role", 0, 2);
				require(std::trunc(role) == role, U"中心の役割");
				const auto rail = value[U"rail"].getOpt<bool>();
				require(rail.has_value(), U"駅フラグ");
				target.centers << Center{{number(value, U"x", -.85, .85), number(value, U"z", -.85, .85)},
					number(value, U"radius", .1, .8), static_cast<CenterRole>(static_cast<int>(role)), *rail};
			}
			require(target.centers.any([](const Center& center) { return center.rail; }), U"少なくとも1つの駅拠点");
			const auto green = item[U"greenAreas"];
			require(green.isArray() && green.size() <= 16, U"緑地の配列");
			for (const auto entry : green)
			{
				const auto value = entry.value;
				keys(value, {U"x", U"z", U"w", U"h"});
				const RectF area{number(value, U"x", -.95, .95), number(value, U"z", -.95, .95),
					number(value, U"w", .01, 1.9), number(value, U"h", .01, 1.9)};
				require(area.x + area.w <= .95 && area.y + area.h <= .95, U"緑地は市街地内");
				target.greenAreas << area;
			}
		}
		return result;
	}
	StringView id(Type type)
	{
		const auto index = static_cast<size_t>(type);
		return typeIds[index < typeIds.size() ? index : 0];
	}
	Type fromId(StringView value)
	{
		for (size_t i = 1; i < typeIds.size(); ++i)
		{
			if (typeIds[i] == value)
			{
				return static_cast<Type>(i);
			}
		}
		return Type::None;
	}
	const Profile& profile(Type type)
	{
		static const auto values =
			load(FileSystem::IsDirectory(U"assets/generation") ? U"assets/generation/urbanStructures.json"
															   : U"../../App/assets/generation/urbanStructures.json");
		const auto index = static_cast<size_t>(type);
		require(index > 0 && index < values.size(), U"都市型ID");
		return values[index];
	}
	Type choose(const UrbanMorphology::Site& site, uint64 salt)
	{
		Array<std::pair<Type, double>> candidates;
		double total = 0;
		for (size_t i = 1; i < typeIds.size(); ++i)
		{
			const auto type = static_cast<Type>(i);
			const auto& settings = profile(type);
			if (site.relief < settings.minimumRelief || site.relief > settings.maximumRelief)
			{
				continue;
			}
			if (settings.shoreRange > 0 && site.shoreDistance > settings.shoreRange)
			{
				continue;
			}
			// 適性を満たす型を比較する。水際のない福岡型、急斜面の札幌型を乱数だけで選ばない。
			const double weight = settings.weight;
			total += weight;
			candidates.emplace_back(type, total);
		}
		if (total <= 0)
		{
			return Type::RegionalHub;
		}
		const double choice =
			static_cast<double>(UrbanMorphology::mix(salt ^ 0xbfa734ca19ULL) >> 11) * 0x1.0p-53 * total;
		for (const auto& [type, upper] : candidates)
		{
			if (choice < upper)
			{
				return type;
			}
		}
		return candidates.back().first;
	}
	void alignCenters(UrbanMorphology::Plan& plan)
	{
		if (plan.structure == Type::None)
		{
			return;
		}
		const auto x = UrbanStructure::streetCoordinates(plan, false),
				   z = UrbanStructure::streetCoordinates(plan, true);
		const auto centerOfBlock = [](const Array<float>& coordinates, double desired)
		{
			for (size_t i = 1; i < coordinates.size(); ++i)
			{
				if (desired <= coordinates[i])
				{
					return (coordinates[i - 1] + coordinates[i]) * .5;
				}
			}
			return static_cast<double>((coordinates[coordinates.size() - 2] + coordinates.back()) * .5);
		};
		plan.station.reset();
		for (auto& center : plan.centers)
		{
			if (center.rail)
			{
				center.position = {centerOfBlock(x, center.position.x), centerOfBlock(z, center.position.y)};
			}
			const auto available = [&](Vec2 point)
			{
				return (!plan.civic || !plan.civic->contains(point)) &&
					   !plan.neighborhoodParks.any([&](const Polygon& park) { return park.contains(point); });
			};
			if (!available(center.position))
			{
				// 城址や寺社の予約を消さず、最も近い利用可能な街区に新しい拠点を寄せる。
				double nearest = Math::Inf;
				Vec2 best = center.position;
				for (size_t row = 1; row < z.size(); ++row)
					for (size_t col = 1; col < x.size(); ++col)
					{
						const Vec2 candidate{(x[col - 1] + x[col]) * .5, (z[row - 1] + z[row]) * .5};
						const double distance = candidate.distanceFromSq(center.position);
						if (distance < nearest && available(candidate))
						{
							nearest = distance;
							best = candidate;
						}
					}
				center.position = best;
			}
			if (center.rail && !plan.station)
			{
				plan.station = center.position;
			}
		}
	}

	void apply(UrbanMorphology::Plan& plan, Type type)
	{
		if (type == Type::None || plan.scale != 0 || plan.origin == UrbanMorphology::Origin::Planned ||
			plan.origin == UrbanMorphology::Origin::Rural)
		{
			return;
		}
		const auto& settings = profile(type);
		const Vec2 scale{settings.extentX / plan.halfExtent.x, settings.extentZ / plan.halfExtent.y};
		const auto rectangle = [&](RectF& area)
		{ area = {area.x * scale.x, area.y * scale.y, area.w * scale.x, area.h * scale.y}; };
		if (plan.civic)
		{
			rectangle(*plan.civic);
		}
		rectangle(plan.industry);
		plan.structure = type;
		plan.halfExtent = {settings.extentX, settings.extentZ};
		plan.centers.clear();
		plan.neighborhoodParks.clear();
		for (const auto& source : settings.centers)
		{
			Center center = source;
			center.position = {source.position.x * plan.halfExtent.x, source.position.y * plan.halfExtent.y};
			center.radius *= Min(plan.halfExtent.x, plan.halfExtent.y);
			plan.centers << center;
		}
		for (const auto& area : settings.greenAreas)
		{
			const RectF scaled{area.x * plan.halfExtent.x, area.y * plan.halfExtent.y, area.w * plan.halfExtent.x,
				area.h * plan.halfExtent.y};
			plan.neighborhoodParks << Polygon{{scaled.tl(), scaled.tr(), scaled.br(), scaled.bl()}};
		}
		alignCenters(plan);
		for (const auto& center : plan.centers)
		{
			if (center.role == CenterRole::Shopping)
			{
				plan.oldCore = center.position;
				break;
			}
		}
	}
	double intensity(const UrbanMorphology::Plan& plan, Vec2 point)
	{
		const auto& settings = profile(plan.structure);
		double peak = 0;
		for (const auto& center : plan.centers)
		{
			const double distance = point.distanceFrom(center.position) / Max(1.0, center.radius);
			peak = Max(peak, std::exp(-distance * distance));
		}
		const double edge = Max(Abs(point.x) / plan.halfExtent.x, Abs(point.y) / plan.halfExtent.y);
		return Max(peak, settings.backgroundIntensity) *
			   Clamp((settings.edgeFadeEnd - edge) / settings.edgeFadeWidth, 0.0, 1.0);
	}
	UrbanMorphology::LandUse sample(const UrbanMorphology::Plan& plan, Vec2 point)
	{
		using namespace UrbanMorphology;
		const auto& settings = profile(plan.structure);
		const double dense = intensity(plan, point);
		for (const auto& center : plan.centers)
		{
			if (point.distanceFrom(center.position) < center.radius * settings.coreRadiusRatio)
			{
				return {center.role == CenterRole::Business ? District::Station : District::OldTown, Generation::Modern,
					1, settings.coreFrontage};
			}
		}
		// 商業地は拠点を結ぶ実際の直交街路の沿道へ。全面を商業用途にはしない。
		if (plan.station)
		{
			for (const auto& center : plan.centers)
			{
				const Vec2 corner{center.position.x, plan.station->y};
				const double distance =
					Min(segmentDistance(point, *plan.station, corner), segmentDistance(point, corner, center.position));
				if (distance < GenerationSettings::get().settlements_stationStreetRadius)
				{
					return {District::OldTown, Generation::Railway, 1, settings.coreFrontage};
				}
			}
		}
		if (plan.industry.w > 0 && plan.industry.contains(point))
		{
			return {District::Industry, Generation::Modern, GenerationSettings::get().settlements_industryOccupancy,
				GenerationSettings::get().settlements_industryFrontage};
		}
		return {District::Housing, Generation::Modern, Math::Lerp(settings.outerOccupancy, 1.0, dense),
			Math::Lerp(settings.outerFrontage, settings.coreFrontage, dense)};
	}
	Array<float> streetCoordinates(const UrbanMorphology::Plan& plan, bool crossAxis)
	{
		const auto& settings = profile(plan.structure);
		const double extent = crossAxis ? plan.halfExtent.y : plan.halfExtent.x;
		const double spacing = crossAxis ? settings.spacingZ : settings.spacingX;
		const int count = Max(4, static_cast<int>(std::round(extent / spacing)) * 2);
		Array<float> coordinates;
		for (int i = 0; i <= count; ++i)
		{
			const double t = 2.0 * i / count - 1;
			double position=extent*((1-settings.cubicWeight)*t+settings.cubicWeight*t*t*t);
			// Historic street blocks vary in width while the outer boundary and main axis stay fixed.
			if (plan.structure!=Type::PlannedGrid && i>0 && i<count && i!=count/2)
			{
				const double phase=static_cast<double>(plan.salt%1009u)*.013+(crossAxis ? 1.63 : 0.0);
				const double irregular=Sin(i*1.91+phase)*.65+Sin(i*.83+phase*.37)*.35;
				position+=Min(spacing*.13,12.0)*irregular*(1.0-Abs(t)*.35);
			}
			coordinates << static_cast<float>(position);
		}
		return coordinates;
	}
	GeneratedStreet::Role streetRole(const UrbanMorphology::Plan& plan, const Array<float>& x, const Array<float>& z,
		int colA, int rowA, int colB, int rowB)
	{
		using Role = GeneratedStreet::Role;
		const auto& settings = profile(plan.structure);
		const bool along = rowA == rowB;
		const auto& cross = along ? z : x;
		const int corridor = along ? rowA : colA;
		const int middle = static_cast<int>(cross.size() / 2);
		const Vec2 point{(x[colA] + x[colB]) * .5, (z[rowA] + z[rowB]) * .5};
		if (corridor == middle && (along || settings.crossBoulevard))
		{
			return Role::MainArterial;
		}
		if (settings.ringRatio > 0)
		{
			const int low = nearestIndex(cross, -cross.back() * settings.ringRatio),
					  high = nearestIndex(cross, cross.back() * settings.ringRatio);
			const double length = along ? Abs(point.x) / plan.halfExtent.x : Abs(point.y) / plan.halfExtent.y;
			if ((corridor == low || corridor == high) && length <= settings.ringRatio)
			{
				return Role::Collector;
			}
		}
		for (const auto& center : plan.centers)
		{
			const double desired = along ? center.position.y : center.position.x;
			if (corridor == nearestIndex(cross, desired))
			{
				return Role::Collector;
			}
		}
		if (Abs(corridor - middle) % settings.collectorEvery == 0)
		{
			return Role::Collector;
		}
		return Role::Local;
	}
	bool allowStreet(const UrbanMorphology::Plan& plan, const Array<float>& x, const Array<float>& z, int colA,
		int rowA, int colB, int rowB)
	{
		const auto& settings = profile(plan.structure);
		if (settings.staggerEvery == 0) { return true; }
		if (streetRole(plan, x, z, colA, rowA, colB, rowB) != GeneratedStreet::Role::Local) { return true; }
		if (rowA != rowB)
		{
			const int upper=Min(rowA,rowB);
			if (colA==0 || colA+1==static_cast<int>(x.size()) || upper==0 || upper+2>=static_cast<int>(z.size())) { return true; }
			// Occasional missing local links form T-junctions without cutting collectors.
			return (plan.salt+static_cast<uint64>(colA*17+upper*11))%7u!=0u;
		}
		if (rowA==0 || rowA+1==static_cast<int>(z.size())) { return true; }
		return (rowA%2==0 || colA%settings.staggerEvery!=(rowA/2)%settings.staggerEvery);
	}
	void adaptBuilding(Building& building, const UrbanMorphology::Plan& plan, Vec2 local, uint32 roll)
	{
		if (plan.structure == Type::None || building.type == BuildingType::None)
		{
			return;
		}
		const auto use = UrbanMorphology::sample(plan, local);
		const auto& settings = profile(plan.structure);
		const double choice = roll * .01;
		if (use.district == UrbanMorphology::District::Station || use.district == UrbanMorphology::District::OldTown)
		{
			const double peak = intensity(plan, local);
			const double high = settings.coreHighShare * peak;
			double officeShare = settings.coreOfficeShare;
			for (const auto& center : plan.centers)
			{
				if (center.role == CenterRole::Shopping &&
					local.distanceFrom(center.position) < center.radius * settings.coreRadiusRatio)
				{
					officeShare *= settings.shoppingOfficeRatio;
					break;
				}
			}
			const double office = high + officeShare * peak;
			const double mid = office + settings.coreMidShare;
			building.type = choice < high
								? BuildingType::HighApartment
								: (choice < office ? BuildingType::Office
												   : (choice < mid ? BuildingType::MidApartment : BuildingType::Shop));
			for (const auto& center : plan.centers)
			{
				if (center.role == CenterRole::Waterfront &&
					local.distanceFrom(center.position) < center.radius * settings.coreRadiusRatio)
				{
					building.type =
						choice < settings.coreOfficeShare ? BuildingType::PublicFacility : BuildingType::Shop;
				}
			}
		}
		else if (use.district == UrbanMorphology::District::Housing ||
				 use.district == UrbanMorphology::District::PlannedHousing)
		{
			const double detached =
				Math::Lerp(settings.outerDetachedShare, settings.innerDetachedShare, intensity(plan, local));
			building.type = choice < detached ? BuildingType::Detached
											  : (choice < detached + (1 - detached) * settings.housingMidShare
														? BuildingType::MidApartment
														: BuildingType::LowApartment);
		}
	}
	Array<Vec2> stationPositions(const UrbanMorphology::Plan& plan)
	{
		Array<Vec2> positions;
		if (plan.structure != Type::None)
		{
			for (const auto& center : plan.centers)
			{
				if (center.rail)
				{
					positions << center.position;
				}
			}
		}
		else if (plan.station)
		{
			positions << *plan.station;
		}
		return positions;
	}
	JSON saveLayout(const UrbanMorphology::Plan& plan)
	{
		JSON result;
		result[U"type"] = String{id(plan.structure)};
		result[U"centers"] = Array<JSON>{};
		result[U"parks"] = Array<JSON>{};
		for (const auto& center : plan.centers)
		{
			JSON row;
			row[U"x"] = center.position.x;
			row[U"z"] = center.position.y;
			row[U"radius"] = center.radius;
			row[U"role"] = static_cast<int>(center.role);
			row[U"rail"] = center.rail;
			result[U"centers"].push_back(row);
		}
		for (const auto& park : plan.neighborhoodParks)
		{
			JSON points = Array<JSON>{};
			for (const auto& point : park.outer())
			{
				JSON row;
				row[U"x"] = point.x;
				row[U"z"] = point.y;
				points.push_back(row);
			}
			result[U"parks"].push_back(points);
		}
		return result;
	}
	void restoreLayout(UrbanMorphology::Plan& plan, const JSON& state)
	{
		plan.structure = fromId(state[U"type"].getOr<String>(U"none"));
		plan.centers.clear();
		plan.neighborhoodParks.clear();
		for (const auto item : state[U"centers"])
		{
			const auto row = item.value;
			plan.centers << Center{{row[U"x"].get<double>(), row[U"z"].get<double>()}, row[U"radius"].get<double>(),
				static_cast<CenterRole>(row[U"role"].get<int>()), row[U"rail"].get<bool>()};
		}
		for (const auto item : state[U"parks"])
		{
			Array<Vec2> points;
			for (const auto row : item.value)
			{
				points << Vec2{row.value[U"x"].get<double>(), row.value[U"z"].get<double>()};
			}
			if (points.size() >= 3)
			{
				plan.neighborhoodParks << Polygon{points};
			}
		}
	}
} // namespace UrbanStructure
