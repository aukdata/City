#include "UrbanFacilities.hpp"
#include "ParcelRoadIndex.hpp"
#include "../world/ZoneGrid.hpp"
#include "../debug/DebugLog.hpp"
#include "../railway/RailTimetable.hpp"
#include "RoadAlignment.hpp"

namespace UrbanFacilities
{
	namespace
	{
		struct Site
		{
			Vec2 center;
			double angle;
			double radius;
			ParcelGeometry::Quad footprint;
		};
		struct Candidate
		{
			Building building;
			Vec2 center;
			double score;
		};
		Vec2 local(const MapGenerator::Settlement& town, Vec2 point)
		{
			const Vec2 delta = point - town.center;
			return {delta.dot(town.gridAxisX), delta.dot(town.gridAxisZ)};
		}
		Vec2 position(const MapGenerator::Settlement& town, Vec2 point)
		{
			return town.center + town.gridAxisX * point.x + town.gridAxisZ * point.y;
		}
		bool reserved(const MapGenerator::Settlement& town, const ParcelGeometry::Quad& footprint)
		{
			Array<Vec2> points;
			for (const auto corner : footprint)
			{
				const Vec2 point = local(town, corner);
				if (UrbanMorphology::isReservedGreen(town.plan, point))
				{
					return true;
				}
				points << point;
			}
			const Polygon polygon{points};
			for (const auto& park : town.plan.neighborhoodParks)
			{
				if (polygon.intersects(park))
				{
					return true;
				}
			}
			return false;
		}
		int towerCount(UrbanStructure::Type type)
		{
			const auto& settings = GenerationSettings::get();
			switch (type)
			{
			case UrbanStructure::Type::Metropolitan:
				return settings.landmarks_towerCountMetropolitan;
			case UrbanStructure::Type::CoastalHubs:
				return settings.landmarks_towerCountCoastal;
			case UrbanStructure::Type::HistoricGrid:
			case UrbanStructure::Type::ConstrainedLinear:
				return 0;
			default:
				return settings.landmarks_towerCountRegional;
			}
		}
	} // namespace
	Result generate(World& world, const RoadNetwork& roads, const TrainNetwork& trains,
		const Array<MapGenerator::Settlement>& districts, uint64 seed)
	{
		Result result;
		const auto& settings = GenerationSettings::get();
		ParcelRoadIndex roadSpace{roads, true};
		roadSpace.addRailway(trains);
		Array<Site> occupied;
		// 中心セルが遠い大規模敷地も、物理的な外周で除外する。
		for (int z = 0; z < WORLD_CHUNKS; ++z)
		{
			for (int x = 0; x < WORLD_CHUNKS; ++x)
			{
				const auto* chunk = world.getChunk({x, z});
				if (!chunk)
				{
					continue;
				}
				for (int row = 0; row < ZONE_CELLS; ++row)
				{
					for (int col = 0; col < ZONE_CELLS; ++col)
					{
						const auto& building = chunk->buildingGrid[{col, row}];
						if (building.type == BuildingType::None || building.type == BuildingType::Farmland)
						{
							continue;
						}
						const Vec2 center =
							ZoneGrid::cellCenterXZ({x, z}, col, row) + Vec2{building.offsetX, building.offsetZ};
						const double half = buildingFootprintXZ(building.type) * .5 + .5;
						occupied << Site{center, building.angle, half * 1.415,
							ParcelGeometry::footprint(center, half, building.angle)};
					}
				}
			}
		}
		for (const auto& town : districts)
		{
			const bool satelliteTown=town.plan.origin==UrbanMorphology::Origin::Planned && town.plan.scale==1;
			if ((town.plan.scale != 0 && !satelliteTown) || !town.plan.ready)
			{
				continue;
			}
			const Vec2 civic = town.plan.civic ? town.plan.civic->center() : town.plan.oldCore;
			const std::array<std::pair<BuildingType, int>, 5> requests{
				{{BuildingType::CityHall, satelliteTown ? 0 : settings.landmarks_cityHallCount},
					{BuildingType::ShoppingMall, satelliteTown ? 0 : settings.landmarks_mallCount},
					{BuildingType::Hospital, satelliteTown ? 0 : settings.landmarks_hospitalCount},
					{BuildingType::School, satelliteTown ? 1 : settings.landmarks_schoolCount},
					{BuildingType::OfficeTower, satelliteTown ? 0 : towerCount(town.plan.structure)}}};
			for (const auto [type, wanted] : requests)
			{
				if (wanted <= 0)
				{
					continue;
				}
				Array<Candidate> candidates;
				Array<Vec2> selected;
				const double half = buildingFootprintXZ(type) * .5;
				for (const auto& edge : roads.edges())
				{
					if (edge.id < 0 || !edge.isRoadbedBuilt() || !edge.hasRoadLanes() || edge.useElevation ||
						edge.tunnel || (edge.roadType != RoadType::LocalRoad && edge.roadType != RoadType::Arterial))
					{
						continue;
					}
					if (type == BuildingType::ShoppingMall &&
						(edge.roadType != RoadType::Arterial ||
							edge.totalWidth() < settings.landmarks_mallMinimumRoadWidth))
					{
						continue;
					}
					const auto curve = roads.getBezier(edge.id);
					if (!curve)
					{
						continue;
					}
					const double start = edge.cutoffA + half + settings.landmarks_setback;
					const double end = edge.length - edge.cutoffB - half - settings.landmarks_setback;
					for (double arc = start; arc <= end; arc += settings.landmarks_siteSampleSpacing)
					{
						const Vec3 point = curve->positionAt(static_cast<float>(arc));
						const Vec3 right3 = tangentToRight(curve->tangentAt(static_cast<float>(arc)));
						const Vec2 right{right3.x, right3.z};
						for (const int sign : {-1, 1})
						{
							const Vec2 normal = right * sign;
							const Vec2 center = Vec2{point.x, point.z} +
												normal * (edge.totalWidth() * .5 + half + settings.landmarks_setback);
							const Vec2 p = local(town, center);
							const double radius =
								Max(Abs(p.x) / town.plan.halfExtent.x, Abs(p.y) / town.plan.halfExtent.y);
							const auto use = UrbanMorphology::sample(town.plan, p);
							const double intensity = UrbanMorphology::downtownIntensity(town.plan, p);
							if (type == BuildingType::ShoppingMall)
							{
								if (radius < settings.landmarks_mallMinimumRadius ||
									radius > settings.landmarks_mallMaximumRadius)
								{
									continue;
								}
							}
							else if (radius > .94 || use.district == UrbanMorphology::District::Industry)
							{
								continue;
							}
							if (type == BuildingType::OfficeTower &&
								(intensity < settings.landmarks_towerMinimumIntensity ||
									use.district == UrbanMorphology::District::Civic))
							{
								continue;
							}
							if (type == BuildingType::School && (intensity > .72 || radius < .32))
							{
								continue;
							}
							double score = (type == BuildingType::CityHall || (satelliteTown && type == BuildingType::School))
											   ? p.distanceFrom(civic)
											   : p.distanceFrom(Vec2{0, town.plan.halfExtent.y * .60});
							if (type == BuildingType::ShoppingMall)
							{
								score = Abs(radius - 1.18) * 1000;
							}
							if (type == BuildingType::OfficeTower)
							{
								score = (1 - intensity) * 1000;
							}
							score += static_cast<double>(UrbanMorphology::mix(seed + static_cast<uint64>(edge.id) * 31 +
																			  static_cast<uint64>(arc)) %
														 100) *
									 .01;
							Building building;
							building.type = type;
							building.edgeId = edge.id;
							building.edgeT = curve->tFromArcLength(static_cast<float>(arc));
							building.angle = static_cast<float>(Atan2(-normal.x, normal.y));
							candidates << Candidate{building, center, score};
						}
					}
				}
				candidates.stable_sort_by([](const Candidate& a, const Candidate& b) { return a.score < b.score; });
				for (auto candidate : candidates)
				{
					if (static_cast<int>(selected.size()) >= wanted)
					{
						break;
					}
					const auto footprint =
						ParcelGeometry::footprint(candidate.center, half + .5, candidate.building.angle);
					const double separation = type == BuildingType::School ? settings.landmarks_schoolSeparation
																		   : settings.landmarks_siteSeparation;
					if (selected.any([&](Vec2 point) { return point.distanceFrom(candidate.center) < separation; }))
					{
						continue;
					}
					if (roadSpace.overlaps(footprint) ||
						districts.any([&](const auto& other) { return reserved(other, footprint); }) ||
						occupied.any([&](const Site& site)
					{
						return site.center.distanceFrom(candidate.center) < site.radius + half * 1.415 &&
							   ParcelGeometry::overlaps(site.footprint, footprint);
					}))
					{
						++result.rejected;
						continue;
					}
					// 四隅だけでは見落とす池や小さな丘も、敷地内部を走査して避ける。
					bool dry = true;
					double low = Math::Inf, high = -Math::Inf;
					const int count =
						Max(1, static_cast<int>(Ceil(half * 2 / settings.landmarks_terrainSampleSpacing)));
					for (int row = 0; row <= count; ++row)
					{
						for (int col = 0; col <= count; ++col)
						{
							const Vec2 point = footprint[0] +
											   (footprint[1] - footprint[0]) * (static_cast<double>(col) / count) +
											   (footprint[3] - footprint[0]) * (static_cast<double>(row) / count);
							const auto* chunk = world.getChunk(
								{static_cast<int>(point.x) / CHUNK_SIZE, static_cast<int>(point.y) / CHUNK_SIZE});
							if (!chunk)
							{
								dry = false;
								continue;
							}
							const double height =
								world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.y));
							dry &= height >
								   world.waterSurfaceHeight(point.x, point.y) + settings.development_buildingFreeboard;
							low = Min(low, height);
							high = Max(high, height);
						}
					}
					if (!dry || high - low > settings.landmarks_maximumRelief)
					{
						++result.rejected;
						continue;
					}
					Point coord;
					int col = 0, row = 0;
					ZoneGrid::worldToZoneCell(static_cast<float>(candidate.center.x),
						static_cast<float>(candidate.center.y), coord, col, row);
					auto* chunk = world.getChunk(coord);
					if (!chunk || chunk->buildingGrid[{col, row}].type != BuildingType::None)
					{
						continue;
					}
					const Vec2 offset = candidate.center - ZoneGrid::cellCenterXZ(coord, col, row);
					candidate.building.offsetX = static_cast<float>(offset.x);
					candidate.building.offsetZ = static_cast<float>(offset.y);
					chunk->buildingGrid[{col, row}] = candidate.building;
					chunk->zoneMap[{col, row}] =
						(type == BuildingType::OfficeTower || type == BuildingType::ShoppingMall)
							? ZoneType::Commercial
							: ZoneType::UrbanControl;
					chunk->meshDirty = true;
					selected << candidate.center;
					occupied << Site{candidate.center, candidate.building.angle, (half + .5) * 1.415, footprint};
					++result.placed;
				}
				DBG_LOG(U"[UrbanFacility] town={} type={} requested={} placed={} candidates={}"_fmt(
					town.name, static_cast<int>(type), wanted, selected.size(), candidates.size()));
			}
		}
		return result;
	}
	void generateSubways(World& world, TrainNetwork& trains, const Array<MapGenerator::Settlement>& districts)
	{
		const auto& settings = GenerationSettings::get();
		for (const auto& town : districts)
		{
			if (town.plan.scale != 0 || (town.plan.structure != UrbanStructure::Type::Metropolitan &&
											town.plan.structure != UrbanStructure::Type::CoastalHubs))
			{
				continue;
			}
			const auto hubs = UrbanStructure::stationPositions(town.plan);
			const double z = hubs.isEmpty() ? 0 : hubs.front().y;
			double start = -Min(settings.landmarks_subwayHalfLength, town.plan.halfExtent.x * .72), end = -start;
			for (const Vec2 hub : hubs)
			{
				start = Min(start, hub.x);
				end = Max(end, hub.x);
			}
			const Vec2 a = position(town, {start, z + settings.landmarks_subwayOffset}),
					   b = position(town, {end, z + settings.landmarks_subwayOffset});
			if (a.distanceFrom(b) < settings.landmarks_subwayMinimumLength)
			{
				continue;
			}
			double low = Math::Inf, high = -Math::Inf;
			bool valid = true;
			const int samples =
				Max(1, static_cast<int>(Ceil(a.distanceFrom(b) / settings.landmarks_terrainSampleSpacing)));
			for (int i = 0; i <= samples; ++i)
			{
				for (const double side : {-9.0, 0.0, 9.0})
				{
					const Vec2 point = a.lerp(b, static_cast<double>(i) / samples) + town.gridAxisZ * side;
					if (!world.getChunk(
							{static_cast<int>(point.x) / CHUNK_SIZE, static_cast<int>(point.y) / CHUNK_SIZE}))
					{
						valid = false;
						continue;
					}
					const double h = world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.y));
					low = Min(low, h);
					high = Max(high, h);
				}
			}
			// 都心下の短い地下線は、全断面で土被りを満たす最高の水平縦断を選ぶ。
			const double elevation = low - settings.landmarks_subwayDepth;
			if (!valid || high - elevation > settings.landmarks_subwayMaximumDepth)
			{
				continue;
			}
			const int stops = a.distanceFrom(b) >= settings.landmarks_subwayMinimumLength * 2 ? 3 : 2;
			Array<Vec3> points, entrances;
			ParcelRoadIndex streetSpace{trains.infrastructure(), true};
			streetSpace.addRailway(trains);
			for (int i = 0; i < stops; ++i)
			{
				const Vec2 center = a.lerp(b, static_cast<double>(i) / (stops - 1));
				points << Vec3{center.x, elevation, center.y};
				Optional<Vec3> entrance;
				double nearest = Math::Inf;
				for (const auto& road : trains.infrastructure().edges())
				{
					if (road.id < 0 || !road.hasRoadLanes() || !road.isRoadbedBuilt() || road.useElevation ||
						road.tunnel || road.roadType == RoadType::Expressway)
					{
						continue;
					}
					const auto curve = trains.infrastructure().getBezier(road.id);
					if (!curve)
					{
						continue;
					}
					const Vec3 middle = curve->positionAt(curve->totalLength * .5f);
					if (Vec2{middle.x, middle.z}.distanceFrom(center) > curve->totalLength * .5 + 100)
					{
						continue;
					}
					for (float arc = road.cutoffA + 12; arc < road.length - road.cutoffB - 12; arc += 12)
					{
						const Vec3 point = curve->positionAt(arc), right = tangentToRight(curve->tangentAt(arc));
						for (const int sign : {-1, 1})
						{
							Vec3 candidate = point + right * sign * (road.totalWidth() * .5 + 7);
							const Vec2 flat{candidate.x, candidate.z};
							const double distance = flat.distanceFrom(center);
							if (distance > 100 || distance >= nearest ||
								streetSpace.overlaps(
									ParcelGeometry::footprint(flat, 5, Atan2(town.gridAxisX.y, town.gridAxisX.x))))
							{
								continue;
							}
							candidate.y =
								world.sampleHeight(static_cast<float>(flat.x), static_cast<float>(flat.y)) + .04;
							if (candidate.y < world.waterSurfaceHeight(flat.x, flat.y) + 1 ||
								UrbanMorphology::isReservedGreen(town.plan, local(town, flat)))
							{
								continue;
							}
							nearest = distance;
							entrance = candidate;
						}
					}
				}
				if (!entrance)
				{
					valid = false;
					break;
				}
				entrances << *entrance;
			}
			if (!valid)
			{
				DBG_LOG(U"[SubwayOmitted] town={} reason=no-surface-access"_fmt(town.name));
				continue;
			}
			Array<int> ids;
			for (int i = 0; i < stops; ++i)
			{
				const int id = trains.addStation(points[i], U"{}地下{}"_fmt(town.name, i + 1));
				auto* node = trains.getNode(id);
				node->stationKind = StationKind::Underground;
				node->entrance = entrances[i];
				ids << id;
			}
			for (int i = 1; i < stops; ++i)
			{
				const Vec3 delta = points[i] - points[i - 1];
				const int id = trains.addEdge(
					ids[i - 1], ids[i], points[i - 1] + delta / 3, points[i - 1] + delta * 2 / 3, 70, true);
				auto* edge = trains.getEdge(id);
				TransportCrossSection::railway(*edge, true, true);
				edge->tunnel = true;
				edge->useElevation = true;
			}
			auto schedule = RailTimetable::makeDefault(trains, ids.front(), ids.back());
			schedule.name = town.name + U"地下線";
			schedule.headwaySec = static_cast<float>(settings.landmarks_subwayHeadwayMinutes * 60);
			schedule.stops.clear();
			for (const int id : ids)
			{
				schedule.stops << StopEntry{id, 30};
			}
			trains.addSchedule(schedule);
			DBG_LOG(U"[Subway] town={} stations={} length={:.0f} depth={:.1f}"_fmt(
				town.name, ids.size(), a.distanceFrom(b), high - elevation));
		}
	}
} // namespace UrbanFacilities
