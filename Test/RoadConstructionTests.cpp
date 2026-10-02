#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/road/RoadConstruction.hpp"
#include "src/road/RoadPlanDraft.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/render/CityLighting.hpp"
#include "src/render/BridgeStructure.hpp"
#include "src/ui/ConstructionStatus.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/save/RoadBinary.hpp"

namespace
{
	int makeRoad(RoadNetwork& network, bool elevated)
	{
		const double y=elevated ? 12 : 0;
		const int a=network.addNode({432,y,512}),b=network.addNode({592,y,512});
		const int id=*network.addEdge(a,b,{480,y,506},{544,y,518},RoadType::Arterial,2);
		network.applyEdgeTemplate(id,RoadPlanDraft::makeRoadTemplate(1));
		auto* edge=network.getEdge(id);edge->useElevation=elevated;edge->edgeState=EdgeState::Planned;
		if(elevated) for(float arc:{30.0f,60.0f,90.0f,120.0f})
		{
			RoadObject pier; pier.type=RoadObjectType::Pier; pier.parentEdgeId=id;pier.arcPos=arc;network.addObject(pier);
		}
		return id;
	}
	void flatWorld(World& world)
	{
		world.reserveChunks();
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,0.0f),0.0f,0.0f});
	}
}
void registerRoadConstructionTests(TestRunner& runner)
{
	runner.add(U"Construction.PlayableDuration",[](TestContext& context)
	{
		RoadNetwork network;
		const double duration=network.estimatePlanConstructionDuration(RoadType::LocalRoad,70);
		context.expect(duration>=5 && duration<=10,U"A short local road opens within seconds, despite a 24-minute day");
		context.expectNear(network.estimatePlanConstructionDuration(RoadType::Arterial,1000),120,1e-6,U"One kilometre of arterial takes two minutes at standard speed");
		context.expect(network.estimatePlanConstructionDuration(RoadType::Highway,1000)>120,U"Large roads keep longer construction work");
		GameClock clock;clock.speed=TimeSpeed::Paused;clock.advance(100);
		context.expectNear(clock.now,0,1e-6,U"Pause still stops construction");
		clock.speed=TimeSpeed::x4;clock.advance(duration/4);
		context.expectNear(clock.now,duration,1e-6,U"Speed controls accelerate construction consistently");
		context.expect(ConstructionStatus::durationLabel(duration)==U"約5秒" && ConstructionStatus::durationLabel(120)==U"約2分",U"Estimate shows real waiting time");
	});
	runner.add(U"Construction.TrafficAdmission",[](TestContext& context)
	{
		RoadNetwork network;const int id=makeRoad(network,false);
		RoadPlan plan;plan.edgeIds={id};const int planId=network.addPlan(plan);
		auto graph=SimGraph::build(network);
		context.expect(!graph.getEdge(id)->isRoadbedBuilt(),U"Planned roads cannot receive traffic");
		context.expect(!isPassable(*network.getEdge(id),0),U"Direct lane access must also reject a planned road");
		network.startPlanConstruction(planId,0);
		const auto* edge=network.getEdge(id);
		graph.updateAround({edge->nodeA,edge->nodeB},network);
		context.expect(!graph.getEdge(id)->isRoadbedBuilt(),U"Incremental simulation updates keep unfinished roads closed");
		context.expect(!network.startPlanConstruction(planId,50),U"Repeated start cannot reset time or charge again");
		network.completePlanConstruction(planId);
		graph.updateAround({edge->nodeA,edge->nodeB},network);
		context.expect(graph.getEdge(id)->isRoadbedBuilt(),U"Traffic is admitted after completion");
	});
	runner.add(U"Construction.StageClockAndResume",[](TestContext& context)
	{
		using Stage=RoadConstruction::Stage;
		context.expect(RoadConstruction::progress(-1,100,false).stage==Stage::Clearance,U"A future start cannot advance construction");
		context.expect(RoadConstruction::progress(36,100,false).stage==Stage::BaseCourse,U"Exact ground stage boundary uses the next stage");
		context.expect(RoadConstruction::progress(40,100,true).stage==Stage::BaseCourse,U"Upper structure follows substructure");
		context.expect(RoadConstruction::progress(200,100,true).stage==Stage::Complete,U"A time jump completes all phases");
		RoadNetwork network;const int id=makeRoad(network,false);
		RoadPlan plan;plan.edgeIds={id};const int planId=network.addPlan(plan);
		network.getPlan(planId)->constructionDuration=100;
		network.startPlanConstruction(planId,1000);
		const auto before=RoadConstruction::progress(network,*network.getEdge(id),1093);
		context.expect(before.stage==Stage::Marking,U"An almost completed road is being marked");
		context.expect(RoadBinary::writeGlobal(U"TestResults/construction_roads.bin",network),U"Construction network saves");
		RoadNetwork loaded;context.expect(RoadBinary::readGlobal(U"TestResults/construction_roads.bin",loaded),U"Construction network loads");
		const auto after=RoadConstruction::progress(loaded,*loaded.getEdge(id),1093);
		context.expectNear(after.total,before.total,1e-6,U"The saved clock restores the same phase");
		context.expectNear(after.fraction,before.fraction,1e-6,U"Pause and reload preserve work within the phase");
	});
	runner.add(U"Construction.ClearanceAndPersistentTombstones",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;const int id=makeRoad(network,false);
		auto* chunk=world.getChunk({0,0});
		chunk->buildingGrid[{31,31}].type=BuildingType::Detached;
		chunk->buildingGrid[{31,31}].angle=static_cast<float>(45_deg);
		chunk->buildingGrid[{31,33}].type=BuildingType::Detached;
		chunk->zoneMap[{31,31}]=ZoneType::Residential;
		LandPatch patch;patch.sourceParcelKey=31|(31<<8);chunk->landPatches << patch;
		Stopwatch timer{StartImmediately::Yes};
		const auto cells=RoadConstruction::affectedCells(network,{id},world);
		TextWriter{U"TestResults/construction_clearance_timing.txt"}.write(U"160m curved road: {:.3f}ms; affected cells: {}"_fmt(timer.msF(),cells.size()));
		context.expect(chunk->buildingGrid[{31,31}].type==BuildingType::Detached,U"Planning/estimating never demolishes");
		RoadConstruction::ClearanceLedger ledger;
		context.expectEqual(ledger.clear(world,cells),1,U"Only the intersected rotated building is removed");
		context.expect(chunk->buildingGrid[{31,33}].type==BuildingType::Detached,U"Neighbouring building survives");
		context.expect(chunk->zoneMap[{31,31}]==ZoneType::Unzoned,U"Automatic growth cannot immediately rebuild the cleared cell");
		context.expect(chunk->landPatches.isEmpty(),U"Its garden, fences and lot decorations are removed");
		context.expectEqual(ledger.clear(world,cells),0,U"Reapplying clearance is idempotent");
		context.expect(ledger.save(U"TestResults/clearance.json"),U"Clearance saves independently of road deletion");
		RoadConstruction::ClearanceLedger loaded;
		context.expect(loaded.load(U"TestResults/clearance.json"),U"Clearance loads");
		chunk->buildingGrid[{31,31}].type=BuildingType::Detached;
		loaded.apply(world);
		context.expect(chunk->buildingGrid[{31,31}].type==BuildingType::None,U"Procedural regeneration cannot restore a demolished building");
	});
	runner.add(U"Construction.ElevatedClearance",[](TestContext& context)
	{
		World world;flatWorld(world);RoadNetwork network;const int id=makeRoad(network,true);
		auto* chunk=world.getChunk({0,0});
		chunk->buildingGrid[{31,31}].type=BuildingType::Detached;
		chunk->buildingGrid[{34,31}].type=BuildingType::HighApartment;
		chunk->buildingGrid[{28,31}].type=BuildingType::Detached;
		const auto pier=network.getBezier(id)->positionAt(30);
		chunk->buildingGrid[{28,31}].offsetX=static_cast<float>(pier.x-456);
		chunk->buildingGrid[{28,31}].offsetZ=static_cast<float>(pier.z-504);
		RoadConstruction::ClearanceLedger ledger;
		ledger.clear(world,RoadConstruction::affectedCells(network,{id},world));
		context.expect(chunk->buildingGrid[{31,31}].type==BuildingType::Detached,U"A low building under a clear span survives");
		context.expect(chunk->buildingGrid[{34,31}].type==BuildingType::None,U"A tall building through the deck is demolished");
		context.expect(chunk->buildingGrid[{28,31}].type==BuildingType::None,U"A low building at a pier is demolished");
	});
	runner.add(U"Construction.ParcelKeysAcrossChunks",[](TestContext& context)
	{
		World world;flatWorld(world);
		const Point coord{2,3};
		world.installChunkDirect(coord,HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,0.0f),0.0f,0.0f});
		auto* chunk=world.getChunk(coord);
		chunk->buildingGrid[{31,31}].type=BuildingType::Detached;
		LandPatch owned;owned.sourceParcelKey=(chunkCoordToKey(coord)<<16) ^ (31<<8) ^ 31;
		LandPatch neighbour;neighbour.sourceParcelKey=(chunkCoordToKey(coord)<<16) ^ (32<<8) ^ 31;
		chunk->landPatches << owned << neighbour;
		RoadConstruction::ClearanceLedger ledger;
		ledger.clear(world,{{coord,{31,31},true}});
		context.expect(chunk->landPatches.size()==1 && chunk->landPatches.front().sourceParcelKey==neighbour.sourceParcelKey,U"Demolition clears only the globally keyed plot in the correct chunk");
		context.expect(ledger.save(U"TestResults/clearance_nonzero.json"),U"Nonzero coordinates save");
		RoadConstruction::ClearanceLedger restored;
		context.expect(restored.load(U"TestResults/clearance_nonzero.json"),U"Nonzero coordinates load");
		chunk->buildingGrid[{31,31}].type=BuildingType::Detached;restored.apply(world);
		context.expect(chunk->buildingGrid[{31,31}].type==BuildingType::None,U"Persistent demolition targets the original chunk");
		JSON invalid;invalid[U"count"]=1;invalid[U"cell_0"][U"x"]=-1;invalid.save(U"TestResults/clearance_invalid.json");
		context.expect(!restored.load(U"TestResults/clearance_invalid.json") && restored.size()==1,U"Invalid saves cannot partially replace the ledger");
	});
	runner.add(U"Construction.BridgeSurfaceNormals",[](TestContext& context)
	{
		const Vec3 center{0,12,0};const auto pier=BridgeStructure::pier(center,{1,0,0},0,10.8,14);
		bool valid=true;
		for(const auto& triangle:pier.indices)
		{
			const auto& a=pier.vertices[triangle.i0];const auto& b=pier.vertices[triangle.i1];const auto& c=pier.vertices[triangle.i2];
			const Vec3 cross=(Vec3{b.pos}-Vec3{a.pos}).cross(Vec3{c.pos}-Vec3{a.pos});
			valid=valid && cross.lengthSq()>1e-12 && cross.dot(Vec3{a.normal})>0;
		}
		context.expect(valid,U"Chamfered column and crosshead have nondegenerate outward surfaces");
		RoadNetwork network;const int id=makeRoad(network,true);const auto curve=*network.getBezier(id);
		const auto beam=BridgeStructure::girders(curve,14,curve.totalLength);
		bool topFaces=false,bottomFaces=false;
		for(const auto& v:beam.vertices)
		{
			if(v.normal.y>.9f && v.pos.y>11.8f) topFaces=true;
			if(v.normal.y<-.9f && v.pos.y<10.85f) bottomFaces=true;
		}
		context.expect(topFaces && bottomFaces,U"Girder flange exteriors face above and below the bridge");
	});
	runner.add(U"Construction.CancelledShadowClearsWhilePaused", [](TestContext& context)
	{
		const FilePath testDirectory = FileSystem::CurrentDirectory();
		struct RestoreDirectory
		{
			FilePath path;
			~RestoreDirectory() { FileSystem::ChangeCurrentDirectory(path); }
		} restore{testDirectory};
		FileSystem::ChangeCurrentDirectory(testDirectory + U"../../App/");
		RegisterAssets();
		World world;
		flatWorld(world);
		RoadNetwork network;
		const int edgeId = makeRoad(network, false);
		RoadPlan plan;
		plan.edgeIds = {edgeId};
		const int planId = network.addPlan(plan);
		constexpr GameTime kPausedTime = 6;
		network.getPlan(planId)->constructionDuration = 100;
		context.expect(network.startPlanConstruction(planId, 0), U"The isolated road enters construction");
		const int nodeA = network.getEdge(edgeId)->nodeA;
		const int nodeB = network.getEdge(edgeId)->nodeB;
		RoadRenderer renderer;
		CityLighting lighting;
		const bool assetsReady = renderer.loadAssets();
		const bool lightingReady = lighting.initialize();
		context.expect(assetsReady && lightingReady, U"Production construction models and shadow shaders load");
		if (!assetsReady || !lightingReady) { return; }
		renderer.setConstructionTime(kPausedTime);
		const Size size{480, 320};
		const Vec3 focus{512, 0, 512};
		const Vec3 sun = Vec3{1, 1, 1}.normalized();
		const BasicCamera3D camera{size, 40_deg, Vec3{555, 95, 375}, focus};
		const RenderTexture colorTarget{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		{
			const ScopedRenderTarget3D target{colorTarget.clear(ColorF{0})};
			const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(camera);
			renderer.render(network, world, ViewFrustum{camera, 24000}, camera.getEyePosition());
		}
		Graphics3D::Flush();
		int shadowBuilds = 0;
		const auto occupiedShadowPixels = [&](const BasicCamera3D& view)
		{
			lighting.update(view, focus, sun, 1.0, renderer.geometryRevision(), [&](Vec3 center, double radius)
			{
				++shadowBuilds;
				renderer.renderShadowCasters(center, radius);
			});
			Grid<float> depth;
			lighting.readStaticShadowDepth(depth);
			int64 occupied = 0;
			for (const float value : depth) { occupied += value > 0.0f; }
			return occupied;
		};
		const int64 beforePixels = occupiedShadowPixels(camera);
		context.expect(beforePixels > 100, U"The real paused construction site casts measurable shadow depth");
		context.expectEqual(occupiedShadowPixels(camera), beforePixels, U"An unchanged paused view preserves its shadow texture");
		context.expectEqual(shadowBuilds, 1, U"An unchanged paused view reuses the static shadow cache");
		const uint64 beforeRevision = renderer.geometryRevision();
		// Cancellation must invalidate the edge itself before its node attachments are removed.
		renderer.invalidateEdgeCache(edgeId, nodeA, nodeB);
		network.removeEdge(edgeId);
		network.removePlan(planId);
		renderer.invalidateCachesAroundNode(nodeA, network);
		renderer.invalidateCachesAroundNode(nodeB, network);
		const uint64 cancelledRevision = renderer.geometryRevision();
		const int64 cancelledPixels = occupiedShadowPixels(camera);
		const int cancelledBuilds = shadowBuilds;
		context.expect(!network.getEdge(edgeId) && !network.getPlan(planId), U"Cancellation removes the construction edge and its plan");
		context.expect(cancelledRevision > beforeRevision, U"Removing construction-only geometry invalidates the static shadow revision");
		context.expectEqual(cancelledPixels, 0, U"Cancellation clears real shadow depth without advancing the clock, sun, or camera");
		context.expectEqual(cancelledBuilds, 2, U"Cancellation rebuilds the static shadow texture exactly once");
		renderer.invalidateEdgeCache(edgeId, nodeA, nodeB);
		context.expect(renderer.geometryRevision() == cancelledRevision, U"Repeated invalidation of the removed edge does not dirty the shadow cache");
		context.expectEqual(occupiedShadowPixels(camera), 0, U"The next paused frame stays free of the cancelled construction shadow");
		context.expectEqual(shadowBuilds, cancelledBuilds, U"The cleared static shadow texture is reused while paused");
		const BasicCamera3D movedCamera{size, 40_deg, Vec3{555, 130, 330}, focus};
		const int64 movedPixels = occupiedShadowPixels(movedCamera);
		context.expectEqual(movedPixels, 0, U"Moving the camera cannot recast deleted construction geometry");
		context.expectEqual(shadowBuilds, cancelledBuilds + 1, U"A changed camera rebuilds once without retaining removed casters");
		JSON report;
		report[U"pausedTime"] = kPausedTime;
		report[U"beforeRevision"] = beforeRevision;
		report[U"cancelledRevision"] = cancelledRevision;
		report[U"beforeShadowPixels"] = beforePixels;
		report[U"cancelledShadowPixels"] = cancelledPixels;
		report[U"movedShadowPixels"] = movedPixels;
		report[U"cancelledShadowBuilds"] = cancelledBuilds;
		report[U"totalShadowBuilds"] = shadowBuilds;
		context.expect(report.save(testDirectory + U"TestResults/construction_cancelled_shadow.json"), U"Construction shadow readback measurements save");
	});
	runner.add(U"Construction.StagesVisualReview",[](TestContext& context)
	{
		const FilePath testDirectory=FileSystem::CurrentDirectory();
		struct Restore {FilePath path;~Restore(){FileSystem::ChangeCurrentDirectory(path);}} restore{testDirectory};
		FileSystem::ChangeCurrentDirectory(testDirectory+U"../../App/");RegisterAssets();
		Font font{FontMethod::MSDF,14};
		for(int elevated=0;elevated<2;++elevated)
		{
			World world;flatWorld(world);RoadNetwork network;const int id=makeRoad(network,elevated!=0);
			RoadPlan plan;plan.edgeIds={id};const int planId=network.addPlan(plan);
			network.getPlan(planId)->constructionDuration=100;
			network.startPlanConstruction(planId,0);
			RoadRenderer renderer;context.expect(renderer.loadAssets(),U"Construction uses production road parts and asphalt");
			const Size size{1100,720};
			const BasicCamera3D camera{size,40_deg,Vec3{555,95,375},Vec3{512,4,512}};
			const std::array<double,6> groundTimes{6,25,48,73,95,100};
			const std::array<double,6> bridgeTimes{5,28,58,82,96,100};
			for(int phase=0;phase<6;++phase)
			{
				const double now=(elevated ? bridgeTimes : groundTimes)[phase];
				if(phase==5) {network.completePlanConstruction(planId);renderer.invalidateAllCaches();}
				renderer.setConstructionTime(now);
				RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
				{
					const ScopedRenderTarget3D renderTarget{target.clear(ColorF{.53,.66,.77})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
					Graphics3D::SetCameraTransform(camera);Graphics3D::SetSunDirection(Vec3{1,2,-1}.normalized());
					Graphics3D::SetGlobalAmbientColor(ColorF{.5});
					Box{Vec3{512,-.45,512},Vec3{300,.2,250}}.draw(ColorF{.19,.25,.15}.removeSRGBCurve());
					renderer.render(network,world,ViewFrustum{camera,24000},camera.getEyePosition());
				}
				Graphics3D::Flush();
				{
					const ScopedRenderTarget2D ui{target};
					ConstructionStatus::draw(font,RectF{20,20,360,70},RoadConstruction::progress(now,100,elevated!=0));
				}
				Graphics2D::Flush();Image screenshot;target.readAsImage(screenshot);
				context.expect(screenshot.save(testDirectory+U"Screenshot/construction_{}_{}.png"_fmt(elevated,phase)),U"Review each real construction stage before UI integration");
				if(phase==1 || phase==2 || phase==3 || phase==5)
				{
					const Vec3 focus=network.getBezier(id)->positionAt(90)+Vec3{0,elevated ? -5.0 : 0,0};
					const BasicCamera3D detailCamera{size,40_deg,focus+Vec3{18,14,-24},focus};
					{
						const ScopedRenderTarget3D renderTarget{target.clear(ColorF{.53,.66,.77})};
						const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
						Graphics3D::SetCameraTransform(detailCamera);
						Box{Vec3{512,-.45,512},Vec3{300,.2,250}}.draw(ColorF{.19,.25,.15}.removeSRGBCurve());
						renderer.render(network,world,ViewFrustum{detailCamera,24000},detailCamera.getEyePosition());
					}
					Graphics3D::Flush();target.readAsImage(screenshot);
					context.expect(screenshot.save(testDirectory+U"Screenshot/construction_detail_{}_{}.png"_fmt(elevated,phase)),U"Inspect machinery, unfinished surface and piers at street distance");
				}

			}
		}
	});
}
