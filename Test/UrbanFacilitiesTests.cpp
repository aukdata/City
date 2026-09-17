#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/UrbanFacilities.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/RailwayAlignment.hpp"
#include "src/render/ModelLod.hpp"
#include "src/render/RailFacilities.hpp"
#include "src/render/TunnelGeometry.hpp"
#include "src/render/TunnelRenderer.hpp"
#include "src/pedestrian/PedestrianNetwork.hpp"
#include "src/railway/TrainManager.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/world/ZoneGrid.hpp"

namespace
{
	struct AppDirectory
	{
		FilePath previous = FileSystem::CurrentDirectory();
		AppDirectory()
		{
			FileSystem::ChangeCurrentDirectory(previous + U"../../App/");
			RegisterAssets();
		}
		~AppDirectory()
		{
			FileSystem::ChangeCurrentDirectory(previous);
			DebugLog::shutdown();
		}
	};
} // namespace
void registerUrbanFacilitiesTests(TestRunner& runner)
{
	runner.add(U"UrbanFacilities.ModelsAndGpu", [](TestContext& context)
	{
		AppDirectory directory;
		JSON report;
		const Size size{640, 480};
		const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		for (const auto type : {BuildingType::OfficeTower, BuildingType::CityHall, BuildingType::ShoppingMall,
				 BuildingType::Hospital, BuildingType::School})
		{
			const int variants = type == BuildingType::OfficeTower ? 2 : 1;
			for (int variant = 0; variant < variants; ++variant)
			{
				String stem;
				context.expect(tryGetBuildingModelStemForVariant(type, static_cast<uint8>(variant), stem),
					U"Dedicated building type resolves its model");
				const FilePath source = U"assets/buildings/commercial/" + stem + U".obj";
				Model original{source};
				context.expect(!original.isEmpty(), U"New facility loads: " + stem);
				if (original.isEmpty())
				{
					continue;
				}
				const auto bounds = original.boundingBox();
				const double radius = bounds.size.length();
				context.expect(Max(bounds.size.x, bounds.size.z) <= buildingFootprintXZ(type) + .01,
					U"Complete campus stays inside reserved lot");
				context.expect(bounds.size.y <= buildingHeight(type), U"Height budget does not shrink authored models");
				for (int level = 0; level < 3; ++level)
				{
					Model model{modelLodPath(source, level)};
					context.expect(!model.isEmpty(), U"All three LOD levels load");
					if (model.isEmpty())
					{
						continue;
					}
					context.expectNear(
						model.boundingBox().size.distanceFrom(bounds.size), 0, .15, U"LOD retains silhouette extents");
					Model::RegisterDiffuseTextures(model, TextureDesc::MippedSRGB);
					const BasicCamera3D camera{
						size, 40_deg, bounds.center + Vec3{radius * .8, radius * .5, -radius}, bounds.center};
					{
						const ScopedRenderTarget3D output{target.clear(ColorF{.06, .13, .24})};
						const ScopedRenderStates3D states{
							DepthStencilState::DepthTestWrite, RasterizerState::SolidCullBack};
						Graphics3D::SetCameraTransform(camera);
						Graphics3D::SetSunDirection(Vec3{1, 2, -1}.normalized());
						Graphics3D::SetGlobalAmbientColor(ColorF{.45});
						model.draw();
					}
					Graphics3D::Flush();
					Image pixels;
					target.readAsImage(pixels);
					int visible = 0;
					HashSet<uint32> colors;
					for (const auto color : pixels)
					{
						visible += color != pixels[0][0];
						colors.insert((color.r / 16) * 256 + (color.g / 16) * 16 + color.b / 16);
					}
					context.expect(visible > 4000 && colors.size() > 8,
						U"Campus, facade and materials survive GPU back-face culling");
					report[stem][U"lod{}"_fmt(level)] = visible;
					if (level == 0)
					{
						pixels.save(directory.previous + U"Screenshot/" + stem + U".png");
					}
				}
			}
		}
		report.save(directory.previous + U"TestResults/urban_facility_models.json");
	});
	runner.add(U"UrbanFacilities.GeneratedAccess", [](TestContext& context)
	{
		AppDirectory directory;
		World world;
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		for (int z = 30; z <= 33; ++z)
		{
			for (int x = 30; x <= 33; ++x)
			{
				world.installChunkDirect(
					{x, z}, HeightMapResult{Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20), 20, 20});
			}
		}
		MapGenerator::Settlement town;
		town.center = {32768, 32768};
		town.kind = MapGenerator::SettlementKind::RegionalCity;
		town.name = U"施設試験市";
		town.plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle, 0, {}, 42, true);
		UrbanStructure::apply(town.plan, UrbanStructure::Type::Metropolitan);
		RoadNetwork roads;
		const Vec3 from{30900, 20, 32768}, to{34600, 20, 32768};
		roads.addEdge(roads.addNode(from), roads.addNode(to), from.lerp(to, 1.0 / 3), from.lerp(to, 2.0 / 3),
			RoadType::Arterial, 2);
		DistrictRoads::generateSettlement(42, 0, town, DistrictRoads::extractKaido(town, roads, 2000), world, roads);
		for (const auto& road : roads.edges())
		{
			if (road.id >= 0)
			{
				roads.getEdge(road.id)->edgeState = EdgeState::Open;
			}
		}
		DebugLog::initialize(directory.previous + U"TestResults/urban_facilities.log");
		TrainNetwork trains;
		RailwayAlignment::generate(trains, world, {town}, &roads);
		const Array<MapGenerator::Settlement> towns{town};
		SettlementDevelopment development{world, roads, trains, towns, 42};
		development.applyZonesGlobal();
		const auto validation = development.placeInitialBuildings(true);
		context.expect(validation.passed, U"Full generation retains valid parcels/frontage: " + validation.summary);
		SimGraph graph = SimGraph::build(roads);
		BuildingAccessIndex access;
		access.rebuild(world, roads, graph);
		HashTable<int, int> counts;
		JSON report;
		for (int z = 30; z <= 33; ++z)
		{
			for (int x = 30; x <= 33; ++x)
			{
				const auto& chunk = *world.getChunk({x, z});
				for (int row = 0; row < ZONE_CELLS; ++row)
				{
					for (int col = 0; col < ZONE_CELLS; ++col)
					{
						const auto& building = chunk.buildingGrid[{col, row}];
						if (building.type < BuildingType::OfficeTower)
						{
							continue;
						}
						++counts[static_cast<int>(building.type)];
						context.expect(BuildingAccessIndex::project(chunk, {col, row}, roads, graph).has_value(),
							U"New large facilities are real vehicle trip endpoints");
						const Vec2 center =
							ZoneGrid::cellCenterXZ({x, z}, col, row) + Vec2{building.offsetX, building.offsetZ};
						if (building.type == BuildingType::ShoppingMall)
						{
							context.expect(!UrbanMorphology::inCore(town.plan, center - town.center),
								U"Regional mall belongs outside the urban core");
							context.expect(roads.getEdge(building.edgeId)->roadType == RoadType::Arterial,
								U"Mall has arterial frontage");
						}
					}
				}
			}
		}
		for (const auto type : {BuildingType::OfficeTower, BuildingType::CityHall, BuildingType::ShoppingMall,
				 BuildingType::Hospital, BuildingType::School})
		{
			context.expect(counts[static_cast<int>(type)] > 0,
				U"Every requested facility is produced on suitable streets: " + Format(static_cast<int>(type)));
			report[U"buildings"][Format(static_cast<int>(type))] = counts[static_cast<int>(type)];
		}
		PedestrianNetwork walking;
		walking.rebuild(world, roads, access, trains);
		int underground = 0, terminal = 0;
		const Size imageSize{640, 480};
		const RenderTexture facilityTarget{imageSize, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		TunnelRenderer tunnels;
		tunnels.build(world, roads, trains);
		for (const auto& station : trains.nodes())
		{
			if (station.type != TrackNodeType::Station || station.stationKind == StationKind::Local)
			{
				continue;
			}
			const auto frame = RailwaySite::stationFrame(trains, station.id);
			if (!frame)
			{
				continue;
			}
			const bool subway = station.stationKind == StationKind::Underground;
			const Vec3 center = subway ? *station.entrance : frame->point(0, 5, 25);
			const double radius = subway ? 15 : 75;
			const BasicCamera3D camera{imageSize, 45_deg,
				center + frame->right * radius + Vec3{0, radius * .7, 0} - frame->along * radius, center};
			const auto geometry = RailFacilities::station(trains, world, station.id);
			{
				const ScopedRenderTarget3D output{facilityTarget.clear(ColorF{.06, .13, .24})};
				const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite, RasterizerState::SolidCullBack};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(Vec3{1, 2, -1}.normalized());
				Graphics3D::SetGlobalAmbientColor(ColorF{.45});
				for (size_t material = 0; material < RailFacilities::Count; ++material)
				{
					if (!geometry.parts[material].indices.isEmpty())
					{
						Mesh{geometry.parts[material]}.draw(RailFacilities::color(material));
					}
				}
			}
			Graphics3D::Flush();
			Image pixels;
			facilityTarget.readAsImage(pixels);
			int visible = 0;
			for (const auto color : pixels)
			{
				visible += color != pixels[0][0];
			}
			context.expect(
				visible > 4000, U"Terminal concourse and underground entrance render with front-face culling");
			report[U"stationPixels"][Format(station.id)] = visible;
			pixels.save(directory.previous + U"Screenshot/facility_station_" + Format(station.id) + U".png");
			if (subway)
			{
				context.expect(tunnels.openings.any(
								   [&](const auto& opening)
				{
					return opening.bounds.contains(Vec2{station.entrance->x, station.entrance->z}) &&
						   opening.floor > station.position.y + 10;
				}),
					U"Entrance stairs cut a small actual opening in surface terrain");
			}
		}
		for (const auto& station : trains.nodes())
		{
			if (station.type != TrackNodeType::Station)
			{
				continue;
			}
			terminal += station.stationKind == StationKind::Terminal;
			if (station.stationKind != StationKind::Underground)
			{
				continue;
			}
			++underground;
			context.expect(station.entrance.has_value() && station.entrance->y - station.position.y > 12,
				U"Underground station has a separate surface entrance");
			context.expect(walking.stationIndex(station.id).has_value(),
				U"Underground platforms connect to the pedestrian network");
			context.expect(RailwaySite::stationMinimumRadius(trains, station.id) >= RailwaySite::kStationMinimumRadius,
				U"Station platforms remain straight");
			const auto geometry = RailFacilities::station(trains, world, station.id);
			context.expect(!geometry.parts[RailFacilities::Glass].indices.isEmpty() && !geometry.signs.isEmpty(),
				U"Underground entrance has a roof, lift and signage");
		}
		context.expect(underground >= 2 && terminal >= 1,
			U"Automatic generation includes terminal and operating underground stations");
		for (const auto& schedule : trains.schedules())
		{
			context.expect(RailTimetable::validate(trains, schedule).isEmpty(),
				U"Every generated default timetable is editable and valid");
		}
		TrainNetwork restored;
		restored.bind(&roads);
		context.expect(restored.restoreState(trains.saveState()), U"Facility kinds and entrances survive storage");
		context.expect(restored.saveState().formatMinimum() == trains.saveState().formatMinimum(),
			U"Station metadata round-trips exactly");
		TrainManager manager;
		manager.init(&trains);
		bool moved = false;
		for (int i = 0; i < 400; ++i)
		{
			manager.update(.1, 8 * 3600 + i * .1);
			for (const auto& train : manager.trains())
			{
				moved |= train.position.y < 10 && train.arcPos > 10;
			}
		}
		context.expect(moved, U"Default subway services actually depart and travel below ground");
		report[U"undergroundStations"] = underground;
		report[U"terminalStations"] = terminal;
		report[U"valid"] = validation.passed;
		report[U"subwayMoving"] = moved;
		report.save(directory.previous + U"TestResults/urban_facility_generation.json");
	});
}
