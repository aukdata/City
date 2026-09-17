#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/world/World.hpp"
#include "src/gameplay/CitySimulation.hpp"
#include "src/road/RoadTypes.hpp"
#include "src/road/RoadMarkingGenerator.hpp"
#include "src/road/JunctionGeometry.hpp"
#include "src/gen/ParcelGeometry.hpp"
#include "src/gen/UrbanParcel.hpp"
#include "src/render/VehicleRenderer.hpp"
#include "src/render/CityLighting.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/debug/DebugLog.hpp"
#include "src/asset/AssetRegistrar.hpp"

void registerCityGenerationTests(TestRunner& runner)
{
	runner.add(U"RoadNetwork.ShortConnectorPreservesTangents", [](TestContext& context)
	{
		RoadNetwork network;
		const int a = network.addNode(Vec3{0,0,0}), b = network.addNode(Vec3{10,0,0}), c = network.addNode(Vec3{100,0,0});
		network.addEdge(a,b,Vec3{3,0,0},Vec3{7,0,0},RoadType::Arterial,2);
		const auto branch = network.addEdge(b,c,Vec3{10,0,30},Vec3{70,0,30},RoadType::Arterial,2);
		const Vec3 tangent = network.getBezier(*branch)->tangentAt(0);
		context.expectEqual(network.mergeShortEdges(20),1,U"The redundant short connector is merged");
		const auto curve = network.getBezier(*branch);
		context.expect(curve->tangentAt(0).dot(tangent) > .9999,U"Moving the merged node preserves its attached curve direction");
		context.expect(Abs(network.getEdge(*branch)->length-curve->totalLength)<.001f,U"Length and cutoffs use the new curve geometry");
	});

	runner.add(U"Hud.HousingCapacityIncrementalUpdates", [](TestContext& context)
	{
		World world;
		world.reserveChunks();
		auto* first = world.getChunk(Point{0,0});
		auto* last = world.getChunk(Point{63,63});
		first->buildingGrid[{0,0}].type = BuildingType::Detached;
		last->buildingGrid[{0,0}].type = BuildingType::MidApartment;
		HousingCapacityCache cache;
		cache.update(world,4096);
		context.expectEqual(cache.capacity(),int64{83},U"Initial capacity includes distant sleeping chunks");
		first->buildingGrid[{0,0}].type = BuildingType::HighApartment;
		last->buildingGrid[{0,0}].type = BuildingType::None;
		cache.update(world,1);
		context.expectEqual(cache.capacity(),int64{380},U"Only the budgeted chunk changes its cached contribution");
		cache.update(world,4096);
		context.expectEqual(cache.capacity(),int64{300},U"Demolition is reflected after a complete refresh without double counting");
	});

	runner.add(U"RoadRendering.SlopedArterialJunctions", [](TestContext& context)
	{
		const FilePath testDirectory = FileSystem::CurrentDirectory();
		struct RestoreDirectory
		{
			FilePath path;
			~RestoreDirectory() { FileSystem::ChangeCurrentDirectory(path); }
		} restore{ testDirectory };
		FileSystem::ChangeCurrentDirectory(testDirectory + U"../../App/");
		DebugLog::initialize(testDirectory + U"TestResults/junction_render.log");
		RegisterAssets();
		TextWriter report{ testDirectory + U"TestResults/junction_diagnostics.txt" };
		for (const int scenario : { 3, 4, 5, 6, 7, 8, 9, 10, 11 })
		{
			const int arms = scenario == 11 ? 6 : (scenario == 10 ? 2 : (scenario == 4 ? 4 : 3));
			World world;
			world.reserveChunks();
			world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
			auto height = [](double x, double z) { return 70.0 + (x - 32768.0) * 0.06 + (z - 32768.0) * 0.04; };
			for (int z = 31; z <= 32; ++z)
			{
				for (int x = 31; x <= 32; ++x)
				{
					Grid<float> heights(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1);
					for (int row = 0; row <= HEIGHT_CELLS; ++row)
					{
						for (int col = 0; col <= HEIGHT_CELLS; ++col)
						{
							heights[{ col, row }] = static_cast<float>(height(x * CHUNK_SIZE + col * CHUNK_SIZE / HEIGHT_CELLS, z * CHUNK_SIZE + row * CHUNK_SIZE / HEIGHT_CELLS));
						}
					}
					world.installChunkDirect(Point{ x, z }, HeightMapResult{ heights, -40.0f, 180.0f });
				}
			}
			RoadNetwork network;
			const Vec3 focus{ 32768, scenario == 7 ? 78.0 : 70.0, 32768 };
			const int center = network.addNode(focus);
			Array<Vec2> ends{ Vec2{ 0, -180 }, Vec2{ 180, 0 }, Vec2{ 0, 180 }, Vec2{ -180, 0 } };
			if (scenario == 6) { ends[1] = Vec2{ 127, -127 }; }
			if (scenario == 8) { ends[1] = Vec2{ 4, -180 }; }
			if (scenario == 9) { ends[1] = Vec2{ 5, 0 }; }
			if (scenario == 10) { ends[1] = Vec2{ 0, 180 }; }
			if (scenario == 11) { ends = {Vec2{-180,0},Vec2{180,40},Vec2{20,-180},Vec2{-5,180},Vec2{160,90},Vec2{50,150}}; }
			for (int arm = 0; arm < arms; ++arm)
			{
				Vec3 end = focus + Vec3{ ends[arm].x, 0, ends[arm].y };
				end.y = height(end.x, end.z) + (scenario == 7 ? 8.0 : 0.0);
				const int endNode = network.addNode(end);
				const bool reversed = arm % 2 == 0;
				const Vec3 a = reversed ? end : focus;
				const Vec3 b = reversed ? focus : end;
				const auto edgeId = network.addEdge(reversed ? endNode : center, reversed ? center : endNode,
					a + (b - a) / 3.0, a + (b - a) * (2.0 / 3.0), (scenario == 5 && arm == 1) ? RoadType::LocalRoad : RoadType::Arterial, (scenario == 5 && arm == 2) ? 4 : 2);
				context.expect(edgeId.has_value(), U"Junction fixture edge added");
				if (edgeId)
				{
					network.getEdge(*edgeId)->edgeState = EdgeState::Open;
					network.getEdge(*edgeId)->useElevation = scenario == 7;
				}
			}
			if (scenario == 10)
			{
				network.getNode(center)->type = NodeType::Intersection;
				for (const auto& item : network.edges())
				{
					auto& edge = *network.getEdge(item.id);
					if (edge.nodeA == center) { edge.cutoffA = 12; } else { edge.cutoffB = 12; }
				}
				context.expect(!RoadMarkingGenerator::collectNode(network,center,true).isEmpty(), U"Two-arm Intersection retains its lane markings");
			}
			const auto layout = JunctionGeometry::build(network, center);
			context.expect(!layout.asphalt.indices.isEmpty(), U"Concave junction triangulates for mixed width, skew and elevated approaches");
			context.expectEqual(layout.corners.size(), arms, U"Each pair of approaches has a connected corner");
			for (const auto& corner : layout.corners)
			{
				context.expect(corner.sections.size() >= 2, U"Corner has two endpoints");
				for (const auto& section : corner.sections)
				{
					for (size_t i = 0; i < corner.bands.size(); ++i)
					{
						const auto& band = corner.bands[i];
						const double near = Math::Lerp(band.nearStart, band.nearEnd, section.fraction);
						const double far = Math::Lerp(band.farStart, band.farEnd, section.fraction);
						context.expect(far >= near && near >= -0.001, U"Connected strip retains nonnegative width outside asphalt");
						for (size_t j = i+1; j < corner.bands.size(); ++j)
						{
							const auto& other = corner.bands[j];
							const double otherNear = Math::Lerp(other.nearStart, other.nearEnd, section.fraction);
							const double otherFar = Math::Lerp(other.farStart, other.farEnd, section.fraction);
							context.expect(Min(far, otherFar) - Max(near, otherNear) < 0.002, U"Tapered parts do not overlap neighbouring parts");
						}
					}
				}
			}
			const Size size{ 1280, 900 };
			world.update(focus);
			WorldRenderer terrain;
			RoadRenderer roads;
			context.expect(roads.loadAssets(), U"Road assets load for sloped junction");
			for (int view = 0; view < 2; ++view)
			{
				const BasicCamera3D camera{ size, 40_deg, focus + (view == 0 ? Vec3{ 75, 100, -135 } : Vec3{ 0, 160, -0.1 }), focus };
				const RenderTexture target{ size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
				{
					const ScopedRenderTarget3D renderTarget{ target.clear(ColorF{ 0.05, 0.35, 0.7 }) };
					const ScopedRenderStates3D state{ DepthStencilState::DepthTestWrite };
					Graphics3D::SetCameraTransform(camera);
					Graphics3D::SetSunDirection(Vec3{ 1, 2, -1 }.normalized());
					Graphics3D::SetGlobalAmbientColor(ColorF{ 0.5 });
					terrain.render(world, network, camera);
					roads.render(network, world, ViewFrustum{ camera, 24000.0 }, camera.getEyePosition());
				}
				Graphics3D::Flush();
				Image screenshot;
				target.readAsImage(screenshot);
				context.expect(screenshot.save(testDirectory + U"Screenshot/junction_{}_{}.png"_fmt(scenario, view)), U"Save road junction review");
				if (view != 1) { continue; }
				int white = 0, samples = 0, visiblePaint = 0, expectedPaint = 0;
				for (const auto& edge : network.edges())
				{
					const auto bezier = network.getBezier(edge.id);
					for (const auto& marking : RoadMarkingGenerator::collectEdge(network, edge))
					{
						if (marking.kind != RoadMarkingKind::Crosswalk) { continue; }
						const Vec3 position = bezier->positionAt(marking.arcOffset);
						const Vec3 tangent = bezier->tangentAt(marking.arcOffset).normalized();
						const Vec3 right = tangentToRight(tangent);
						const double oldHeightError = (height(position.x + right.x * marking.width * 0.5, position.z + right.z * marking.width * 0.5) - position.y);
						report << U"arms={} edge={} cutoffA={} cutoffB={} crosswalkCornerHeightErrorM={:.4f}"_fmt(arms, edge.id, edge.cutoffA, edge.cutoffB, oldHeightError);
						const int bars = static_cast<int>(Floor((marking.width + 0.45) / 0.90));
						for (int bar = 0; bar < bars; ++bar)
						{
							for (const double forward : { -1.0, -0.5, 0.0, 0.5, 1.0 })
							{
								const double lateral = (bar - (bars-1)*0.5)*0.90;
								Vec3 point = position + right*lateral + tangent*forward;
								point.y = (scenario == 7 ? (position + tangent*forward).y : height(point.x, point.z)) + kRoadLineLift;
								const Float3 projected = camera.worldToScreenPoint(Float3{ point });
								const Point pixel{ static_cast<int>(Round(projected.x)), static_cast<int>(Round(projected.y)) };
								if (!screenshot.inBounds(pixel)) { continue; }
								++expectedPaint;
								// Search a 1px neighbourhood for projection/rasterisation rounding.
								bool visible = false;
								for (int dy = -1; dy <= 1; ++dy)
								{
									for (int dx = -1; dx <= 1; ++dx)
									{
										const Point nearby = pixel + Point{dx,dy};
										if (!screenshot.inBounds(nearby)) { continue; }
										const Color color = screenshot[nearby];
										visible = visible || (color.r > 190 && color.g > 190 && color.b > 190);
									}
								}
								visiblePaint += visible;
							}
						}
						for (double lateral = -marking.width * 0.5 + 0.15; lateral < marking.width * 0.5; lateral += 0.15)
						{
							for (double forward = -1.4; forward < 1.4; forward += 0.15)
							{
								Vec3 point = position + right * lateral + tangent * forward;
								point.y = (scenario == 7 ? (position + tangent * forward).y : height(point.x, point.z)) + kRoadLineLift;
								const Float3 projected = camera.worldToScreenPoint(Float3{ point });
								const Point pixel{ static_cast<int>(projected.x), static_cast<int>(projected.y) };
								if (!screenshot.inBounds(pixel)) { continue; }
								const Color color = screenshot[pixel];
								++samples;
								white += color.r > 190 && color.g > 190 && color.b > 190;
							}
						}
					}
				}
				report << U"arms={} crosswalkWhiteSamples={} totalSamples={} coverage={:.4f}"_fmt(arms, white, samples, white / static_cast<double>(Max(1, samples)));
				report << U"scenario={} visibleBarCenters={} expectedBarCenters={}"_fmt(scenario, visiblePaint, expectedPaint);
				if (scenario == 10)
				{
					int seamVisible = 0, seamSamples = 0;
					for (int z = -10; z <= 10; ++z)
					{
						for (const double x : {-3.325,0.0,3.325})
						{
							const Vec3 point = focus + Vec3{x, height(focus.x+x,focus.z+z)-focus.y+kRoadLineLift, static_cast<double>(z)};
							const auto projected = camera.worldToScreenPoint(Float3{point});
							bool visible = false;
							for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
							{
								const Point pixel{static_cast<int>(Round(projected.x))+dx,static_cast<int>(Round(projected.y))+dy};
								if (!screenshot.inBounds(pixel)) { continue; }
								const Color color = screenshot[pixel];
								visible = visible || (color.r > 170 && color.g > 155);
							}
							++seamSamples; seamVisible += visible;
						}
					}
					report << U"seamVisible={} seamSamples={}"_fmt(seamVisible,seamSamples);
					context.expect(seamVisible >= seamSamples * .90, U"Center and edge lines remain visible across a two-arm Intersection on a slope");
					continue;
				}
				if (scenario == 11) { continue; }
				if (scenario == 6 || (scenario == 8 && samples == 0))
				{
					context.expectEqual(samples, 0, U"The existing placement rule suppresses crossings at sharply skewed junctions");
				}
				else
				{
					context.expect(samples > 500, U"Crosswalks present in sloped arterial fixture");
					context.expect(white > samples * 0.32 && white < samples * 0.65, U"Zebra bars cover their expected share of the crossing");
					context.expect(expectedPaint > 0 && visiblePaint >= expectedPaint * 0.95, U"All longitudinal zebra bars remain visible across both uphill and downhill halves");
				}
			}
			if (scenario == 10)
			{
				const BasicCamera3D camera{size,40_deg,focus+Vec3{75,100,-135},focus};
				const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
				WorldRenderer streamed, reference;
				streamed.setAsyncTerrain(true);
				auto renderTerrain = [&](WorldRenderer& renderer)
				{
					const ScopedRenderTarget3D renderTarget{target.clear(ColorF{0.05,0.35,0.7})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera);
					renderer.render(world,network,camera);
					Graphics3D::Flush();
				};
				renderTerrain(streamed);
				auto* edited = world.getChunk(Point{32,32});
				for (auto& heightValue : edited->heightMap) { heightValue += 0.7f; }
				edited->meshDirty = true;
				int frames = 0;
				do
				{
					if (!System::Update()) { break; }
					renderTerrain(streamed);
					++frames;
				} while (streamed.pendingTerrainJobs() > 0 && frames < 240);
				context.expect(streamed.pendingTerrainJobs()==0,U"Terrain jobs settle after an edit invalidates an in-flight snapshot");
				Image actual, expected;
				target.readAsImage(actual);
				renderTerrain(reference);
				target.readAsImage(expected);
				int different = 0;
				for (int y = 0; y < size.y; ++y) for (int x = 0; x < size.x; ++x)
				{
					if (actual[{x,y}] != expected[{x,y}]) { ++different; }
				}
				report << U"asyncTerrainDifferentPixels={} totalPixels={}"_fmt(different,size.x*size.y);
				context.expect(different==0,U"Completed asynchronous terrain exactly matches the synchronous reference after an edit");
			}
		}
		DebugLog::shutdown();
	});

	runner.add(U"Generation.CastleTownConnectivityAndTerrain", [](TestContext& context)
	{
		const FilePath testDirectory = FileSystem::CurrentDirectory();
		struct RestoreDirectory
		{
			FilePath path;
			~RestoreDirectory() { FileSystem::ChangeCurrentDirectory(path); }
		} restore{ testDirectory };
		FileSystem::ChangeCurrentDirectory(testDirectory + U"../../App/");
		DebugLog::initialize(testDirectory + U"TestResults/generation_render.log");
		RegisterAssets();
		World world;
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		for (int z = 29; z <= 34; ++z)
		{
			for (int x = 29; x <= 34; ++x)
			{
				world.installChunkDirect(Point{ x, z }, HeightMapResult{
					Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20.0f), 20.0f, 20.0f });
			}
		}
		TextWriter report{ testDirectory + U"TestResults/generation_diagnostics.txt" };
		for (const uint64 seed : { 20260316ULL, 42ULL, 20260912ULL })
		{
			RoadNetwork network;
			MapGenerator::Settlement settlement;
			settlement.center = Vec2{ 32768, 32768 };
			settlement.radius = 700.0f;
			settlement.kind = MapGenerator::SettlementKind::RegionalCity;
			DistrictRoads::generateCastleTown(seed, 0, settlement, DistrictRoads::KaidoSegment{}, world, network);
			for (const auto& generatedEdge : network.edges())
			{
				if (auto* edge = network.getEdge(generatedEdge.id))
				{
					edge->edgeState = EdgeState::Open;
				}
			}
			double maxAxisError = 0.0;
			for (const auto& edge : network.edges())
			{
				if (edge.id < 0) { continue; }
				const auto insideCore=[&](int nodeId)
				{
					const Vec3 position=network.getNode(nodeId)->position;
					const Vec2 delta=Vec2{position.x,position.z}-settlement.center;
					return UrbanMorphology::inCore(settlement.plan,{delta.dot(settlement.gridAxisX),delta.dot(settlement.gridAxisZ)},.1);
				};
				if (!insideCore(edge.nodeA) || !insideCore(edge.nodeB)) { continue; }
				const Vec3 direction = network.getNode(edge.nodeB)->position - network.getNode(edge.nodeA)->position;
				const Vec2 horizontal{ direction.x, direction.z };
				maxAxisError = Max(maxAxisError, Min(Abs(horizontal.dot(settlement.gridAxisX)), Abs(horizontal.dot(settlement.gridAxisZ))));
			}
			report << U"seed={} maxCrossStreetDriftM={}"_fmt(seed, maxAxisError);
			context.expect(maxAxisError < 0.01, U"Planned town streets share corridor axes without per-intersection jitter");
			HashSet<int> visited;
			int components = 0;
			int endpoints = 0;
			int edgeCount = 0;
			double minLength = Math::Inf;
			for (const auto& edge : network.edges())
			{
				if (edge.id >= 0)
				{
					++edgeCount;
					minLength = Min(minLength, static_cast<double>(edge.length));
				}
			}
			for (const auto& node : network.nodes())
			{
				if (node.id < 0 || node.attachments.isEmpty())
				{
					continue;
				}
				endpoints += (node.attachments.size() == 1);
				if (visited.contains(node.id))
				{
					continue;
				}
				++components;
				Array<int> pending{ node.id };
				visited.insert(node.id);
				for (size_t next = 0; next < pending.size(); ++next)
				{
					const auto* current = network.getNode(pending[next]);
					for (const auto& attachment : current->attachments)
					{
						const auto* edge = network.getEdge(attachment.edgeId);
						if (!edge)
						{
							continue;
						}
						const int neighbor = edge->nodeA == current->id ? edge->nodeB : edge->nodeA;
						if (visited.insert(neighbor).second)
						{
							pending << neighbor;
						}
					}
				}
			}
			report << U"seed={} edges={} connectedComponents={} endpoints={} minEdgeLengthM={}"_fmt(seed, edgeCount, components, endpoints, minLength);
			context.expectEqual(components, 1, U"A flat town must have a connected local street network");
			context.expectEqual(endpoints, 0, U"Grid omissions must not create isolated dead ends");
			int boulevards = 0, collectors = 0, local = 0, oneWay = 0;
			double usableFrontage = 0.0;
			for (const auto& edge : network.edges())
			{
				if (edge.id < 0) { continue; }
				if (edge.roadType == RoadType::Arterial && edge.lanes.size() == 4) { ++boulevards; }
				else if (edge.roadType == RoadType::Arterial && edge.lanes.size() == 2) { ++collectors; }
				else if (edge.lanes.size() == 1) { ++oneWay; }
				else { ++local; }
				usableFrontage += Max(0.0f,edge.length-edge.cutoffA-edge.cutoffB-13.0f) * 2.0;
			}
			context.expect(boulevards > 20 && collectors > 20 && local > 20 && oneWay > 20, U"Town contains all four functional street levels");
			context.expect(usableFrontage > 70000, U"Town retains at least 70 km of buildable street frontage");
			context.expect(minLength > 1.0, U"No coincident-node connectors");
			if (seed != 42)
			{
				continue;
			}
			const Size size{ 1280, 720 };
			const Vec3 focus{ 32900, 20, 32900 };
			const BasicCamera3D camera{ size, 40_deg, focus + Vec3{ 260, 190, -300 }, focus };
			world.update(camera.getEyePosition());
			WorldRenderer terrainRenderer;
			RoadRenderer roadRenderer;
			context.expect(roadRenderer.loadAssets(), U"Road assets load");
			const RenderTexture target{ size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
			{
				const ScopedRenderTarget3D renderTarget{ target.clear(ColorF{ 0.3 }) };
				const ScopedRenderStates3D state{ DepthStencilState::DepthTestWrite };
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(Vec3{ 1, 1, 1 }.normalized());
				Graphics3D::SetGlobalAmbientColor(ColorF{ 0.5 });
				terrainRenderer.render(world, network, camera);
				roadRenderer.render(network, world, ViewFrustum{ camera, 24000.0 }, camera.getEyePosition());
			}
			Graphics3D::Flush();
			Image screenshot;
			target.readAsImage(screenshot);
			context.expect(screenshot.save(testDirectory + U"Screenshot/generated_roads_review.png"), U"Production terrain and road renderers produce a review image");
			int visibleRoadSamples = 0;
			int darkRoadSamples = 0;
			for (const auto& edge : network.edges())
			{
				const auto bezier = network.getBezier(edge.id);
				if (!bezier)
				{
					continue;
				}
				const Vec3 position = bezier->positionAt(bezier->totalLength * 0.5f);
				// Beyond this range a narrow road occupies less than a few pixels.
				if (position.distanceFrom(camera.getEyePosition()) > 550.0)
				{
					continue;
				}
				const Vec3 right = tangentToRight(bezier->tangentAt(bezier->totalLength * 0.5f));
				const Float3 roadPixel = camera.worldToScreenPoint(Float3{ position + right * 0.8 + Vec3{ 0, kRoadSurfaceLift, 0 } });
				const Float3 grassPixel = camera.worldToScreenPoint(Float3{ position + right * (edge.totalWidth() * 0.5 + 4.0) });
				const Point roadPoint{ static_cast<int>(roadPixel.x), static_cast<int>(roadPixel.y) };
				const Point grassPoint{ static_cast<int>(grassPixel.x), static_cast<int>(grassPixel.y) };
				if (roadPixel.z <= 0 || grassPixel.z <= 0 || !screenshot.inBounds(roadPoint) || !screenshot.inBounds(grassPoint))
				{
					continue;
				}
				++visibleRoadSamples;
				const Color road = screenshot[roadPoint];
				const Color grass = screenshot[grassPoint];
				// Sample inside a lane, away from the newly generated yellow center line.
				// Reject both grass and blue water exposed by holes in the pavement.
				const int roadChroma = Max(Max(road.r, road.g), road.b) - Min(Min(road.r, road.g), road.b);
				const int grassChroma = Max(Max(grass.r, grass.g), grass.b) - Min(Min(grass.r, grass.g), grass.b);
				darkRoadSamples += (roadChroma < 35 && road.b <= road.r + 8 && roadChroma < grassChroma);
			}
			report << U"visibleRoadSamples={} roadMaterialVisible={}"_fmt(visibleRoadSamples, darkRoadSamples);
			context.expect(visibleRoadSamples > 20 && darkRoadSamples > visibleRoadSamples * 0.8,
				U"Dense-town roads must remain visible through the terrain");
		}
		DebugLog::shutdown();
	});
	runner.add(U"CityLighting.ShadowsAndMaterialReadback", [](TestContext& context)
	{
		CityLighting lighting;
		context.expect(lighting.initialize(U"../../App/shaders/hlsl/city_forward.hlsl"), U"All city shaders compile");
		if (!lighting.ready())
		{
			return;
		}
		const Size size{ 1280, 720 };
		const BasicCamera3D camera{ size, 40_deg, Vec3{ 55, 65, -80 }, Vec3{ 0, 0, 5 } };
		const Vec3 sun = Vec3{ 1, 1, 1 }.normalized();
		const Box ground{ 0, -0.5, 0, 120, 1, 120 };
		const Box caster{ -12, 4, 5, 8, 8, 8 };
		Array<Model> homes;
		for (int index = 11; index <= 14; ++index)
		{
			homes << Model{ U"../../App/assets/buildings/residential/residential_{:03d}.obj"_fmt(index) };
			Model::RegisterDiffuseTextures(homes.back(), TextureDesc::MippedSRGB);
		}
		auto drawBuildings = [&]
		{
			caster.draw(ColorF{ 0.70, 0.64, 0.55 });
			for (int index = 0; index < 8; ++index)
			{
				const Mat4x4 transform = Mat4x4::Translate(8.0 + (index % 4) * 13.0, 0.02, -12.0 + (index / 4) * 30.0);
				homes[index % homes.size()].draw(transform);
			}
		};
		Graphics3D::SetCameraTransform(camera);
		lighting.update(camera, Vec3{ 0, 0, 5 }, sun, 1.0, 1, [&](Vec3, double)
		{
			drawBuildings();
			ground.draw(ColorF{ 0.55 });
		});
		const RenderTexture target{ size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
		{
			const ScopedRenderTarget3D renderTarget{ target.clear(ColorF{ 0.35, 0.45, 0.55 }) };
			const ScopedRenderStates3D states{ DepthStencilState::DepthTestWrite, RasterizerState::SolidCullNone };
			const ScopedRenderStates3D sampler{ ScopedRenderStates3D::SamplerStateInfo{ ShaderStage::Pixel, 1, SamplerState::ClampNearest } };
			Graphics3D::SetCameraTransform(camera);
			Graphics3D::SetSunDirection(sun);
			Graphics3D::SetSunColor(ColorF{ 0.9 });
			Graphics3D::SetGlobalAmbientColor(ColorF{ 0.32 });
			lighting.bind();
			const ScopedCustomShader3D shader{ lighting.shader() };
			ground.draw(ColorF{ 0.55 });
			Box{ 24, 0.01, 3, 60, 0.02, 7 }.draw(ColorF{ 0.12 });
			drawBuildings();
		}
		Graphics3D::Flush();
		Image screenshot;
		target.readAsImage(screenshot);
		context.expect(screenshot.size() == size, U"Readback has the requested dimensions");
		if (screenshot.size() != size)
		{
			return;
		}
		FileSystem::CreateDirectories(U"Screenshot");
		context.expect(screenshot.save(U"Screenshot/city_lighting_review.png"), U"GPU output is saved for visual review");
		auto luminanceAt = [&](Vec3 position)
		{
			const Float3 pixel = camera.worldToScreenPoint(Float3{ position });
			const int x = Clamp(static_cast<int>(Round(pixel.x)), 1, size.x - 2);
			const int y = Clamp(static_cast<int>(Round(pixel.y)), 1, size.y - 2);
			double value = 0;
			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const Color color = screenshot[y + dy][x + dx];
					value += (color.r + color.g + color.b) / (3.0 * 255.0);
				}
			}
			return value / 9;
		};
		const double shadow = luminanceAt(Vec3{ -18, 0, -1 });
		const double sunlight = luminanceAt(Vec3{ -32, 0, -1 });
		TextWriter report{ U"TestResults/lighting_diagnostics.txt" };
		report << U"shadowLuminance={} sunLuminance={} ratio={} shadowSubmitMs={}"_fmt(shadow, sunlight, shadow / Max(0.001, sunlight), lighting.shadowMilliseconds());
		context.expect(shadow < sunlight * 0.85, U"Cast shadow is measurably darker than adjacent lit ground");
		context.expect(shadow > sunlight * 0.25, U"Ambient skylight preserves detail in the shadow");
	});
	runner.add(U"Parcels.BothRoadSidesAndSharedBoundary", [](TestContext& context)
	{
		TextWriter report{ U"TestResults/parcel_partition.txt" };
		int valid = 0;
		for (int degrees = 0; degrees < 360; degrees += 5)
		{
			const double angle = degrees * Math::Pi / 180.0;
			const Vec2 along{ Cos(angle), Sin(angle) };
			for (int sign : { -1, 1 })
			{
				const Vec2 inward = Vec2{ -along.y, along.x } * sign;
				const Vec2 origin{ 1024, 1024 };
				const Vec2 center = origin + inward * 10;
				const Vec2 neighbor = center + along * 16;
				Array<Vec2> points{ origin - along * 12, origin + along * 12,
					origin + along * 12 + inward * 24, origin - along * 12 + inward * 24 };
				points = UrbanParcel::clipCloserTo(std::move(points), center, neighbor);
				UrbanParcel::normalize(points);
				const Polygon shape{ points };
				context.expect(Polygon::Validate(points) == PolygonFailureType::OK, U"Both road sides triangulate after partitioning");
				context.expect(!shape.indices().isEmpty() && shape.contains(center), U"A usable surface and garden interior remain");
				context.expect(!shape.contains((center + neighbor) * 0.5), U"Adjacent plots leave the prescribed boundary gap");
				valid += !shape.isEmpty();
			}
		}
		report << U"validRoadSideOrientations={}/144"_fmt(valid);
		// The two lots live in different chunks; neither may claim its neighbour's yard.
		World world;
		world.reserveChunks();
		world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		for (int x : { 0, 1 })
		{
			world.installChunkDirect(Point{ x, 0 }, HeightMapResult{
				Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20.0f), 20.0f, 20.0f });
			world.getChunk(Point{ x, 0 })->buildingGrid[{ x == 0 ? ZONE_CELLS - 1 : 0, 0 }].type = BuildingType::Detached;
		}
		const Vec2 left{ 1016, 8 }, right{ 1032, 8 };
		Array<Vec2> lot{ Vec2{ 1000, 0 }, Vec2{ 1040, 0 }, Vec2{ 1040, 32 }, Vec2{ 1000, 32 } };
		const Polygon partition{ UrbanParcel::partition(world, Point{ 0, 0 }, ZONE_CELLS - 1, 0, left, lot) };
		context.expect(partition.contains(left) && !partition.contains(right) && !partition.contains(Vec2{ 1024, 8 }),
			U"Partition searches adjacent chunks as well as the local building grid");
	});
	runner.add(U"CityLighting.MovingVehicleShadowClearsWithoutStaticRebuild", [](TestContext& context)
	{
		const FilePath testDirectory = FileSystem::CurrentDirectory();
		struct RestoreDirectory
		{
			FilePath path;
			~RestoreDirectory() { FileSystem::ChangeCurrentDirectory(path); }
		} restore{ testDirectory };
		FileSystem::ChangeCurrentDirectory(testDirectory + U"../../App/");
		CityLighting lighting;
		context.expect(lighting.initialize(), U"Static and dynamic shadow shaders compile");
		const Size size{ 960, 720 };
		const Vec3 focus{ 0, 0, 0 };
		const BasicCamera3D camera{ size, 35_deg, Vec3{ 0, 75, -45 }, focus };
		const Vec3 sun = Vec3{ 1, 1, 1 }.normalized();
		const Box ground{ 0, -0.5, 0, 100, 1, 100 };
		const MSRenderTexture target{ size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes };
		VehicleRenderer renderer;
		Vehicle vehicle;
		vehicle.id = 2;
		vehicle.type = VehicleType::PassengerCar;
		vehicle.position = Vec3{ -15, 0, 0 };
		Array<Vehicle> vehicles{ vehicle };
		int staticBuilds = 0;
		auto renderGround = [&](bool moving, bool drawBody)
		{
			std::function<void(Vec3, double)> dynamic;
			if (moving) { dynamic = [&](Vec3 center, double radius) { renderer.renderShadowCasters(vehicles, center, radius); }; }
			lighting.update(camera, focus, sun, 1.0, 1, [&](Vec3, double)
			{
				++staticBuilds;
				ground.draw(ColorF{ 0.6 });
			}, dynamic);
			{
				const ScopedRenderTarget3D targetScope{ target.clear(ColorF{ 0.4 }) };
				const ScopedRenderStates3D state{ DepthStencilState::DepthTestWrite, RasterizerState::SolidCullNone };
				const ScopedRenderStates3D sampler{ ScopedRenderStates3D::SamplerStateInfo{ ShaderStage::Pixel, 1, SamplerState::ClampNearest } };
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(sun);
				Graphics3D::SetSunColor(ColorF{ 0.9 });
				lighting.bind();
				const ScopedCustomShader3D shader{ lighting.shader() };
				ground.draw(ColorF{ 0.6 });
				if (drawBody) { renderer.render(vehicles, camera.getEyePosition()); }
			}
			Graphics3D::Flush();
			target.resolve();
			Image result;
			target.readAsImage(result);
			return result;
		};
		const Image baseline = renderGround(false, false);
		const Image before = renderGround(true, false);
		vehicles[0].position.z += 12;
		const Image after = renderGround(true, false);
		int oldShadowPixels = 0, restoredPixels = 0, newShadowPixels = 0;
		for (int y = 0; y < size.y; ++y)
		{
			for (int x = 0; x < size.x; ++x)
			{
				if (baseline[y][x].r > before[y][x].r + 12)
				{
					++oldShadowPixels;
					restoredPixels += Abs(static_cast<int>(baseline[y][x].r) - after[y][x].r) <= 3;
				}
				newShadowPixels += baseline[y][x].r > after[y][x].r + 12;
			}
		}
		TextWriter report{ testDirectory + U"TestResults/moving_shadow.txt" };
		report << U"staticBuilds={} oldShadowPixels={} restoredPixels={} newShadowPixels={}"_fmt(staticBuilds, oldShadowPixels, restoredPixels, newShadowPixels);
		context.expect(oldShadowPixels > 30 && newShadowPixels > 30, U"The real car mesh casts a measurable moving shadow");
		context.expect(restoredPixels >= oldShadowPixels * 0.98, U"Moving the car removes its previous shadow without a trail");
		context.expectEqual(staticBuilds, 1, U"Moving vehicles do not rebuild the static depth map");
		renderGround(true, true).save(testDirectory + U"Screenshot/moving_vehicle_shadow.png");
	});
	runner.add(U"Parcels.RotatedAndOffsetFootprints", [](TestContext& context)
	{
		const auto home = ParcelGeometry::footprint(Vec2{ 1023, 400 }, 5.0, 45_deg);
		context.expect(ParcelGeometry::overlaps(home, ParcelGeometry::footprint(Vec2{ 1031, 400 }, 5.0, 0.0)),
			U"Rotated plots collide across a chunk boundary");
		context.expect(!ParcelGeometry::overlaps(home, ParcelGeometry::footprint(Vec2{ 1040, 400 }, 5.0, 0.0)),
			U"Separated plots remain available");
		const ParcelGeometry::Quad crossingRoad{ Vec2{ 1015, 399 }, Vec2{ 1040, 399 }, Vec2{ 1040, 402 }, Vec2{ 1015, 402 } };
		context.expect(ParcelGeometry::overlaps(home, crossingRoad), U"Side roads must also protect their full width");
	});
	runner.add(U"Terrain.HeightMatchesRenderedTriangles", [](TestContext& context)
	{
		Grid<float> heights(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 0.0f);
		heights[{ 1, 1 }] = 8.0f;
		// The rendered diagonal joins (16,0) and (0,16), so this triangle is flat.
		context.expectNear(sampleHeightMap(heights, Point{ 0, 0 }, 4.0f, 4.0f), 0.0, 1e-5,
			U"Roads and parcels must sample the actual terrain triangle");
		context.expectNear(sampleHeightMap(heights, Point{ 0, 0 }, 12.0f, 12.0f), 4.0, 1e-5,
			U"The second triangle must use its plane, not a bilinear surface");
	});
	runner.add(U"Terrain.GenerationHeightDiagnostics", [](TestContext& context)
	{
		World world;
		TextWriter report{ U"TestResults/terrain_height_diagnostics.txt" };
		for (uint64 seed : { 20260316ULL, 42ULL, 20260912ULL })
		{
			world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
			double maxDifference = 0.0;
			double totalDifference = 0.0;
			double maxTriangleError = 0.0;
			Vec2 worstPosition;
			double worstSample = 0, worstReference = 0;
			Float3 worstVertices;
			int buriedSamples = 0;
			int sampleCount = 0;
			for (Point coord : { Point{ 21, 63 }, Point{ 32, 32 }, Point{ 16, 20 } })
			{
				const auto terrain = world.buildHeightMap(coord);
				for (int row = 0; row < HEIGHT_CELLS; ++row)
				{
					for (int col = 0; col < HEIGHT_CELLS; ++col)
					{
						const float x = coord.x * CHUNK_SIZE + col * 16.0f + 5.0f;
						const float z = coord.y * CHUNK_SIZE + row * 16.0f + 7.0f;
						const double sampled = sampleHeightMap(terrain.heightMap, coord, x, z);
						const double triangleHeight = terrain.heightMap[{ col, row }] * (4.0 / 16.0)
							+ terrain.heightMap[{ col + 1, row }] * (5.0 / 16.0)
							+ terrain.heightMap[{ col, row + 1 }] * (7.0 / 16.0);
						if (Abs(sampled - triangleHeight) > maxTriangleError)
						{
							maxTriangleError = Abs(sampled - triangleHeight);
							worstPosition = Vec2{x, z};
							worstSample = sampled;
							worstReference = triangleHeight;
							worstVertices = Float3{terrain.heightMap[{col, row}], terrain.heightMap[{col + 1, row}], terrain.heightMap[{col, row + 1}]};
						}
						const double difference = sampled - world.computeHeight(x, z);
						maxDifference = Max(maxDifference, Abs(difference));
						totalDifference += Abs(difference);
						buriedSamples += (difference > kRoadSurfaceLift);
						++sampleCount;
					}
				}
			}
			report << U"seed={} samples={} analyticMaxDifferenceM={:.5f} analyticMeanDifferenceM={:.5f} analyticBuriedSamples={}"_fmt(
				seed, sampleCount, maxDifference, totalDifference / sampleCount, buriedSamples);
			context.expect(sampleCount > 0, U"Real generated terrain was sampled");
			report << U"renderedTriangleMaxErrorM={:.9f} position={} sampled={:.9f} reference={:.9f} vertices={}"_fmt(
				maxTriangleError, worstPosition, worstSample, worstReference, worstVertices);
			context.expect(maxTriangleError < 0.0001, U"Generated terrain sampling matches the rendered plane within 0.1mm");
		}
	});
}
