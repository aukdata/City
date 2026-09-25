#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/gen/RoadDesignLimits.hpp"
#include "src/gen/RoadTerrainFit.hpp"
#include "src/gen/WaterCrossings.hpp"
#include "src/gen/RoadAlignment.hpp"
#include "src/gen/RoadConstructionCost.hpp"
#include "src/gen/RailCostProfile.hpp"
#include "src/gen/RoadVerticalAlignment.hpp"
#include "src/gen/MapGenerator.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/traffic/TrafficSpawn.hpp"
#include "src/traffic/VehicleManager.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/gen/UrbanParcel.hpp"
#include "src/traffic/SignalRegistry.hpp"
#include "src/road/GuideSign.hpp"
#include "src/road/RoadSign.hpp"
#include "src/road/JunctionGeometry.hpp"

namespace
{
	struct AppDirectory
	{
		FilePath previous = FileSystem::CurrentDirectory();
		AppDirectory() { FileSystem::ChangeCurrentDirectory(previous+U"../../App/"); RegisterAssets(); }
		~AppDirectory() { FileSystem::ChangeCurrentDirectory(previous); }
	};

	/// @brief シミュレーションスレッドと同じ探索グラフで実トリップを解決する。
	void resolveTrips(VehicleManager& manager, const SimGraph& graph, const TrafficGraph& routes, TestContext& context)
	{
		for (const auto& message : manager.collectRequests())
		{
			const auto* request = std::get_if<RouteRequest>(&message);
			if (!request) { continue; }
			const auto path = routes.dijkstra(routes.entryNodeId(request->startEdge, request->startLane), request->goalEdge);
			RouteResponse response; response.vehicleId = request->vehicleId; response.found = path.found;
			context.expect(path.found, U"Automatically selected destinations are reachable in the actual route graph");
			int previous = request->startEdge;
			for (const int id : path.nodeIds)
			{
				const auto* node = routes.getLaneNode(id);
				if (!node || node->edgeId == previous) { continue; }
				const auto* edge = graph.getEdge(node->edgeId);
				response.waypoints << RouteWaypoint{edge->id, node->laneIndex, node->arcPos, edge->length, edge->length/8};
				previous = edge->id;
			}
			manager.applyRouteResponse(response);
		}
	}
}

