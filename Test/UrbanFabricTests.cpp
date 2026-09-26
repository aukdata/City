#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/ParcelGeometry.hpp"
#include "src/world/ZoneGrid.hpp"
#include "src/debug/DebugLog.hpp"
#include "src/gen/SettlementPlacement.hpp"
#include "src/pedestrian/PedestrianNetwork.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"

void registerUrbanFabricTests(TestRunner& runner)
{
	runner.add(U"UrbanFabric.GeneratedDistricts", [](TestContext& context)
	{
		const FilePath directory = FileSystem::CurrentDirectory();
		struct Restore
		{
			FilePath directory;
			~Restore()
			{
				FileSystem::ChangeCurrentDirectory(directory);
				DebugLog::shutdown();
			}
		} restore{directory};
		FileSystem::ChangeCurrentDirectory(directory + U"../../App/");
		DebugLog::initialize(directory + U"TestResults/urban_fabric.log");
		RegisterAssets();
		JSON report;
		for (const auto origin : {UrbanMorphology::Origin::Castle, UrbanMorphology::Origin::Planned})
		{
			Array<uint64> seeds{42, 130};
			if (origin == UrbanMorphology::Origin::Planned)
			{
				seeds << 2026;
			}
			for (const uint64 seed : seeds)
			{
				World world;
				world.reserveChunks();
				world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
				for (int z = 30; z <= 33; ++z)
				{
					for (int x = 30; x <= 33; ++x)
					{
						Grid<float> heights(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20);
						if (seed == 2026)
						{
							for (int row = 0; row <= HEIGHT_CELLS; ++row)
							{
								for (int col = 0; col <= HEIGHT_CELLS; ++col)
								{
									heights[{col, row}] =
										static_cast<float>(80 + (x * CHUNK_SIZE + col * 16 - 32768) * .015 +
														   (z * CHUNK_SIZE + row * 16 - 32768) * .006);
								}
							}
						}
						world.installChunkDirect({x, z}, HeightMapResult{heights, 20, 120});
					}
				}
				MapGenerator::Settlement town;
				town.center = {32768, 32768};
				town.kind =
					seed == 2026 ? MapGenerator::SettlementKind::LocalTown : MapGenerator::SettlementKind::RegionalCity;
				town.plan = UrbanMorphology::makePlan(
					origin, static_cast<uint8>(town.kind), UrbanMorphology::Site{}, seed, true);
				RoadNetwork roads;
				const Vec3 from{31168, seed == 2026 ? 56.0 : 20.0, 32768},
					to{34368, seed == 2026 ? 104.0 : 20.0, 32768};
				roads.addEdge(roads.addNode(from), roads.addNode(to), from.lerp(to, 1.0 / 3), from.lerp(to, 2.0 / 3),
					RoadType::Arterial, 2);
				const auto kaido = DistrictRoads::extractKaido(town, roads, 1800);
				DistrictRoads::generateSettlement(seed, 0, town, kaido, world, roads);
				if (origin == UrbanMorphology::Origin::Planned)
				{
					context.expect(
						town.plan.greenways.size() > 10, U"New towns build a continuous network of greenways");
					context.expect(!town.plan.neighborhoodParks.isEmpty(),U"Blocks without car frontage become planned parks instead of failed housing sites");
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
							{
								continue;
							}
							const int other = edge->nodeA == pending[i] ? edge->nodeB : edge->nodeA;
							if (reached.insert(other).second)
							{
								pending << other;
							}
						}
					}
					for (const auto& edge : roads.edges())
					{
						if (edge.id >= 0 && edge.hasRoadLanes())
						{
							context.expect(reached.contains(edge.nodeA) && reached.contains(edge.nodeB),
								U"All car roads stay connected after separating greenways");
						}
					}
					for (const auto& node : roads.nodes())
					{
						if (node.id < 0)
						{
							continue;
						}
						for (const auto& turn : node.laneConnections)
						{
							const auto* fromEdge = roads.getEdge(turn.fromEdgeId);
							const auto* toEdge = roads.getEdge(turn.toEdgeId);
							context.expect(fromEdge && toEdge &&
											   turn.fromLaneIndex < static_cast<int>(fromEdge->lanes.size()) &&
											   turn.toLaneIndex < static_cast<int>(toEdge->lanes.size()),
								U"No vehicle turn references a removed greenway lane");
						}
					}
					BuildingAccessIndex access;
					PedestrianNetwork walking;
					const TrainNetwork trainsForWalking;
					walking.rebuild(world, roads, access, trainsForWalking);
					Optional<int> firstWalk;
					for (const auto& path : town.plan.greenways)
					{
						const Vec2 point = town.center + town.gridAxisX * ((path.begin.x + path.end.x) * .5) +
										   town.gridAxisZ * ((path.begin.y + path.end.y) * .5);
						const auto node = walking.nearestNode({point.x, world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.y)), point.y}, 40);
						context.expect(node.has_value(), U"Each greenway belongs to the live walking network");
						if (node && firstWalk)
						{
							context.expect(walking.nodes()[*node].component == walking.nodes()[*firstWalk].component,
								U"Greenways are connected to each other through real sidewalks");
						}
						if (node && !firstWalk)
						{
							firstWalk = node;
						}
					}
				}
				const Array<MapGenerator::Settlement> towns{town};
				TrainNetwork trains;
				SettlementDevelopment development{world, roads, trains, towns, seed};
				development.applyZonesGlobal();
				const auto validation = development.placeInitialBuildings(true);
				int count = 0, apartments = 0, detached = 0, elevated = 0;
				double footprintArea = 0, parcelArea = 0, setbacks = 0;
				int clippedBuildingCorners = 0;
				HashTable<int64, ParcelGeometry::Quad> coreFootprints;
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
								if (building.type == BuildingType::None || building.type == BuildingType::Farmland)
								{
									continue;
								}
								const Vec2 center =
									ZoneGrid::cellCenterXZ({x, z}, col, row) + Vec2{building.offsetX, building.offsetZ};
								if (origin==UrbanMorphology::Origin::Planned)
								{
									for (const Vec2 corner:ParcelGeometry::footprint(center,buildingFootprintXZ(building.type)*.5,building.angle))
									{
										const Vec2 offset=corner-town.center;
										context.expect(!UrbanMorphology::isReservedGreen(town.plan,{offset.dot(town.gridAxisX),offset.dot(town.gridAxisZ)}),U"Final building footprints leave the greenway reservation clear");
									}
								}
								const Vec2 delta = center - town.center;
								const Vec2 local{delta.dot(town.gridAxisX), delta.dot(town.gridAxisZ)};
								if (Abs(local.x) > town.plan.halfExtent.x * .7 ||
									Abs(local.y) > town.plan.halfExtent.y * .7)
								{
									continue;
								}
								++count;
								footprintArea += Square(buildingFootprintXZ(building.type));
								apartments += building.type == BuildingType::MidApartment ||
											  building.type == BuildingType::LowApartment;
								detached += building.type == BuildingType::Detached;
								coreFootprints[ZoneGrid::zoneCellKey({x, z}, col, row)] =
									ParcelGeometry::footprint(center, buildingFootprintXZ(building.type) * .5, building.angle);
								const auto* edge = roads.getEdge(building.edgeId);
								context.expect(edge != nullptr, U"All buildings have road access");
								if (edge)
								{
									const Vec3 p = roads.getBezier(edge->id)->evaluate(building.edgeT);
									setbacks += center.distanceFrom(Vec2{p.x, p.z}) - edge->totalWidth() * .5 -
												buildingFootprintXZ(building.type) * .5;
								}
							}
						}
					}
				}
				for (int z = 30; z <= 33; ++z)
				{
					for (int x = 30; x <= 33; ++x)
					{
						for (const auto& patch : world.getChunk({x, z})->landPatches)
						{
							if (const auto footprint = coreFootprints.find(patch.sourceParcelKey); footprint != coreFootprints.end())
							{
								const Polygon parcel{patch.polygon};
								parcelArea += parcel.area();
								for (const Vec2 corner : footprint->second)
								{
									clippedBuildingCorners += !parcel.contains(corner);
								}
							}
						}
					}
				}
				for (const auto& edge : roads.edges())
				{
					elevated += edge.id >= 0 && edge.useElevation;
				}
				JSON item;
				item[U"origin"] = static_cast<int>(origin);
				item[U"scale"] = static_cast<int>(town.kind);
				item[U"seed"] = seed;
				item[U"buildings"] = count;
				item[U"apartments"] = apartments;
				item[U"detached"] = detached;
				item[U"elevated"] = elevated;
				item[U"footprintArea"] = footprintArea;
				item[U"parcelArea"] = parcelArea;
				item[U"coverage"] = footprintArea / Max(1.0, parcelArea);
				item[U"clippedBuildingCorners"] = clippedBuildingCorners;
				item[U"meanSetback"] = setbacks / Max(1, count);
				item[U"validation"] = validation.summary;
				if (seed == 42 || seed == 2026)
				{
					// 本体の建物・敷地・樹木・道路を GPU で描画する。画像はローカル保存のみ。
					WorldRenderer renderer;
					RoadRenderer roadRenderer;
					renderer.setAsyncTerrain(false);
					renderer.preloadBuildingModels();
					context.expect(roadRenderer.loadAssets(), U"Production street assets load");
					world.update({32768, 20, 32768});
					const Size size{1024, 768};
					const BasicCamera3D camera{size, 45_deg,
						{32768, seed == 2026 ? 650.0 : 1150.0, seed == 2026 ? 32018.0 : 31468.0},
						{32768, seed == 2026 ? 80.0 : 20.0, 32768}};
					const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
					for (int frame = 0; frame < 10; ++frame)
					{
						const ScopedRenderTarget3D renderTarget{target.clear(ColorF{.03, .1, .3})};
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
					item[U"renderedBuildings"] = renderer.buildingsSubmitted();
					item[U"renderedTriangles"] = renderer.buildingTriangles();
					item[U"greenPixels"] = green;
					item[U"quantizedColors"] = colors.size();
					context.expect(renderer.buildingsSubmitted() > 100 && renderer.buildingTriangles() > 10000,
						U"Production renderer submits the generated city models");
					context.expect(colors.size() > 30 && dark < 1024 * 768 / 20,
						U"District image contains lit geometry and materials rather than a missing or black surface");
					if (origin == UrbanMorphology::Origin::Planned)
					{
						context.expect(
							green > 5000, U"Greenways and neighborhood parks remain visible in the rendered new town");
					}
					pixels.save(directory + U"Screenshot/urban_fabric_{}_{}.png"_fmt(
												static_cast<int>(origin), static_cast<int>(town.kind)));
				}
				report.push_back(item);
				if (origin == UrbanMorphology::Origin::Castle)
				{
					context.expect(count >= 3400 && footprintArea > 480000,
						U"The central district retains its building count while filling more ground area");
					context.expect(footprintArea / Max(1.0, parcelArea) > .84,
						U"Central parcels closely follow the building footprint");
					context.expectEqual(clippedBuildingCorners, 0,
						U"Compact parcels still contain every corner of their building");
				}
				context.expect(count > 100, U"The central district develops a substantial street frontage");
				context.expect(validation.passed, U"Real generation constraints: " + validation.summary);
				context.expectEqual(elevated, 0, U"Flat districts remain at ground level");
			}
		}
		report.save(directory + U"TestResults/urban_fabric.json");
	});
	runner.add(U"UrbanFabric.NewTownSites", [](TestContext& context)
	{
		using namespace UrbanMorphology;
		using namespace SettlementPlacement;
		Array<Candidate> candidates{{{9000, 9000}, 1}};
		for (double z = 2400; z <= 15600; z += 480)
		{
			for (double x = 2400; x <= 15600; x += 480)
			{
				candidates << Candidate{{x, z}, .70f};
			}
		}
		int generated = 0;
		for (const uint64 seed : {42ULL, 130ULL, 7ULL, 2026ULL})
		{
			const auto flat = [](Vec2) { return 20.0; };
			const auto towns = generate(seed, candidates, RectF{0, 0, 18000, 18000}, flat);
			const auto repeated = generate(seed, candidates, RectF{0, 0, 18000, 18000}, flat);
			context.expectEqual(
				towns.size(), repeated.size(), U"Seed and terrain fully determine settlement placement");
			for (size_t index = 0; index < towns.size(); ++index)
			{
				const auto& town = towns[index];
				context.expect(town.center == repeated[index].center && town.plan.origin == repeated[index].plan.origin,
					U"New-town site selection is repeatable");
				if (town.plan.origin != Origin::Planned)
				{
					continue;
				}
				++generated;
				context.expect(town.kind == MapGenerator::SettlementKind::LocalTown,
					U"New towns retain neighborhood scale rather than creating another city center");
				context.expect(town.center.distanceFrom(towns.front().center) >= 3200 &&
								   town.center.distanceFrom(towns.front().center) <= 6500,
					U"New towns are satellite residential districts around an existing city");
				context.expect(RectF{1000, 1000, 16000, 16000}.contains(town.center - town.plan.halfExtent) &&
								   RectF{1000, 1000, 16000, 16000}.contains(town.center + town.plan.halfExtent),
					U"The whole planned site respects the one-kilometer map-edge exclusion");
			}
		}
		context.expect(generated >= 2, U"Suitable city hinterlands reliably generate new towns across seeds");
		const auto steep=generate(42,candidates,RectF{0,0,18000,18000},[](Vec2 p)
		{
			return 20.0+Max(0.0,p.distanceFrom(Vec2{9000,9000})-1800)*.2;
		});
		context.expect(!steep.any([](const auto& town) { return town.plan.origin==Origin::Planned; }),U"No new town is forced onto steep hills around a city");
		const auto wet = generate(42, candidates, RectF{0, 0, 18000, 18000},
			[](Vec2 p) { return p.distanceFrom(Vec2{9000, 9000}) < 1800 ? 20.0 : -2.0; });
		context.expect(!wet.any([](const auto& town) { return town.plan.origin == Origin::Planned; }),
			U"No new town is forced onto unsuitable or submerged land");
	});
}
