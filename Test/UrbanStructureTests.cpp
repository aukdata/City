#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/SettlementPlan.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/SettlementPlacement.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/RailwayAlignment.hpp"
#include "src/world/ZoneGrid.hpp"
#include "src/gen/ParcelGeometry.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"

namespace
{
	/// @brief 資料別の型を同一条件で比較し、地形や乱数の違いと混同しない。
	struct Fixture
	{
		FilePath directory = FileSystem::CurrentDirectory();
		Fixture() { FileSystem::ChangeCurrentDirectory(directory + U"../../App/"); }
		~Fixture()
		{
			FileSystem::ChangeCurrentDirectory(directory);
			DebugLog::shutdown();
		}
	};
	void installFlat(World& world)
	{
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		for (int z = 30; z <= 33; ++z)
			for (int x = 30; x <= 33; ++x)
			{
				world.installChunkDirect(
					{x, z}, HeightMapResult{Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20), 20, 20});
			}
	}
} // namespace

void registerUrbanStructureTests(TestRunner& runner)
{
	using namespace UrbanStructure;
	runner.add(U"UrbanStructure.SelectionAndSettings", [](TestContext& context)
	{
		HashSet<int> selected;
		for (uint64 seed = 0; seed < 512; ++seed)
		{
			for (const double relief : {0.0, 45.0, 600.0})
			{
				UrbanMorphology::Site inland;
				inland.relief = relief;
				const auto chosen = choose(inland, seed);
				selected.insert(static_cast<int>(chosen));
				context.expect(chosen == choose(inland, seed), U"City structure selection is deterministic");
				context.expect(chosen != Type::CoastalHubs, U"Waterfront cities require a nearby shore");
				if (relief == 600)
				{
					context.expect(chosen == Type::ConstrainedLinear || chosen == Type::RegionalHub,
						U"Steep sites cannot choose an unrestricted metropolitan or planned grid");
				}
				inland.shoreDistance = 900;
				selected.insert(static_cast<int>(choose(inland, seed)));
			}
		}
		context.expectEqual(
			selected.size(), 7, U"All seven structures occur on suitable terrain across deterministic seeds");
		const JSON source = JSON::Load(U"../../App/assets/generation/urbanStructures.json");
		const auto verify = [&](StringView key, const JSON& value, bool expected)
		{
			JSON changed = source;
			changed[U"historic_grid"][key] = value;
			changed.save(U"TestResults/urban_structure_invalid.json");
			bool loaded = false;
			try
			{
				load(U"TestResults/urban_structure_invalid.json");
				loaded = true;
			}
			catch (const Error&)
			{
			}
			context.expect(loaded == expected, U"Profile validation: " + String{key});
		};
		verify(U"spacingX", JSON(91.0), true);
		verify(U"spacingX", JSON(-1.0), false);
		verify(U"collectorEvery", JSON(2.5), false);
		verify(U"coreHighShare", JSON(1.0), false);
		verify(U"unknown", JSON(1), false);
		verify(U"centers", JSON(Array<JSON>{}), false);
		const Array<SettlementPlacement::Candidate> sites{{{9000, 9000}, 1}};
		const auto towns =
			SettlementPlacement::generate(42, sites, RectF{0, 0, 18000, 18000}, [](Vec2) { return 20.0; });
		context.expect(!towns.isEmpty() && towns.front().plan.structure != Type::None,
			U"The production settlement selector assigns a modern structure automatically");
		for (const auto& town : towns)
		{
			if (town.kind != MapGenerator::SettlementKind::RegionalCity)
			{
				context.expect(
					town.plan.structure == Type::None, U"Villages and new towns retain their separate morphology");
			}
		}
	});
	runner.add(U"UrbanStructure.CentersAndPersistence", [](TestContext& context)
	{
		for (int index = 1; index <= 7; ++index)
		{
			const auto type = static_cast<Type>(index);
			auto plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle, 0, {}, 42, true);
			apply(plan, type);
			const double oldRadius = plan.centers.front().radius;
			UrbanMorphology::rescale(plan, .72);
			alignCenters(plan);
			context.expectNear(plan.centers.front().radius, oldRadius * .72, .001,
				U"All center influence areas shrink with the terrain fit");
			const JSON saved = saveLayout(plan);
			auto restored = plan;
			restored.centers.clear();
			restored.structure = Type::None;
			restored.neighborhoodParks.clear();
			restoreLayout(restored, saved);
			context.expect(restored.structure == type && restored.centers.size() == plan.centers.size(),
				U"Urban type and center count survive saving");
			context.expectEqual(restored.neighborhoodParks.size(), plan.neighborhoodParks.size(),
				U"Public green reservations survive saving");
			for (size_t i = 0; i < plan.centers.size(); ++i)
			{
				context.expectNear(restored.centers[i].position.distanceFrom(plan.centers[i].position), 0, .001,
					U"Saved centers retain exact positions");
				context.expectNear(intensity(restored, plan.centers[i].position),
					intensity(plan, plan.centers[i].position), .0001, U"Restored urban density is unchanged");
			}
			const auto positions = stationPositions(plan);
			for (const auto point : positions)
			{
				const auto x = UrbanMorphology::streetCoordinates(plan, false),
						   z = UrbanMorphology::streetCoordinates(plan, true);
				for (const double street : x)
				{
					context.expect(
						Abs(point.x - street) > 10, U"Station center stays inside a block after terrain fitting");
				}
				for (const double street : z)
				{
					context.expect(Abs(point.y - street) > 10, U"Station center does not move onto a cross street");
				}
			}
			if (type == Type::TransitCorridor)
			{
				context.expect(
					intensity(plan, plan.centers.front().position) > intensity(plan, {0, plan.halfExtent.y * .8}) + .6,
					U"Transit centers have distinct density peaks above the outer suburbs");
			}
		}
	});
	runner.add(U"UrbanStructure.TerrainAndOrigins", [](TestContext& context)
	{
		for (int type = 1; type <= 7; ++type)
			for (const auto origin : {UrbanMorphology::Origin::Castle, UrbanMorphology::Origin::Port,
					 UrbanMorphology::Origin::Industrial, UrbanMorphology::Origin::Temple})
			{
				UrbanMorphology::Site site;
				site.shoreDistance = 900;
				auto plan = UrbanMorphology::makePlan(origin, 0, site, 130, true);
				apply(plan, static_cast<Type>(type));
				context.expect(
					plan.origin == origin, U"Modern form does not overwrite the settlement's historical origin");
				for (const auto& center : plan.centers)
				{
					const auto use = UrbanMorphology::sample(plan, center.position);
					context.expect(use.district == UrbanMorphology::District::Station ||
									   use.district == UrbanMorphology::District::OldTown,
						U"Historic civic and industrial rectangles cannot erase modern centers: {} origin={} point={}"_fmt(
							type, static_cast<int>(origin), center.position));
				}
			}
		Fixture fixture;
		for (const bool coast : {true, false})
		{
			World world;
			world.reserveChunks();
			world.setGenerationParams(130, WORLD_SIZE, WORLD_SIZE);
			for (int z = 30; z <= 33; ++z)
				for (int x = 30; x <= 33; ++x)
				{
					Grid<float> heights(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 200);
					for (int row = 0; row <= HEIGHT_CELLS; ++row)
						for (int col = 0; col <= HEIGHT_CELLS; ++col)
						{
							const double wx = x * CHUNK_SIZE + col * 16.0;
							heights[{col, row}] =
								static_cast<float>(coast ? (wx > 33360 ? -5 : 200) : 200 + (wx - 32768) * .045);
						}
					world.installChunkDirect({x, z}, HeightMapResult{heights, -5, 300});
				}
			MapGenerator::Settlement town;
			town.center = {32768, 32768};
			town.kind = MapGenerator::SettlementKind::RegionalCity;
			town.plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle, 0, {}, 130, true);
			apply(town.plan, coast ? Type::CoastalHubs : Type::ConstrainedLinear);
			const double initialDepth = town.plan.halfExtent.y;
			town.gridAxisX = {0, -1};
			town.gridAxisZ = {1, 0};
			RoadNetwork roads;
			const Vec3 a{32768, 200, 30968}, b{32768, 200, 34568};
			roads.addEdge(
				roads.addNode(a), roads.addNode(b), a.lerp(b, 1.0 / 3), a.lerp(b, 2.0 / 3), RoadType::Arterial, 2);
			DistrictRoads::generateSettlement(
				130, 0, town, DistrictRoads::extractKaido(town, roads, 2000), world, roads);
			context.expect(
				!town.plan.frontageRoads, U"Feasible coastal and contour-oriented districts retain their urban plan");
			if (coast)
				context.expect(
					town.plan.halfExtent.y < initialDepth, U"Coastal city shrinks to dry land before replacing roads");
			context.expectNear(
				Abs(town.gridAxisX.x), 0, .001, U"Coastal and linear cities retain the shore or contour axis");
			for (const auto& edge : roads.edges())
				if (edge.id >= 0)
				{
					context.expect(!edge.useElevation, U"Terrain fitting never raises the whole city onto viaducts");
					const auto curve = roads.getBezier(edge.id);
					for (float t = 0; t <= 1; t += .1f)
					{
						const auto point = curve->evaluate(t);
						context.expect(
							world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.z)) > 3.2,
							U"Urban roads stay on dry terrain after shrinking the plan");
					}
				}
		}
	});
	for (int index = 1; index <= 7; ++index)
	{
		const auto type = static_cast<Type>(index);
		runner.add(U"UrbanStructure.Generated." + String{id(type)}, [type](TestContext& context)
		{
			Fixture fixture;
			RegisterAssets();
			DebugLog::initialize(fixture.directory + U"TestResults/urban_structure_" + id(type) + U".log");
			World world;
			installFlat(world);
			MapGenerator::Settlement town;
			town.center = {32768, 32768};
			town.kind = MapGenerator::SettlementKind::RegionalCity;
			town.name = U"検証市";
			town.plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle, 0, {}, 42, true);
			apply(town.plan, type);
			RoadNetwork roads;
			const Vec3 from{30968, 20, 32768}, to{34568, 20, 32768};
			roads.addEdge(roads.addNode(from), roads.addNode(to), from.lerp(to, 1.0 / 3), from.lerp(to, 2.0 / 3),
				RoadType::Arterial, 2);
			const auto kaido = DistrictRoads::extractKaido(town, roads, 2000);
			DistrictRoads::generateSettlement(42, 0, town, kaido, world, roads);
			HashSet<int> reached;
			Array<int> pending;
			for (const auto& edge : roads.edges())
			{
				if (edge.id >= 0 && edge.hasRoadLanes())
				{
					reached.insert(edge.nodeA);
					pending << edge.nodeA;
					break;
				}
			}
			for (size_t i = 0; i < pending.size(); ++i)
			{
				for (const auto& attachment : roads.getNode(pending[i])->attachments)
				{
					const auto* edge = roads.getEdge(attachment.edgeId);
					if (!edge->hasRoadLanes())
						continue;
					const int other = edge->nodeA == pending[i] ? edge->nodeB : edge->nodeA;
					if (reached.insert(other).second)
					{
						pending << other;
					}
				}
			}
			int streetCount = 0, junctions = 0, threeWay = 0, arterials = 0;
			for (const auto& edge : roads.edges())
				if (edge.id >= 0 && edge.hasRoadLanes())
				{
					++streetCount;
					arterials += edge.roadType == RoadType::Arterial;
					context.expect(reached.contains(edge.nodeA) && reached.contains(edge.nodeB),
						U"Every generated street connects to the regional network");
					context.expect(!edge.useElevation, U"Flat city streets stay on the ground");
				}
			for (const auto& node : roads.nodes())
				if (node.id >= 0)
				{
					junctions += node.attachments.size() >= 3;
					threeWay += node.attachments.size() == 3;
				}
			TrainNetwork trains;
			// 実際の共通軌道・駅を生成し、建物は完成した線路も避けて配置する。
			RailwayAlignment::generate(trains, world, {town}, &roads);
			const Array<MapGenerator::Settlement> towns{town};
			SettlementDevelopment actualDevelopment{world, roads, trains, towns, 42};
			actualDevelopment.applyZonesGlobal();
			const auto validation = actualDevelopment.placeInitialBuildings(true);
			int buildings = 0, high = 0, mid = 0, detached = 0, offices = 0, railEdges = 0;
			double area = 0;
			for (int z = 30; z <= 33; ++z)
				for (int x = 30; x <= 33; ++x)
				{
					const auto& chunk = *world.getChunk({x, z});
					for (int row = 0; row < ZONE_CELLS; ++row)
						for (int col = 0; col < ZONE_CELLS; ++col)
						{
							const auto& building = chunk.buildingGrid[{col, row}];
							if (building.type == BuildingType::None || building.type == BuildingType::Farmland)
								continue;
							const Vec2 point =
								ZoneGrid::cellCenterXZ({x, z}, col, row) + Vec2{building.offsetX, building.offsetZ};
							const Vec2 delta = point - town.center,
									   local{delta.dot(town.gridAxisX), delta.dot(town.gridAxisZ)};
							if (!UrbanMorphology::inCore(town.plan, local))
								continue;
							++buildings;
							area += Square(buildingFootprintXZ(building.type));
							high += building.type == BuildingType::HighApartment;
							mid += building.type == BuildingType::MidApartment;
							detached += building.type == BuildingType::Detached;
							offices += building.type == BuildingType::Office;
							context.expect(roads.getEdge(building.edgeId) != nullptr,
								U"Every building retains real road frontage");
							for (const Vec2 corner : ParcelGeometry::footprint(
									 point, buildingFootprintXZ(building.type) * .5, building.angle))
							{
								const Vec2 deltaCorner = corner - town.center;
								context.expect(!UrbanMorphology::isReservedGreen(town.plan,
												   {deltaCorner.dot(town.gridAxisX), deltaCorner.dot(town.gridAxisZ)}),
									U"Entire building footprints leave the civic green axis clear");
							}
						}
				}
			for (const auto& edge : roads.edges())
				if (edge.id >= 0 && edge.hasRailLanes())
					++railEdges;
			JSON report;
			report[U"type"] = String{id(type)};
			report[U"buildings"] = buildings;
			report[U"high"] = high;
			report[U"mid"] = mid;
			report[U"detached"] = detached;
			report[U"offices"] = offices;
			report[U"footprintArea"] = area;
			report[U"roads"] = streetCount;
			report[U"arterials"] = arterials;
			report[U"threeWay"] = threeWay;
			report[U"junctions"] = junctions;
			int actualStations = 0;
			for (const auto& node : trains.nodes())
			{
				actualStations += node.id >= 0 && node.type == TrackNodeType::Station;
			}
			report[U"stations"] = actualStations;
			report[U"railEdges"] = railEdges;
			report[U"validation"] = validation.summary;
			report[U"valid"] = validation.passed;
			context.expect(buildings > 600, U"Each city type develops substantial occupied street frontage");
			context.expect(validation.passed, U"Production parcel and access constraints: " + validation.summary);
			if (stationPositions(town.plan).size() > 1)
			{
				context.expect(railEdges > 0,
					U"Multiple planned stations create a usable railway under real alignment constraints");
				context.expectEqual(
					actualStations, stationPositions(town.plan).size(), U"All stations connect on the flat fixture");
			}
			if (type == Type::HistoricGrid)
				context.expectEqual(
					high + offices, 0, U"Historic fine-grid districts preserve their low and medium skyline");
			if (type == Type::Metropolitan)
				context.expect(high > 40, U"Metropolitan subcenters produce an identifiable high-rise skyline");
			if (type == Type::PlannedGrid || type == Type::Metropolitan || type == Type::HistoricGrid)
			{
				WorldRenderer renderer;
				RoadRenderer roadRenderer;
				renderer.setAsyncTerrain(false);
				renderer.preloadBuildingModels();
				roadRenderer.loadAssets();
				world.update({32768, 20, 32768});
				const Size size{1024, 768};
				const BasicCamera3D camera{size, 45_deg, {32768, 1500, 31168}, {32768, 20, 32768}};
				const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
				for (int frame = 0; frame < 10; ++frame)
				{
					const ScopedRenderTarget3D output{target.clear(ColorF{.03, .1, .3})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera);
					Graphics3D::SetSunDirection(Vec3{1, 2, -1}.normalized());
					Graphics3D::SetGlobalAmbientColor(ColorF{.55});
					renderer.render(world, roads, camera);
					roadRenderer.render(roads, world, ViewFrustum{camera, 10000}, camera.getEyePosition());
					Graphics3D::Flush();
				}
				Image pixels;
				target.readAsImage(pixels);
				int green = 0, dark = 0;
				HashSet<uint32> colors;
				for (const auto color : pixels)
				{
					green += color.g > color.r * 1.08 && color.g > color.b * 1.08;
					dark += color.r < 5 && color.g < 5 && color.b < 5;
					colors.insert((color.r / 16) * 256 + (color.g / 16) * 16 + color.b / 16);
				}
				report[U"renderedBuildings"] = renderer.buildingsSubmitted();
				report[U"colors"] = colors.size();
				report[U"greenPixels"] = green;
				context.expect(renderer.buildingsSubmitted() > 100 && colors.size() > 30 && dark < 1024 * 768 / 20,
					U"Generated skyline and materials render through the production renderer");
				pixels.save(fixture.directory + U"Screenshot/urban_structure_" + id(type) + U".png");
			}
			report.save(fixture.directory + U"TestResults/urban_structure_" + id(type) + U".json");
		});
	}
}