void registerRoadsideLifeTests(TestRunner& runner)
{
	runner.add(U"RoadDesign.ThroughRoadContinuity", [](TestContext& context)
	{
		RoadNetwork roads;
		const int first = roads.addNode({100,20,100}), middle = roads.addNode({180,20,150}), last = roads.addNode({260,20,100});
		const int a = *roads.addEdge(first,middle,{120,20,130},{155,20,150},RoadType::Arterial,2);
		const int b = *roads.addEdge(middle,last,{205,20,150},{240,20,130},RoadType::Arterial,2);
		context.expect(RoadDesignLimits::minimumRadius(*roads.getBezier(a)) < 100, U"Fixture has a real sharp through curve");
		context.expectEqual(RoadDesignLimits::smoothThroughChains(roads),1,U"Treat the joined road as one alignment");
		for (int id : {a,b})
		{
			RoadDesignLimits::constrainCurve(roads,id);
			context.expect(RoadDesignLimits::minimumRadius(*roads.getBezier(id)) >= 100, U"Both halves respect the arterial radius");
		}
		const Vec3 incoming = roads.getBezier(a)->tangent(1).normalized(), outgoing = roads.getBezier(b)->tangent(0).normalized();
		context.expect(incoming.dot(outgoing) > .99999,U"Radius correction does not introduce a kink at the shared node");
		context.expectEqual(static_cast<int>(roads.getNode(middle)->attachments.size()),2,U"Road connectivity is preserved");
		for (int id : {a,b})
		{
			roads.getEdge(id)->designGrade = true;
			roads.getEdge(id)->useElevation = false;
			roads.getEdge(id)->edgeState = EdgeState::Open;
		}
		const auto layout = JunctionGeometry::build(roads,middle);
		context.expect(layout.elevated && layout.groundConnected && !roads.isNodeElevated(middle)
			&& roads.nodeUsesDesignHeight(middle),U"Designed ground roads retain ground clipping without becoming bridge hit targets");
	});

	runner.add(U"Buildings.LargeSiteParcelBoundary", [](TestContext& context)
	{
		for (const float angle : {0.0f, .37f, -.6f})
		{
			Building fuel, house; fuel.type = BuildingType::RoadsideFuelStation; house.type = BuildingType::Detached;
			fuel.angle = house.angle = angle;
			const Vec2 along{Cos(angle),Sin(angle)}, inward{-along.y,along.x};
			Array<Vec2> source;
			for (const Vec2 point : {Vec2{-30,-30},Vec2{50,-30},Vec2{50,30},Vec2{-30,30}})
			{
				source << along*point.x+inward*point.y;
			}
			const auto large = UrbanParcel::clipBetweenSites(source,{0,0},fuel,along*34,house);
			const auto small = UrbanParcel::clipBetweenSites(source,along*34,house,{0,0},fuel);
			double largeEnd = -Math::Inf, smallStart = Math::Inf;
			for (const auto& point : large) { largeEnd = Max(largeEnd,point.dot(along)); }
			for (const auto& point : small) { smallStart = Min(smallStart,point.dot(along)); }
			context.expect(largeEnd > 22 && smallStart > largeEnd, U"The full 44m site fits and neighboring parcels remain disjoint");
			context.expect(smallStart < 34-buildingFootprintXZ(house.type)*.5, U"The rotated neighbor also retains its full footprint");
		}
	});

	runner.add(U"Rendering.RefinedStreetFurniture", [](TestContext& context)
	{
		AppDirectory directory;
		SignalRegistry registry;
		context.expect(registry.load(U"assets/signals/"), U"Load refined signals through the production registry");
		const auto* model = registry.getModel(U"signal_3lamp");
		const auto* definition = registry.getDef(U"signal_3lamp");
		context.expect(model && definition && model->texture.has_value(),U"Live signal model and atlas are available");
		if (!model || !definition || !model->texture) { return; }
		const auto meshData = [](const PartModelData& part)
		{
			MeshData result; result.vertices = part.vertices; result.indices = part.indices; return result;
		};
		const Mesh body{meshData(model->meshes.at(U"body"))};
		for (const auto& [name, part] : model->meshes)
		{
			for (const auto& vertex : part.vertices)
			{
				context.expect(std::isfinite(vertex.pos.x) && Abs(vertex.normal.length()-1) < .002,U"Furniture normals and positions are valid");
			}
			if (name.starts_with(U"lamp_") || name == U"sub_lamp")
			{
				context.expect(part.indices.size() >= 48,U"Circular lenses retain a smooth silhouette");
			}
		}
		const MeshData guide = GuideSign::CreatePoleMesh(), pole = RoadSign::CreatePoleMesh(2.5f);
		context.expect(guide.indices.size() > 1000 && pole.indices.size() > 400,U"Runtime pole models include their mounting hardware");
		const Mesh guideMesh{guide}, poleMesh{pole};
		const Size size{960,640};
		const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		JSON report;
		for (int view = 0; view < 4; ++view)
		{
			const bool signal = view < 2;
			const Vec3 focus = signal ? Vec3{-2.35,6.36,0} : Vec3{1.1,3.6,0};
			const Vec3 offset = signal ? Vec3{-.5,.35,view==0 ? 2.4 : -2.4} : Vec3{4.0,2.0,view==2 ? 9.0 : -9.0};
			const BasicCamera3D camera{size,40_deg,focus+offset,focus};
			{
				const ScopedRenderTarget3D rt{target.clear(ColorF{.07,.12,.19})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(Vec3{1,2,3}.normalized());
				Graphics3D::SetGlobalAmbientColor(ColorF{.45});
				if (signal)
				{
					body.draw(Mat4x4::Identity(),*model->texture);
					for (int lamp = 0; lamp < 3; ++lamp)
					{
						const String name = U"lamp_{}"_fmt(lamp);
						const String color = lamp==0 ? U"green" : (lamp==1 ? U"yellow" : U"red");
						const auto region = definition->states.at(color).uvRect;
						Mesh{meshData(model->meshes.at(name))}.draw(Mat4x4::Identity(),(*model->texture)(static_cast<int>(region.x),static_cast<int>(region.y),static_cast<int>(region.z),static_cast<int>(region.w)));
					}
				}
				else
				{
					guideMesh.draw(ColorF{.6}); poleMesh.draw(Mat4x4::Translate(-1.0,0.0,0.0),ColorF{.6});
				}
			}
			Graphics3D::Flush();
			Image actual; target.readAsImage(actual);
			int changed = 0, red = 0, green = 0, yellow = 0;
			const Color background = actual[0][0];
			for (const Color pixel : actual)
			{
				changed += pixel != background;
				red += pixel.r > pixel.g+50 && pixel.r > pixel.b+50;
				green += pixel.g > pixel.r+40 && pixel.g > pixel.b+40;
				yellow += pixel.r > 130 && pixel.g > 130 && pixel.b < 80;
			}
			context.expect(changed > 1000,U"Refined surfaces are visible from both sides with backface culling");
			if (view == 0) { context.expect(red>100 && green>100 && yellow>100,U"All live lamp colors remain visible inside their new visors"); }
			report[U"view_{}"_fmt(view)][U"visiblePixels"] = changed;
			report[U"view_{}"_fmt(view)][U"lampPixels"] = Array<int>{red,green,yellow};
			actual.save(directory.previous+U"Screenshot/furniture_refined_{}.png"_fmt(view));
		}
		report.save(directory.previous+U"TestResults/refined_furniture.json");
	});


	runner.add(U"Buildings.UrbanAndRuralSites", [](TestContext& context)
	{
		AppDirectory directory;
		const Array<BuildingType> types{BuildingType::UrbanConvenience, BuildingType::RoadsideConvenience,
			BuildingType::UrbanFuelStation, BuildingType::RoadsideFuelStation, BuildingType::RuralHouse};
		for (const auto type : types)
		{
			const int count = type == BuildingType::RuralHouse ? 3 : 1;
			for (int variant = 0; variant < count; ++variant)
			{
				String stem; tryGetBuildingModelStemForVariant(type, static_cast<uint8>(variant), stem);
				const String path = U"assets/buildings/{}/{}"_fmt(isResidentialBuildingType(type) ? U"residential" : U"commercial", stem);
				const Model model{path+U".obj"};
				context.expect(!model.isEmpty(), U"Complete site loads: "+stem);
				if (model.isEmpty()) { continue; }
				const Box bounds = model.boundingBox();
				context.expectNear(bounds.center.y-bounds.size.y*.5, 0, .001, U"Forecourt sits on ground");
				context.expectNear(Max(bounds.size.x, bounds.size.z), buildingFootprintXZ(type), .05, U"Parking and yard dimensions are not squeezed into a house slot");
				const Image texture{path+U"_diffuse.png"};
				context.expect(texture.width() == 1024 && texture.height() == 1024, U"Baked texture is packaged");
				const Model distant{U"assets/buildings/lod/{}.obj"_fmt(stem)};
				context.expect(!distant.isEmpty(), U"Distant site retains a model");
				if (!distant.isEmpty()) { context.expectNear(distant.boundingBox().size.y, bounds.size.y, .15, U"Distant roofs retain height"); }
			}
		}
		context.expect(buildingFootprintXZ(BuildingType::RoadsideConvenience) > buildingFootprintXZ(BuildingType::UrbanConvenience)*2,
			U"Rural convenience store reserves a substantially larger lot");
		context.expect(buildingCapacity(BuildingType::RuralHouse) > 0, U"Rural homes contribute to population");
	});

	runner.add(U"RoadDesign.JunctionHeightRepair", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(65,65,100),100,100});
		RoadNetwork roads;
		const auto add=[&](Vec3 a,Vec3 b)
		{
			const int start=roads.addNode(a),end=roads.addNode(b);
			const int id=*roads.addEdge(start,end,a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
			auto* edge=roads.getEdge(id); edge->designGrade=true; edge->useElevation=true; edge->tunnel=true; edge->edgeState=EdgeState::Open;
		};
		add({220,22.5,200},{220,22.5,800}); add({200,20,500},{800,73.7,500});
		const auto before=RoadDesignLimits::measure(roads,world);
		context.expect(before.gradeViolations==0,U"Both approaching tunnels initially satisfy their grade limit");
		context.expect(roads.resolveIntersections(),U"The nearby tunnel crossing becomes a real junction");
		const auto joined=RoadDesignLimits::measure(roads,world);
		RoadDesignLimits::fitGrades(roads,world);
		const auto final=RoadDesignLimits::measure(roads,world);
		JSON report; report[U"before"]=before.gradeViolations; report[U"afterJoin"]=joined.gradeViolations; report[U"final"]=final.gradeViolations;
		report.save(U"TestResults/junction_grade_repair.json");
		context.expect(joined.gradeViolations>0,U"The fixture reproduces the grade change caused by sharing an endpoint");
		context.expect(final.gradeViolations==0 && final.radiusViolations==0,U"Re-establishing the grade plane repairs the shared junction without relaxing the limit");
	});

	runner.add(U"RoadDesign.ConstructionCostRatios", [](TestContext& context)
	{
		using namespace RoadConstructionCost;
		context.expect(unit(20, 20) == 1 && unit(20.5, 20) == 1, U"Surface road is the unit cost");
		context.expect(unit(15, 20) == 8 && unit(25, 20) == 8, U"Cuttings and bounded earthworks cost eight times surface");
		context.expect(unit(27,20)>112 && unit(6,-10,0)>256 && unit(30,20)>160,U"Viaducts add an exponential height penalty to sixteen times their height");
		context.expect((unit(60,20)-640)>(unit(40,20)-320)*4,U"Tall viaduct penalties grow faster than linearly");
		double run=0;const double continuous=segment(0,20,-1e9,800,run);run=0;double divided=0;
		for (int i=0;i<16;++i) { divided+=segment(0,20,-1e9,50,run); }
		context.expectNear(continuous,divided,.001,U"Subdividing a tunnel into edges cannot evade its total-length penalty");
		run=0;double separated=segment(0,20,-1e9,400,run);separated+=segment(20,20,-1e9,1,run);separated+=segment(0,20,-1e9,400,run);
		context.expect(continuous>separated && run==400,U"A genuine open-air interval ends a continuous tunnel run");
		context.expect(unit(11, 20) == 128 && unit(20, 100, 104) == 128, U"Covered tunnels cost 128 times surface, including those beneath streams");
		Array<RailCostProfile::Sample> samples;
		for (int i = 0; i <= 10; ++i) { samples << RailCostProfile::Sample{{i * 40, 0}, 20 + (i % 2) * 2.0, 15, 25}; }
		const auto result = RailCostProfile::solve(samples, 20, 20, .06,
			[](const RailCostProfile::Sample& sample, double elevation) { return unit(elevation, sample.ground); });
		context.expect(result.feasible, U"The shared vertical solver accepts the road cost profile");
		if (result.feasible)
		{
			double cost = 0;
			for (size_t i = 1; i < samples.size(); ++i)
			{
				cost += 20 * (unit(result.heights[i-1], samples[i-1].ground) + unit(result.heights[i], samples[i].ground));
				context.expect(Abs(result.heights[i] - result.heights[i-1]) <= 2.4 + 1e-8, U"Height smoothing retains the hard road grade limit");
			}
			context.expect(Abs(cost - result.cost) < .001 && cost < 1800, U"Reported cost matches final smoothed geometry and improves on the flat cutting-heavy profile");
		}
	});
	runner.add(U"RoadDesign.TerrainContourCurveContinuity", [](TestContext& context)
	{
		// 円い山腹の等高線を粗い折れ線で与え、完成線形の方向と曲率を測る。
		JSON report;
		for (const int offset : {0, 32768})
		{
			World world;
			world.reserveChunks(); world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
			const Vec2 center{offset + 2048, offset + 2048};
			constexpr double radius = 1100;
			for (int cz = 0; cz < 4; ++cz) for (int cx = 0; cx < 4; ++cx)
			{
				Grid<float> heights(65, 65);
				for (int z = 0; z <= 64; ++z) for (int x = 0; x <= 64; ++x)
				{
					const Vec2 point{offset + cx * 1024 + x * 16, offset + cz * 1024 + z * 16};
					heights[{x,z}] = static_cast<float>(250 + (point.distanceFrom(center) - radius) * .2);
				}
				world.installChunkDirect({offset / 1024 + cx, offset / 1024 + cz}, HeightMapResult{heights, 0, 650});
			}
			Array<Vec3> points;
			for (int i = 0; i <= 10; ++i)
			{
				const double angle = -1.25 + i * .25;
				const Vec2 point = center + Vec2{Cos(angle), Sin(angle)} * radius;
				points << Vec3{point.x, world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.y)), point.y};
			}
			for (const RoadType type : {RoadType::LocalRoad, RoadType::Arterial})
			{
				const auto route = RoadAlignment::fitTerrain(world, points, type);
				context.expect(route.has_value(), U"A contour road can be fitted to the hillside");
				if (!route) { continue; }
				const auto curvature = [](const CubicBezier& curve, bool end)
				{
					const Vec3 velocity = (end ? curve.p3 - curve.p2 : curve.p1 - curve.p0) * 3;
					const Vec3 acceleration = (end ? curve.p3 - curve.p2 * 2 + curve.p1 : curve.p2 - curve.p1 * 2 + curve.p0) * 6;
					const double speed = Vec2{velocity.x, velocity.z}.length();
					return (velocity.x * acceleration.z - velocity.z * acceleration.x) / (speed * speed * speed);
				};
				double length = 0, straight = 0, curvatureJump = 0, earthwork = 0, radialDeviation = 0;
				for (size_t i = 0; i < route->curves.size(); ++i)
				{
					const auto& curve = route->curves[i];
					length += curve.totalLength;
					if (curve.minimumHorizontalRadius() > 20000) { straight += curve.totalLength; }
					context.expect(RoadAlignment::respectsLimits(curve, type), U"Every curved piece respects the grade and radius limits");
					if (i > 0)
					{
						const auto& before = route->curves[i - 1];
						context.expect(before.p3.distanceFrom(curve.p0) < .0001, U"The road surface has no gaps");
						const Vec2 incoming{before.p3.x - before.p2.x, before.p3.z - before.p2.z};
						const Vec2 outgoing{curve.p1.x - curve.p0.x, curve.p1.z - curve.p0.z};
						context.expect(incoming.normalized().dot(outgoing.normalized()) > .999999, U"Heading is continuous at every join");
						curvatureJump = Max(curvatureJump, Abs(curvature(before, true) - curvature(curve, false)));
					}
					for (int sample = 0; sample <= 8; ++sample)
					{
						const Vec3 point = curve.evaluate(sample / 8.0f);
						earthwork = Max(earthwork, Abs(point.y - world.sampleHeight(static_cast<float>(point.x), static_cast<float>(point.z))));
						radialDeviation = Max(radialDeviation, Abs(Vec2{point.x, point.z}.distanceFrom(center) - radius));
					}
				}
				context.expect(route->curves.front().p0.distanceFrom(points.front()) < .0001
					&& route->curves.back().p3.distanceFrom(points.back()) < .0001, U"Settlement connection positions and heights stay fixed");
				context.expect(curvatureJump < .00001, U"Turning develops continuously instead of jumping from straights to minimum-radius corners");
				context.expect(straight / length < .15, U"The hillside bend is curved throughout, not mostly straight chords");
				context.expect(earthwork < 1 && radialDeviation < 50, U"Smoothing follows the contour without cutting across the mountain");
				const String key = U"{}_{}"_fmt(offset, static_cast<int>(type));
				report[key][U"curvatureJump"] = curvatureJump;
				report[key][U"straightFraction"] = straight / length;
				report[key][U"maximumEarthwork"] = earthwork;
				report[key][U"radialDeviation"] = radialDeviation;
			}
		}
		report.save(U"TestResults/terrain_contour_curves.json");
	});
	runner.add(U"RoadDesign.LongValleyDetour", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		DebugLog::initialize(U"TestResults/long_road.log");
		// 北側の狭い谷は急斜面、南側は緩い谷。約7 kmの両岸を結ぶ。
		const auto terrain = [](double x, double z)
		{
			const double spread = 200 + z * .55;
			return 20 + 180 * (1 - std::exp(-Square((x - 4096) / spread)));
		};
		for (int cz = 0; cz < 8; ++cz) for (int cx = 0; cx < 8; ++cx)
		{
			Grid<float> grid(65, 65);
			for (int z = 0; z <= 64; ++z) for (int x = 0; x <= 64; ++x) { grid[{x,z}] = static_cast<float>(terrain(cx*1024+x*16, cz*1024+z*16)); }
			world.installChunkDirect({cx,cz}, HeightMapResult{grid,20,200});
		}
		const Vec3 start{700, world.sampleHeight(700,600), 600}, goal{7492,world.sampleHeight(7492,600),600};
		const Stopwatch timer{StartImmediately::Yes};
		const auto result = RoadAlignment::find(world, start, goal, RoadType::Arterial);
		JSON report; report[U"found"] = result.has_value(); report[U"milliseconds"] = timer.msF();
		context.expect(result.has_value(), U"長距離の両岸を、緩い斜面を通って接続できる");
		if (result)
		{
			double gap=0, run=0, detour=0;
			for (const auto& curve : result->curves)
			{
				run += curve.totalLength;
				context.expect(RoadAlignment::respectsLimits(curve,RoadType::Arterial), U"幹線の勾配と半径を満たす");
				for (int i=0; i<=20; ++i)
				{
					const Vec3 p=curve.evaluate(i/20.0f);
					gap=Max(gap,p.y-world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z)));
					detour=Max(detour,Abs(p.z-start.z));
				}
			}
			RoadNetwork built; int previous=built.addNode(start);
			for (const auto& curve : result->curves)
			{
				const int next=built.addNode(curve.p3);
				const int edge=*built.addEdge(previous,next,curve.p1,curve.p2,RoadType::Arterial,2);
				built.getEdge(edge)->designGrade=true;built.updateEdgeElevation(edge,world);previous=next;
			}
			built.smoothAllCurves();RoadDesignLimits::smoothThroughChains(built);
			RoadVerticalAlignment::apply(built,world);RoadDesignLimits::apply(built,world);
			WaterCrossings::repair(built,[&](double x,double z) { return world.sampleHeight(static_cast<float>(x),static_cast<float>(z)); });
			RoadDesignLimits::fitGrades(built,world);
			Array<CubicBezier> finished;
			for (const auto& edge : built.edges()) { if (edge.id>=0) { finished << *built.getBezier(edge.id); } }
			const auto audit=RoadDesignLimits::measure(built,world);
			report[U"finalMaximumHeight"]=RoadAlignment::maximumClearance(world,finished);
			context.expect(finished.size()==result->curves.size() && audit.gradeViolations==0 && audit.radiusViolations==0,U"完成後も接続と設計制約を保つ");
			context.expect(RoadAlignment::maximumClearance(world,finished)<20,U"後処理で地上回廊を高架化しない");
			report[U"maximumHeight"] = gap; report[U"length"] = run; report[U"detour"] = detour; report[U"cost"] = result->cost;
			context.expect(gap < 20, U"約180 m高の直線高架を選ばない");
			context.expect(detour > 500 && result->cost < 100000, U"広い探索範囲で安価な地形沿いの経路を選ぶ");
		}
		RoadNetwork generated;
		MapGenerator::Settlement left,right; left.center={start.x,start.z};right.center={goal.x,goal.z};
		left.kind=right.kind=MapGenerator::SettlementKind::RegionalCity;
		MapGenerator::generateGlobalRoads(42,{left,right},world,generated);
		Array<CubicBezier> initial;
		for (const auto& edge : generated.edges()) { if (edge.id>=0) { initial << *generated.getBezier(edge.id); } }
		const auto generatedAudit=RoadDesignLimits::measure(generated,world);
		report[U"generatedMaximumHeight"]=RoadAlignment::maximumClearance(world,initial);
		report[U"generatedEdges"]=initial.size();
		context.expect(generated.getNode(0)->attachments.size()>0 && generated.getNode(1)->attachments.size()>0 && initial.size()>10,U"初期生成の街道にも同じ地形回廊を使い、両都市を接続する");
		context.expect(generatedAudit.gradeViolations==0 && generatedAudit.radiusViolations==0 && RoadAlignment::maximumClearance(world,initial)<20,U"初期生成の完成形でも高架化・制約違反を起こさない");
		report.save(U"TestResults/long_valley_road.json");
		DebugLog::shutdown();
	});

	runner.add(U"RoadDesign.LongRidgeDetour", [](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		for (int cz=0;cz<8;++cz) for (int cx=0;cx<8;++cx)
		{
			Grid<float> grid(65,65);
			for (int z=0;z<=64;++z) for (int x=0;x<=64;++x)
			{
				const double wx=cx*1024+x*16,wz=cz*1024+z*16;
				grid[{x,z}]=static_cast<float>(20+200*std::exp(-Square((wx-4096)/500))*Clamp((3600-wz)/400,0.0,1.0));
			}
			world.installChunkDirect({cx,cz},HeightMapResult{grid,20,220});
		}
		const Vec3 a{400,20,600},b{7800,20,600};
		const auto route=RoadAlignment::find(world,a,b,RoadType::Arterial,60000,TransportMode::Road,GenerationSettings::get().roads_maximumGeneratedViaductHeight);
		context.expect(route.has_value(),U"7.4 kmの山越えでも尾根の端を回る地上経路が見つかる");
		if (!route) { return; }
		double maximumCut=0,detour=0;
		for (const auto& curve : route->curves)
		{
			context.expect(RoadAlignment::respectsLimits(curve,RoadType::Arterial),U"長距離の迂回も勾配と半径を守る");
			for (int i=0;i<=16;++i)
			{
				const auto p=curve.evaluate(i/16.0f);
				maximumCut=Max(maximumCut,world.sampleHeight(static_cast<float>(p.x),static_cast<float>(p.z))-p.y);
				detour=Max(detour,Abs(p.z-a.z));
			}
		}
		JSON report; report[U"cost"]=route->cost;report[U"maximumCut"]=maximumCut;report[U"detour"]=detour;
		report.save(U"TestResults/long_ridge_road.json");
		context.expect(maximumCut<8 && detour>2400 && route->cost<50000,U"長い直線トンネルを短絡として採用しない");
	});

	runner.add(U"RoadDesign.GenerationHeightBoundAndFlatRoute", [](TestContext& context)
	{
		World world;world.reserveChunks();world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(65,65,20),20,20});
		const Vec3 a{100,20,100},b{900,20,100};
		const auto flat=RoadAlignment::find(world,a,b,RoadType::Arterial);
		context.expect(flat && flat->curves.size()==1 && flat->expanded==0,U"合理的な平地の直線は余計に曲げない");
		const auto manual=RoadAlignment::find(world,a+Vec3{0,100,0},b+Vec3{0,100,0},RoadType::Arterial,100);
		const auto automatic=RoadAlignment::find(world,a+Vec3{0,100,0},b+Vec3{0,100,0},RoadType::Arterial,100,TransportMode::Road,GenerationSettings::get().roads_maximumGeneratedViaductHeight);
		context.expect(manual.has_value() && !automatic,U"自動生成の高架上限は手動計画の高さ指定と分ける");
	});


	runner.add(U"RoadDesign.GeneratedRoadHasNoStraightTunnelFallback", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		Grid<float> terrain(65, 65, 20);
		for (int z = 0; z <= 64; ++z) for (int x = 25; x <= 39; ++x) { terrain[{x, z}] = 100; }
		world.installChunkDirect({0, 0}, HeightMapResult{terrain, 20, 100});
		const Vec3 start{200, 20, 512}, goal{824, 20, 512};
		const auto direct = RoadAlignment::fit({start, goal});
		context.expect(std::isfinite(RoadAlignment::constructionCost(world, direct, RoadType::LocalRoad)),
			U"長い直線トンネルは費用上は有限の候補である");
		const auto route = RoadAlignment::find(world, start, goal, RoadType::LocalRoad, 0,
			TransportMode::Road, GenerationSettings::get().roads_maximumGeneratedViaductHeight);
		context.expect(!route, U"地形回廊が成立しない場合は直線トンネルにフォールバックしない");
	});

	runner.add(U"RoadDesign.SurfaceDetourBeatsTunnel", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		Grid<float> terrain(65, 65, 20);
		for (int z = 0; z <= 37; ++z) for (int x = 25; x <= 39; ++x) { terrain[{x, z}] = 100; }
		world.installChunkDirect({0, 0}, HeightMapResult{terrain, 20, 100});
		const Vec3 start{200, 20, 400}, goal{824, 20, 400};
		const auto direct = RoadAlignment::fit({start, goal});
		const double directCost = RoadAlignment::constructionCost(world, direct, RoadType::LocalRoad);
		const auto result = RoadAlignment::find(world, start, goal, RoadType::LocalRoad, 60000);
		context.expect(result.has_value(), U"A route is found around the end of the ridge");
		if (result)
		{
			double length = 0; for (const auto& curve : result->curves) { length += curve.totalLength; context.expect(RoadAlignment::respectsLimits(curve, RoadType::LocalRoad), U"Detour respects grade and turning radius"); }
			context.expect(result->cost < directCost * .65 && length > 624 * 1.2, U"The longer ground route wins over the short tunnel under surface, cutting, height-weighted viaduct and tunnel costs");
			JSON report; report[U"directCost"] = directCost; report[U"chosenCost"] = result->cost; report[U"length"] = length; report[U"expanded"] = result->expanded;
			report.save(U"TestResults/road_cost_detour.json");
		}
	});

	runner.add(U"RoadDesign.CostSelectedSwitchbacks", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> terrain(65,65,20);
		for (int z=0;z<=64;++z) for (int x=0;x<=64;++x) { terrain[{x,z}]=20+z*16*.2f; }
		world.installChunkDirect({32,32},HeightMapResult{terrain,20,225});
		DebugLog::initialize(U"TestResults/road_alignment.log");
		const Vec3 start{33280,40,32868},goal{33280,180,33568};
		const Stopwatch timer{StartImmediately::Yes};
		const auto result=RoadAlignment::find(world,start,goal,RoadType::LocalRoad,300000);
		JSON report; report[U"feasible"]=result.has_value(); report[U"milliseconds"]=timer.msF();
		if (result)
		{
			double length=0,maximumGrade=0,minimumRadius=Math::Inf,maximumEarthwork=0; int reversals=0; double previous=0;
			for (const auto& curve : result->curves)
			{
				length+=curve.totalLength; minimumRadius=Min(minimumRadius,curve.minimumHorizontalRadius());
				const double dx=curve.p3.x-curve.p0.x;
				if (Abs(dx)>2) { if (previous*dx<0) { ++reversals; } previous=dx; }
				for (int i=0;i<=20;++i)
				{
					const Vec3 tangent=curve.tangent(i/20.0f),point=curve.evaluate(i/20.0f);
					maximumGrade=Max(maximumGrade,Abs(tangent.y)/Vec2{tangent.x,tangent.z}.length());
					maximumEarthwork=Max(maximumEarthwork,Abs(point.y-world.sampleHeight(static_cast<float>(point.x),static_cast<float>(point.z))));
				}
			}
			RoadNetwork built; const int first=built.addNode(start),last=built.addNode(goal),middle=built.addNode(start.lerp(goal,.5));
			const Vec3 midpoint=start.lerp(goal,.5);
			const int lower=*built.addEdge(first,middle,start.lerp(midpoint,1.0/3),start.lerp(midpoint,2.0/3),RoadType::LocalRoad,2);
			const int upper=*built.addEdge(middle,last,midpoint.lerp(goal,1.0/3),midpoint.lerp(goal,2.0/3),RoadType::LocalRoad,2);
			const int before=built.addNode(goal+Vec3{80,0,0}),after=built.addNode(start+Vec3{80,0,0});
			const int approach=*built.addEdge(before,last,goal+Vec3{53,0,0},goal+Vec3{27,0,0},RoadType::Arterial,2);
			const int exit=*built.addEdge(first,after,start+Vec3{27,0,0},start+Vec3{53,0,0},RoadType::Arterial,2);
			const int route=built.addRoute(RoadRouteKind::PrefectureRoute,U"峠道",{approach,upper,lower,exit});
			RoadDesignLimits::apply(built,world);
			const auto audit=RoadDesignLimits::measure(built,world);
			context.expect(audit.edges>10 && audit.gradeViolations==0 && audit.radiusViolations==0,U"The generation pipeline actually replaces the steep connection with legal curved roads");
			context.expect(built.getNode(first)->attachments.size()==2 && built.getNode(last)->attachments.size()==2,U"Both fixed endpoints remain connected");
			const auto* revised=built.getRoute(route);
			context.expect(revised && revised->edgeIds.size()>10,U"The named route survives replacing a whole corridor");
			if (revised)
			{
				for (size_t i=1;i<revised->edgeIds.size();++i)
				{
					const auto* a=built.getEdge(revised->edgeIds[i-1]); const auto* b=built.getEdge(revised->edgeIds[i]);
					context.expect(a && b && (a->nodeA==b->nodeA || a->nodeA==b->nodeB || a->nodeB==b->nodeA || a->nodeB==b->nodeB),U"Reverse route order remains continuous through the replacement");
				}
			}
			int pointIndex=0;
			for (const auto& curve : result->curves) for (int i=0;i<4;++i) { const Vec3 p=curve.evaluate(i/4.0f); report[U"points"][pointIndex++]=Array<double>{p.x,p.y,p.z}; }
			report[U"lengthM"]=length; report[U"maximumGrade"]=maximumGrade; report[U"minimumRadiusM"]=minimumRadius;
			report[U"maximumEarthworkM"]=maximumEarthwork; report[U"reversals"]=reversals; report[U"expanded"]=result->expanded;
			context.expect(maximumGrade<=.09001 && minimumRadius>=15,U"The complete curves obey the local road design constraints");
			context.expect(reversals>=2 && length>1555,U"A steep bounded hillside produces switchbacks through the cost search");
			context.expect(maximumEarthwork<12,U"A hillside road stays near the terrain instead of crossing the sky");
		}
		report.save(U"TestResults/switchback_alignment.json");
		context.expect(result.has_value(),U"The slope-constrained search reaches the fixed high endpoint");
	});

	runner.add(U"RoadDesign.FlatTownBesideMountain", [](TestContext& context)
	{
		World world; world.reserveChunks(); world.setGenerationParams(42,WORLD_SIZE,WORLD_SIZE);
		Grid<float> terrain(65,65,20);
		for (int z=0;z<=64;++z) for (int x=0;x<=64;++x)
		{
			terrain[{x,z}]=static_cast<float>(20+Max(0.0,x*16.0-640)*.8);
		}
		world.installChunkDirect({32,32},HeightMapResult{terrain,20,328});
		RoadNetwork roads; Array<int> town;
		for (int z=0;z<5;++z) for (int x=0;x<5;++x) { town << roads.addNode({32848+x*96.0,20,32848+z*96.0}); }
		const auto connect=[&](int a,int b)
		{
			const Vec3 start=roads.getNode(a)->position,end=roads.getNode(b)->position;
			roads.addEdge(a,b,start.lerp(end,1.0/3),start.lerp(end,2.0/3),RoadType::LocalRoad,2);
		};
		for (int z=0;z<5;++z) for (int x=0;x<5;++x)
		{
			if (x<4) { connect(town[z*5+x],town[z*5+x+1]); }
			if (z<4) { connect(town[z*5+x],town[(z+1)*5+x]); }
		}
		const int high=roads.addNode({33728,276,33040}); connect(town[14],high);
		RoadDesignLimits::apply(roads,world);
		double maximumLift=0; int elevatedTown=0;
		for (int id : town) { maximumLift=Max(maximumLift,roads.getNode(id)->position.y-20); }
		for (const auto& edge : roads.edges()) { elevatedTown+=edge.id>=0 && edge.nodeA!=high && edge.nodeB!=high && edge.useElevation; }
		JSON report; report[U"maximumTownLiftM"]=maximumLift; report[U"elevatedTownEdges"]=elevatedTown;
		report.save(U"TestResults/flat_town_alignment.json");
		context.expect(maximumLift<2.0,U"An infeasible mountain connection must not raise a flat town");
		context.expect(elevatedTown==0,U"Ordinary town streets remain on the ground");
	});

	runner.add(U"RoadDesign.FinalSurfaceGrades", [](TestContext& context)
	{
		AppDirectory directory;
		World world; world.reserveChunks(); world.setGenerationParams(42, WORLD_SIZE, WORLD_SIZE);
		Grid<float> terrain(65, 65, 20);
		for (int z = 0; z <= 64; ++z)
		{
			for (int x = 0; x <= 64; ++x) { terrain[{x,z}] = static_cast<float>(40+32*Sin(x*16*Math::Pi/256)); }
		}
		world.installChunkDirect({32,32}, HeightMapResult{terrain,8,72});
		RoadNetwork roads;
		for (int category = 0; category < 3; ++category)
		{
			int previous = -1;
			for (int sample = 0; sample <= 8; ++sample)
			{
				const Vec3 point{32800+sample*96.0, 40+32*Sin((32+sample*96)*Math::Pi/256), 32900+category*180.0};
				const int id = roads.addNode(point);
				if (previous >= 0)
				{
					const Vec3 start = roads.getNode(previous)->position;
					roads.addEdge(previous, id, start.lerp(point,1.0/3), start.lerp(point,2.0/3), static_cast<RoadType>(category), 2);
				}
				previous = id;
			}
		}
		const auto before = RoadDesignLimits::measure(roads, world);
		context.expect(before.gradeViolations > 10, U"Fixture reproduces terrain-following mountain grades");
		RoadDesignLimits::apply(roads, world);
		RoadTerrainFit::apply(roads, world);
		const auto after = RoadDesignLimits::measure(roads, world);
		for (int category = 0; category < 3; ++category)
		{
			Array<int> frontier{category * 9}; HashSet<int> reached;
			for (size_t i = 0; i < frontier.size(); ++i)
			{
				const int id = frontier[i]; if (reached.contains(id)) { continue; } reached.insert(id);
				if (const auto* node = roads.getNode(id)) for (const auto& attachment : node->attachments)
				{
					const auto* edge = roads.getEdge(attachment.edgeId);
					if (edge) { frontier << (edge->nodeA == id ? edge->nodeB : edge->nodeA); }
				}
			}
			context.expect(reached.contains(category * 9 + 8), U"Cost refinement preserves connectivity between every original pair of endpoints");
		}
		context.expectEqual(after.gradeViolations, 0, U"Final rendered surfaces obey each class after earthworks");
		for (const auto& edge : roads.edges())
		{
			if (edge.id < 0) { continue; }
			context.expect(edge.usesDesignHeight(), U"Ground roads retain their designed vertical profile");
			const auto curve = roads.getBezier(edge.id);
			const Vec3 tangent = curve->tangent(.37f);
			context.expect(Abs(tangent.y)/Vec2{tangent.x,tangent.z}.length() <= RoadDesignLimits::forType(edge.roadType).maximumGrade,
				U"Life streets, arterials and expressways have separate maximum grades");
		}
	});

	runner.add(U"RoadDesign.CurveRadiusAndCoordinatePrecision", [](TestContext& context)
	{
		RoadNetwork roads;
		for (int category = 0; category < 3; ++category)
		{
			const Vec3 a{63000,30,63000+category*200.0}, b = a+Vec3{300,0,0};
			const int start = roads.addNode(a), end = roads.addNode(b);
			const auto id = roads.addEdge(start,end,a+Vec3{0,0,150},b+Vec3{0,0,150},static_cast<RoadType>(category),2);
			context.expect(id.has_value(), U"Curved fixture builds");
			if (!id) { continue; }
			RoadDesignLimits::constrainCurve(roads,*id);
			const auto curve = roads.getBezier(*id);
			context.expect(RoadDesignLimits::minimumRadius(*curve) >= RoadDesignLimits::forType(static_cast<RoadType>(category)).minimumRadius,
				U"Small-radius curves are limited by road class");
		}
		const Vec3 origin{65000,300,64000};
		const CubicBezier tiny{origin, origin+Vec3{.1,.009,0}, origin+Vec3{.2,.018,0}, origin+Vec3{.3,.027,0}};
		for (int index = 0; index <= 100; ++index)
		{
			const float t = index/100.0f;
			context.expect((tiny.evaluate(t)-(origin+Vec3{.3,.027,0}*t)).length() < 1e-8,
				U"Sub-metre roads at the map edge do not gain millimetre-sized vertical kinks");
		}
	});

	runner.add(U"Traffic.AutomaticTripsAndRespawn", [](TestContext& context)
	{
		const FilePath progressPath = FileSystem::CurrentDirectory()+U"TestResults/traffic_progress.txt";
		const auto progress = [&](StringView text) { TextWriter writer{progressPath}; writer << text; };
		progress(U"assets");
		AppDirectory directory;
		progress(U"roads");
		DebugLog::initialize(directory.previous+U"TestResults/traffic_population.log");
		RoadNetwork roads;
		Array<int> nodes;
		for (int index = 0; index < 12; ++index)
		{
			const double angle = index*Math::TwoPi/12;
			nodes << roads.addNode(Vec3{500+320*Cos(angle),20,500+320*Sin(angle)});
		}
		for (int index = 0; index < 12; ++index)
		{
			const int a = nodes[index], b = nodes[(index+1)%12];
			const Vec3 start = roads.getNode(a)->position, end = roads.getNode(b)->position;
			const auto id = roads.addEdge(a,b,start.lerp(end,1.0/3),start.lerp(end,2.0/3),RoadType::LocalRoad,2);
			GeneratedStreet::apply(*roads.getEdge(*id), GeneratedStreet::describe(GeneratedStreet::Role::Local));
			roads.getEdge(*id)->edgeState = EdgeState::Open;
		}
		for (int id : nodes) { roads.updateNodeCutoffs(id); roads.rebuildLaneConnections(id); }
		SimGraph graph = SimGraph::build(roads);
		context.expect(graph.edges.begin()->second.isRoadbedBuilt(), U"Traffic fixture roads are open and built");
		// Include a disconnected, one-way-only road with its first lane closed.
		SimGraph::Edge isolated = graph.edges.begin()->second;
		isolated.id = 10000; isolated.nodeA = 10000; isolated.nodeB = 10001;
		graph.edges[isolated.id] = isolated;
		progress(U"graph");
		TrafficGraph routes; routes.rebuild(graph, 0, {});
		VehicleManager manager; manager.init(graph, roads); manager.setTrafficFocus({500,20,500});
		HashSet<int> visible; for (const auto& edge : roads.edges()) { if (edge.id >= 0) { visible.insert(edge.id); } }
		manager.update(0,0,graph,roads,visible);
		context.expectEqual(manager.vehicleCount(),0,U"Paused time does not create vehicles");
		int maximumId = -1;
		for (int frame = 0; frame < 7200; ++frame)
		{
			if (frame%100 == 0) { progress(U"frame {} update"_fmt(frame)); }
			manager.update(.05,frame*.05,graph,roads,visible);
			if (frame%100 == 0) { progress(U"frame {} route"_fmt(frame)); }
			resolveTrips(manager,graph,routes,context);
			for (const auto& vehicle : manager.vehicles())
			{
				context.expect(vehicle.currentEdge != 10000,U"Isolated roads cannot trap new vehicles");
				context.expect(std::isfinite(vehicle.arcPos) && std::isfinite(vehicle.speed),U"Traffic remains numerically valid");
				maximumId = Max(maximumId,vehicle.id);
			}
		}
		const auto stats = manager.populationStats();
		context.expect(stats.completed >= 10 && stats.spawned > 30 && maximumId > 30,U"Multiple real trips complete and new cars replace them");
		context.expect(manager.vehicleCount() >= 12 && manager.vehicleCount() <= 24,U"Traffic population remains bounded and replenished");
		context.expectEqual(stats.routeFailures,0,U"Spawned trips do not repeatedly fail routing");
		JSON report; report[U"spawned"] = stats.spawned; report[U"completed"] = stats.completed;
		report[U"remaining"] = manager.vehicleCount(); report[U"routeFailures"] = stats.routeFailures;
		report.save(directory.previous+U"TestResults/automatic_traffic.json");
		DebugLog::shutdown();
	});

	runner.add(U"Traffic.SpawnLaneAndVehicleClearance", [](TestContext& context)
	{
		SimGraph::Edge edge; edge.edgeState=EdgeState::Open;
		RoadPart bed; bed.type=RoadPartType::Roadbed; bed.build=BuildState::Built; edge.parts << bed;
		Lane closed, reverse; closed.op=OpState::Closed; reverse.op=OpState::Open; reverse.dir=LaneDir::Backward;
		edge.lanes = {closed,reverse};
		context.expect(!TrafficSpawn::laneOpen(edge,0) && TrafficSpawn::laneOpen(edge,1),U"Closed first lanes and backward-only streets are distinguished");
		Vehicle truck; truck.type=VehicleType::LargeTruck; truck.currentEdge=3; truck.currentLane=1; truck.arcPos=50;
		context.expect(!TrafficSpawn::hasSpace({truck},3,1,60,VehicleType::PassengerCar),U"Spawning checks the actual truck length and safe following gap");
		context.expect(TrafficSpawn::hasSpace({truck},3,0,50,VehicleType::PassengerCar),U"Another lane remains available");
	});
}
