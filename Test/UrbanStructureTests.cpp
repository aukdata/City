#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "UrbanEmptyFaceDiagnostics.hpp"
#include "UrbanGroundPreview.hpp"
#include "VillageGutterTests.hpp"
#include "src/render/ResidentialParcelAccess.hpp"
#include "src/render/FrontageGeometry.hpp"
#include <map>
#include "src/gen/SettlementPlan.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "src/gen/SettlementPlacement.hpp"
#include "src/gen/DistrictRoads.hpp"
#include "src/gen/RailwayAlignment.hpp"
#include "src/world/ZoneGrid.hpp"
#include "src/gen/ParcelGeometry.hpp"
#include "src/gen/ParcelRoadIndex.hpp"
#include "src/gen/StreetBlocks.hpp"
#include "src/gen/RoadAlignment.hpp"
#include "src/gen/UrbanMosaic.hpp"
#include "src/gen/UrbanPocketGreen.hpp"
#include "src/gen/RoadDesignLimits.hpp"
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
	void installFlat(World& world, uint64 seed = 42)
	{
		world.reserveChunks();
		world.setGenerationParams(seed, WORLD_SIZE, WORLD_SIZE);
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
	VillageGutterTests::registerTests(runner);
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
	runner.add(U"UrbanStructure.StreetFaceBuildability", [](TestContext& context)
	{
		for (const double phase : {.5,8.2}) for (int shape=0;shape<5;++shape)
		{
			RoadNetwork roads;
			RoadEdge prototype; GeneratedStreet::apply(prototype,GeneratedStreet::describe(GeneratedStreet::Role::Village));
			const auto range=RoadGeometry::structuralRangeAt(prototype,0); const float roadMargin=GenerationSettings::get().parcels_roadMargin;
			const double roadHalf=Max(-range.left+roadMargin,range.right+roadMargin);
			const double legalHalf=buildingFootprintXZ(BuildingType::Detached)*.5+GenerationSettings::get().development_footprintMargin;
			const double touchHalf=Ceil(legalHalf); // Integer radius gives an exact touching case without reducing the legal footprint.
			const double size=shape==0 ? 8 : shape==1 ? 24 : shape==2 ? 60 : shape==3 ? 2*(roadHalf+legalHalf)-.2 : 2*(roadHalf+touchHalf);
			Array<Vec2> outline{{32768+phase,32768+phase},{32768+phase+size,32768+phase},{32768+phase,32768+phase+size}};
			if (shape>=2) { outline={{32768+phase,32768+phase},{32768+phase+size,32768+phase},{32768+phase+size,32768+phase+size},{32768+phase,32768+phase+size}}; }
			Array<int> nodes; for (const Vec2 point : outline) { nodes << roads.addNode({point.x,20,point.y}); }
			StreetBlocks::Block face; face.outline=outline;
			for (size_t i=0;i<nodes.size();++i)
			{
				const Vec3 a=roads.getNode(nodes[i])->position,b=roads.getNode(nodes[(i+1)%nodes.size()])->position;
				const int id=*roads.addEdge(nodes[i],nodes[(i+1)%nodes.size()],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
				GeneratedStreet::apply(*roads.getEdge(id),GeneratedStreet::describe(GeneratedStreet::Role::Village)); face.edges << id;
			}
			const auto open=StreetBlocks::freeCenterArea(roads,face,0),centers=StreetBlocks::freeCenterArea(roads,face,buildingFootprintXZ(BuildingType::Detached)*.5);
			context.expect(open.has_value() && centers.has_value(),U"Valid street faces provide exact polygon-area diagnostics");
			if (!open || !centers) { continue; }
			if (shape==0) { context.expect(*open<.0001 && *centers<.0001,U"A road-covered junction triangle is not a vacant building parcel"); }
			if (shape==1) { context.expect(*open>1 && *centers<.0001,U"A small open triangular pocket has land but no legal minimum-home center"); }
			if (shape==2) { context.expect(*centers>100,U"A genuinely developable empty block is never excused as a pocket"); }
			if (shape==4)
			{
				const double half=touchHalf;
				const Vec2 center=outline.front()+Vec2{size*.5,size*.5};
				context.expect(!ParcelRoadIndex{roads}.overlaps(ParcelGeometry::footprint(center,half,0)),U"A legal touching square remains accepted by the unchanged road validator");
				const auto touching=StreetBlocks::freeCenterArea(roads,face,half);
				context.expect(touching && *touching>0,U"Tiny positive center regions are retained instead of exempted by an area epsilon");
			}
			if (shape==3)
			{
				const double required=buildingFootprintXZ(BuildingType::Detached)*.5+GenerationSettings::get().development_footprintMargin;
				const auto legalCenters=StreetBlocks::freeCenterArea(roads,face,required);
				context.expect(*centers>0 && legalCenters && *legalCenters<.0001,U"Existing footprint and road margins distinguish a bare-square fit from a legally buildable parcel");
			}

		}
	});
	runner.add(U"UrbanStructure.CurvedAndTaperedFaceBounds", [](TestContext& context)
	{
		RoadNetwork roads;
		const Vec2 origin{32768.2,32768.2};
		const Array<Vec2> outline{origin,origin+Vec2{24,0},origin+Vec2{0,24}};
		Array<int> nodes; for (const Vec2 point : outline) { nodes << roads.addNode({point.x,20,point.y}); }
		StreetBlocks::Block face; face.outline=outline;
		for (size_t i=0;i<nodes.size();++i)
		{
			const Vec3 a=roads.getNode(nodes[i])->position,b=roads.getNode(nodes[(i+1)%nodes.size()])->position;
			const Vec3 bow=i==1 ? Vec3{8,0,8} : Vec3{0,0,0};
			const int id=*roads.addEdge(nodes[i],nodes[(i+1)%nodes.size()],a.lerp(b,1.0/3)+bow,a.lerp(b,2.0/3)+bow,RoadType::LocalRoad,2);
			GeneratedStreet::apply(*roads.getEdge(id),GeneratedStreet::describe(GeneratedStreet::Role::Village)); face.edges << id;
		}
		const auto curved=StreetBlocks::freeCenterArea(roads,face,buildingFootprintXZ(BuildingType::Detached)*.5);
		context.expect(curved && *curved>1,U"A coarse triangular outline cannot erase usable land inside its outward-curving boundary");
		for (auto& part : roads.getEdge(face.edges.front())->parts)
		{
			if (RoadGeometry::isStructuralStrip(part)) { part.offsetB_L-=1; break; }
		}
		const double requiredHalf=buildingFootprintXZ(BuildingType::Detached)*.5+GenerationSettings::get().development_footprintMargin;
		const auto tapered=StreetBlocks::freeCenterArea(roads,face,requiredHalf); const ParcelRoadIndex index{roads}; int legalRotations=0;
		for (const double angle : {0.0,15_deg,30_deg,45_deg})
		{
			if (!index.overlaps(ParcelGeometry::footprint(origin+Vec2{10.5,10.5},requiredHalf,angle)))
			{
				++legalRotations;
				context.expect(tapered && *tapered>0,U"Exact tapered ribbon quads never exempt a face containing a validator-legal rotated square");
			}
		}
		context.expect(legalRotations>=2,U"The curved/tapered fixture has multiple actually legal rotated candidates");
		for (auto& part : roads.getEdge(face.edges.front())->parts)
		{
			part.offsetB_L=part.offsetA_L;
			part.offsetA_R+=1; part.offsetB_R=part.offsetA_R;
		}
		const auto asymmetric=StreetBlocks::freeCenterArea(roads,face,4.5);
		context.expect(asymmetric && *asymmetric>0,U"Constant asymmetric sections use their smaller safe envelope");
		roads.getEdge(face.edges.front())->ctrlA=roads.getNode(roads.getEdge(face.edges.front())->nodeA)->position;
		context.expect(!StreetBlocks::freeCenterArea(roads,face,4.5),U"A degenerate endpoint handle returns unknown rather than using a fallback tangent");
	});
	runner.add(U"UrbanStructure.FlatRoadCapsKeepAvailableLand", [](TestContext& context)
	{
		RoadNetwork roads; const Vec2 origin{32768.2,32768.2};
		const Array<Vec2> local{{0,0},{5,0},{5,10},{15,10},{15,20.2},{0,20.2}};
		StreetBlocks::Block face; Array<int> nodes;
		for (const Vec2 point : local) { face.outline << point+origin; nodes << roads.addNode({point.x+origin.x,20,point.y+origin.y}); }
		for (size_t i=0;i<nodes.size();++i)
		{
			const Vec3 a=roads.getNode(nodes[i])->position,b=roads.getNode(nodes[(i+1)%nodes.size()])->position;
			const int id=*roads.addEdge(nodes[i],nodes[(i+1)%nodes.size()],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
			auto* edge=roads.getEdge(id); GeneratedStreet::apply(*edge,GeneratedStreet::describe(GeneratedStreet::Role::Village));
			const double halfWidth=i==1 ? 3.5 : .2;
			const float scale=static_cast<float>(halfWidth*2/RoadGeometry::structuralWidth(*edge));
			for (auto& part : edge->parts) { part.offsetA_L*=scale; part.offsetB_L*=scale; part.offsetA_R*=scale; part.offsetB_R*=scale; }
			edge->edgeState=EdgeState::Existing; face.edges << id;
		}
		const Polygon actualFace{face.outline}; const Vec2 legalCenter=origin+Vec2{8,15.1};
		for (const Vec2 offset : {Vec2{-4.5,-4.5},Vec2{4.5,-4.5},Vec2{4.5,4.5},Vec2{-4.5,4.5}})
		{
			context.expect(actualFace.contains(legalCenter+offset),U"The counterexample's minimum home is inside the concave street face");
		}
		context.expect(!ParcelRoadIndex{roads}.overlaps(ParcelGeometry::footprint(legalCenter,4.5,0)),U"The cap counterexample clears the actual road ribbons and margin");
		const auto centers=StreetBlocks::freeCenterArea(roads,face,4.5);
		context.expect(centers && *centers>0,U"A wide road's round endpoint cap must not erase legal land beyond its flat end");
	});
	runner.add(U"UrbanStructure.ContinuousFrontageParcel", [](TestContext& context)
	{
		Fixture fixture; World world; installFlat(world,42); RoadNetwork roads;
		Array<int> nodes,edges;
		for (const double x : {-30.0,-10.0,0.0,10.0,30.0}) { nodes << roads.addNode({32768+x,20,32768}); }
		for (size_t i=1;i<nodes.size();++i)
		{
			const Vec3 a=roads.getNode(nodes[i-1])->position,b=roads.getNode(nodes[i])->position;
			const int id=*roads.addEdge(nodes[i-1],nodes[i],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
			GeneratedStreet::apply(*roads.getEdge(id),GeneratedStreet::describe(GeneratedStreet::Role::Village)); edges << id;
		}
		const int id=edges[1]; const auto curve=roads.getBezier(id); const Vec2 span=StreetBlocks::frontageSpan(roads,id);
		context.expect(span.x<span.y && span.x<1 && span.y>9,U"A 10m fitted piece keeps frontage through its tangent-continuous ends");
		const Vec3 point=curve->positionAt(static_cast<float>(span.y));
		const double half=buildingFootprintXZ(BuildingType::Detached)*.5;
		const double setback=GenerationSettings::get().development_minimumRoadSetback;
		const Vec2 position{point.x,point.z-RoadGeometry::structuralWidth(*roads.getEdge(id))*.5-half-setback};
		const auto footprint=ParcelGeometry::footprint(position,half+GenerationSettings::get().development_footprintMargin,Math::Pi);
		context.expect((footprint[0].x<32768 && footprint[2].x>32768) || (footprint[2].x<32768 && footprint[0].x>32768),U"The legal parcel genuinely spans a fitted-piece subdivision");
		context.expect(!ParcelRoadIndex{roads}.overlaps(footprint),U"The unchanged 9m home and footprint margin clear every road ribbon");
		Point coord; int col,row; ZoneGrid::worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
		Building building; building.type=BuildingType::Detached; building.angle=static_cast<float>(Math::Pi); building.edgeId=id; building.edgeT=curve->tFromArcLength(static_cast<float>(span.y));
		const Vec2 cell=ZoneGrid::cellCenterXZ(coord,col,row); building.offsetX=static_cast<float>(position.x-cell.x); building.offsetZ=static_cast<float>(position.y-cell.y);
		world.getChunk(coord)->buildingGrid[{col,row}]=building; world.getChunk(coord)->zoneMap[{col,row}]=ZoneType::LowResidential;
		MapGenerator::Settlement town; town.center={32768,32768}; town.kind=MapGenerator::SettlementKind::RegionalCity;
		town.plan=UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle,0,{},42,true); UrbanStructure::apply(town.plan,Type::HistoricGrid);
		const Array<MapGenerator::Settlement> towns{town}; const TrainNetwork trains;
		SettlementDevelopment development{world,roads,trains,towns,42}; development.generateLandPatches();
		const auto validation=development.validateGeneratedCityConstraints();
		context.expect(validation.passed,U"The cross-subdivision parcel retains real access, parcel and overlap constraints: "+validation.summary);
		const int branch=roads.addNode({32768,20,32798});
		roads.addEdge(nodes[2],branch,{32768,20,32778},{32768,20,32788},RoadType::LocalRoad,2);
		const Vec2 junctionSpan=StreetBlocks::frontageSpan(roads,id);
		context.expectNear(junctionSpan.y,curve->totalLength-roads.getEdge(id)->cutoffB-9,.001,U"A real third arm retains the original intersection exclusion");
		RoadNetwork shortPiece; Array<int> chainNodes,chainEdges;
		for (const double x : {-20.0,0.0,2.0,12.0,40.0}) { chainNodes << shortPiece.addNode({x,0,0}); }
		for (size_t i=1;i<chainNodes.size();++i)
		{
			const Vec3 a=shortPiece.getNode(chainNodes[i-1])->position,b=shortPiece.getNode(chainNodes[i])->position;
			chainEdges << *shortPiece.addEdge(chainNodes[i-1],chainNodes[i],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
		}
		const int arm=shortPiece.addNode({0,0,20}); shortPiece.addEdge(chainNodes[1],arm,{0,0,20.0/3},{0,0,40.0/3},RoadType::LocalRoad,2);
		const Vec2 propagated=StreetBlocks::frontageSpan(shortPiece,chainEdges[2]);
		const double originalExclusion=shortPiece.getEdge(chainEdges[1])->cutoffA+9.0;
		context.expect(propagated.x+2>=originalExclusion-.001,U"A 2m split descendant propagates the real junction exclusion onto the next fitted piece");
		RoadNetwork corner; const int a=corner.addNode({0,0,0}),b=corner.addNode({10,0,0}),c=corner.addNode({10,0,10});
		const int first=*corner.addEdge(a,b,{10.0/3,0,0},{20.0/3,0,0},RoadType::LocalRoad,2);
		corner.addEdge(b,c,{10,0,10.0/3},{10,0,20.0/3},RoadType::LocalRoad,2);
		context.expect(StreetBlocks::frontageSpan(corner,first).y<=1,U"A sharp two-arm corner is not treated as continuous frontage");
	});
	/// @brief Acute same-level departures are checked across split pieces, at either path end.
	runner.add(U"UrbanStructure.SharedDepartureIsBounded", [](TestContext& context)
	{
		RoadEdge profile; GeneratedStreet::apply(profile,GeneratedStreet::describe(GeneratedStreet::Role::Village));
		const auto line=[](Vec3 a,Vec3 b) { return CubicBezier{a,a.lerp(b,1.0/3),a.lerp(b,2.0/3),b}; };
		const auto append=[&](RoadNetwork& roads,int from,int to)
		{
			const Vec3 a=roads.getNode(from)->position,b=roads.getNode(to)->position;
			const int id=*roads.addEdge(from,to,a.lerp(b,1.0/3),a.lerp(b,2.0/3),profile.roadType,static_cast<int>(profile.lanes.size()));
			roads.applyEdgeTemplate(id,profile); roads.getEdge(id)->edgeState=EdgeState::Existing;
			return id;
		};
		RoadNetwork roads; Array<int> nodes,edges;
		for (int i=0;i<=6;++i) { nodes << roads.addNode({i*25.0,20,0}); }
		for (size_t i=1;i<nodes.size();++i) { edges << append(roads,i%2 ? nodes[i-1] : nodes[i],i%2 ? nodes[i] : nodes[i-1]); }
		// A genuine crossing must not hide the unique straight same-profile continuation.
		const int side=roads.addNode({25,20,-40}); append(roads,nodes[1],side);
		const Vec3 gateway=roads.getNode(nodes.front())->position;
		const Vec3 acuteEnd=gateway+Vec3{Cos(12_deg)*100,0,Sin(12_deg)*100};
		const CubicBezier acute=line(gateway,acuteEnd);
		context.expect(RoadAlignment::respectsLimits(acute,profile.roadType),U"The rejected wedge is physically valid; its defect is the long narrow street face");
		const int nextNode=roads.nextNodeId(),nextEdge=roads.nextEdgeId();
		Array<CubicBezier> before; for (const int id : edges) { before << *roads.getBezier(id); }
		const auto conflict=UrbanDeparture::findConflict(roads,Array<CubicBezier>{acute},profile);
		context.expect(conflict && !conflict->atFinish,U"A 12-degree 100m wedge is detected from its shared start");
		if (conflict)
		{
			context.expect(conflict->closeLength>25,U"The check follows existing 25m split pieces instead of stopping at the first child");
			const double expected=RoadGeometry::structuralWidth(profile)+2*(buildingFootprintXZ(BuildingType::Detached)*.5
				+GenerationSettings::get().development_footprintMargin+GenerationSettings::get().parcels_roadMargin);
			context.expectNear(conflict->requiredSeparation,expected,.02,U"Required spacing derives from actual road envelopes and the full legal house");
		}
		const CubicBezier reversed{acute.p3,acute.p2,acute.p1,acute.p0};
		const auto finishConflict=UrbanDeparture::findConflict(roads,Array<CubicBezier>{reversed},profile);
		context.expect(finishConflict && finishConflict->atFinish,U"The same wedge is detected at the proposed path's finish");
		context.expect(!UrbanDeparture::findConflict(roads,Array<CubicBezier>{line(gateway,gateway+Vec3{0,0,100})},profile),U"A useful perpendicular T-junction remains allowed");
		context.expect(!UrbanDeparture::findConflict(roads,Array<CubicBezier>{line(gateway,gateway+Vec3{-100,0,0})},profile),U"Opposite-direction continuation remains allowed; outgoing dot is not absolute");
		const Vec3 parallelStart=gateway+Vec3{0,0,12};
		context.expect(!UrbanDeparture::findConflict(roads,Array<CubicBezier>{line(parallelStart,parallelStart+Vec3{100,0,0})},profile),U"Nearby parallel roads without a shared gateway are outside this narrow departure rule");
		context.expectEqual(roads.nextNodeId(),nextNode,U"The pure check never allocates nodes");
		context.expectEqual(roads.nextEdgeId(),nextEdge,U"The pure check never allocates edges");
		for (size_t i=0;i<edges.size();++i)
		{
			const auto after=*roads.getBezier(edges[i]);
			context.expect(before[i].p0==after.p0 && before[i].p1==after.p1 && before[i].p2==after.p2 && before[i].p3==after.p3,U"Existing road geometry stays unchanged");
		}
		RoadNetwork interior;
		const int a=interior.addNode({0,20,0}),b=interior.addNode({200,20,0}); append(interior,a,b);
		const Vec3 middle{50,20,0}; const CubicBezier interiorWedge=line(middle,middle+Vec3{Cos(11_deg)*100,0,Sin(11_deg)*100});
		context.expect(UrbanDeparture::findConflict(interior,Array<CubicBezier>{interiorWedge},profile).has_value(),U"An 11-degree departure is caught at an unsplit edge-interior gateway too");
		context.expect(!UrbanDeparture::findConflict(interior,Array<CubicBezier>{line(middle,middle+Vec3{0,0,100})},profile),U"A projected interior T-junction remains allowed");
	});

	/// @brief A physically valid fork that opens useful frontage is not rejected as parallel.
	runner.add(U"UrbanStructure.SharedDepartureKeepsUsefulParallelStreet", [](TestContext& context)
	{
		Fixture fixture; World world; installFlat(world,42); RoadNetwork roads;
		RoadEdge profile; GeneratedStreet::apply(profile,GeneratedStreet::describe(GeneratedStreet::Role::Village));
		const Vec3 origin{32780,20,32780};
		Array<int> nodes;
		for (int i=0;i<=8;++i) { nodes << roads.addNode(origin+Vec3{i*25.0,0,0}); }
		for (size_t i=1;i<nodes.size();++i)
		{
			const Vec3 a=roads.getNode(nodes[i-1])->position,b=roads.getNode(nodes[i])->position;
			const int id=*roads.addEdge(nodes[i-1],nodes[i],a.lerp(b,1.0/3),a.lerp(b,2.0/3),profile.roadType,static_cast<int>(profile.lanes.size()));
			roads.applyEdgeTemplate(id,profile); roads.getEdge(id)->edgeState=EdgeState::Existing;
		}
		// Starts at the same 12-degree angle as the negative fixture, then opens to
		// 20m spacing. Its smooth transition has a minimum radius above 20m.
		const CubicBezier departure{origin,origin+Vec3{15,0,15*Tan(12_deg)},origin+Vec3{20,0,20},origin+Vec3{40,0,20}};
		const Vec3 end=origin+Vec3{140,0,20};
		const CubicBezier parallel{departure.p3,departure.p3.lerp(end,1.0/3),departure.p3.lerp(end,2.0/3),end};
		const Array<CubicBezier> proposed{departure,parallel};
		for (const auto& curve : proposed)
		{
			context.expect(RoadAlignment::respectsLimits(curve,profile.roadType),U"The useful fork meets unchanged full-curve grade and minimum-radius limits");
		}
		context.expect(RoadAlignment::maximumClearance(world,proposed)<=2.5,U"The useful fork remains on the flat ground");
		context.expect(!UrbanDeparture::findConflict(roads,proposed,profile),U"A fork that opens usable 20m-spaced parallel frontage is retained");
		int previous=nodes.front();
		for (const auto& curve : proposed)
		{
			const int next=roads.addNode(curve.p3);
			const int id=*roads.addEdge(previous,next,curve.p1,curve.p2,profile.roadType,static_cast<int>(profile.lanes.size()));
			roads.applyEdgeTemplate(id,profile); roads.getEdge(id)->edgeState=EdgeState::Existing; roads.getEdge(id)->designGrade=true;
			previous=next;
		}
		const Vec2 witness{origin.x+90,origin.z+10};
		const double half=buildingFootprintXZ(BuildingType::Detached)*.5+GenerationSettings::get().development_footprintMargin;
		const auto footprint=ParcelGeometry::footprint(witness,half,0);
		context.expect(!ParcelRoadIndex{roads,true}.overlaps(footprint),U"The retained parallel strip admits a real 9m home with unchanged footprint and road margins");
		for (const Vec2 corner : footprint)
		{
			context.expect(world.sampleHeight(static_cast<float>(corner.x),static_cast<float>(corner.y))
				>=world.waterSurfaceHeight(corner.x,corner.y)+GenerationSettings::get().development_buildingFreeboard,U"The positive home's complete footprint is dry on real terrain");
		}
	});
	/// @brief Interior intersections cannot hide a narrow rejoined loop, or forbid a useful crossing.
	runner.add(U"UrbanStructure.RejoinedDepartureAfterIntersections", [](TestContext& context)
	{
		Fixture fixture; World world; installFlat(world,42);
		RoadEdge profile; GeneratedStreet::apply(profile,GeneratedStreet::describe(GeneratedStreet::Role::Village));
		const Vec3 origin{32780,20,32780};
		const auto line=[](Vec3 a,Vec3 b) { return CubicBezier{a,a.lerp(b,1.0/3),a.lerp(b,2.0/3),b}; };
		const CubicBezier lens{origin+Vec3{-60,0,-60},origin+Vec3{40,0,28},origin+Vec3{160,0,28},origin+Vec3{260,0,-60}};
		const CubicBezier opening{origin,origin+Vec3{15,0,15*Tan(12_deg)},origin+Vec3{20,0,20},origin+Vec3{40,0,20}};
		const CubicBezier returning{origin+Vec3{140,0,20},origin+Vec3{160,0,20},origin+Vec3{165,0,15*Tan(12_deg)},origin+Vec3{180,0,0}};
		const CubicBezier approach=line(origin+Vec3{-40,0,-40*Tan(12_deg)},origin);
		const CubicBezier parallel=line(opening.p3,returning.p0);
		const CubicBezier exit=line(returning.p3,origin+Vec3{220,0,-40*Tan(12_deg)});
		struct Case { String name; Array<CubicBezier> curves; bool rejected=false; int shared=0; bool home=false; };
		const Array<Case> cases{
			{U"shallow lens",{lens},true,2,false},
			{U"isolated acute X",{line(origin+Vec3{-60,0,-15},origin+Vec3{260,0,65})},false,1,false},
			{U"useful rejoined parallel",{approach,opening,parallel,returning,exit},false,2,true},
			{U"useful open parallel",{approach,opening,parallel},false,1,true}
		};
		for (const auto& test : cases)
		{
			RoadNetwork roads; Array<int> hostNodes,hostEdges;
			for (int i=0;i<=16;++i) { hostNodes << roads.addNode(origin+Vec3{-100+i*25.0,0,0}); }
			for (size_t i=1;i<hostNodes.size();++i)
			{
				const bool forward=i%2!=0; const int from=hostNodes[forward ? i-1 : i],to=hostNodes[forward ? i : i-1];
				const Vec3 a=roads.getNode(from)->position,b=roads.getNode(to)->position;
				const int id=*roads.addEdge(from,to,a.lerp(b,1.0/3),a.lerp(b,2.0/3),profile.roadType,static_cast<int>(profile.lanes.size()));
				roads.applyEdgeTemplate(id,profile); roads.getEdge(id)->edgeState=EdgeState::Existing; roads.getEdge(id)->designGrade=true; hostEdges << id;
			}
			const int hostRoute=roads.addRoute(RoadRouteKind::CityRoute,U"検証既存通り",hostEdges);
			context.expect(!UrbanDeparture::findConflict(roads,test.curves,profile),test.name+U": original endpoints are independent of the existing street");
			Array<CubicBezier> pieces;
			for (auto curve : test.curves)
			{
				context.expect(RoadAlignment::respectsLimits(curve,profile.roadType),test.name+U": each fitted curve meets the unchanged full-curve limits");
				// Match normal terrain-fit subdivisions. One unsplit double-crossing curve
				// can become adjacent to its host after the first resolved intersection.
				const int count=Max(1,static_cast<int>(Ceil(curve.totalLength/12.0)));
				for (int remaining=count;remaining>1;--remaining)
				{
					const auto pair=curve.split(1.0f/remaining); pieces << pair.first; curve=pair.second;
				}
				pieces << curve;
			}
			const int firstEdge=roads.nextEdgeId();
			Array<int> added=RoadAutoPlace::buildAlignment(roads,world,pieces,profile,.5f);
			context.expect(!added.isEmpty(),test.name+U": the real road builder accepts the fitted path");
			if (added.isEmpty()) { continue; }
			roads.resolveIntersections(firstEdge,&added);
			context.expect(UrbanMosaic::validNewGeometry(roads,firstEdge),test.name+U": every new and split descendant still meets physical limits");
			for (const int id : added) { roads.getEdge(id)->edgeState=EdgeState::Existing; }
			const HashSet<int> candidateIds{added.begin(),added.end()}; HashSet<int> junctions;
			for (const int id : added)
			{
				const auto* edge=roads.getEdge(id);
				for (const int node : {edge->nodeA,edge->nodeB})
				{
					for (const auto& attachment : roads.getNode(node)->attachments)
					{
						if (!candidateIds.contains(attachment.edgeId)) { junctions.insert(node); }
					}
				}
			}
			context.expectEqual(junctions.size(),test.shared,test.name+U": expected real shared junctions exist after intersection splitting");
			if (junctions.size()!=static_cast<size_t>(test.shared)) { continue; }
			if (test.rejected)
			{
				context.expect(roads.getRoute(hostRoute)->edgeIds.any([&](int id) { return id>=firstEdge && !candidateIds.contains(id); }),U"New host-child IDs remain existing-corridor ownership, not candidate ownership");
			}
			const int nextNode=roads.nextNodeId(),nextEdge=roads.nextEdgeId(); const auto routeBefore=roads.getRoute(hostRoute)->edgeIds;
			Array<int> ids; Array<CubicBezier> before;
			for (const auto& edge : roads.edges()) { if (edge.id>=0) { ids << edge.id; before << *roads.getBezier(edge.id); } }
			const auto conflict=UrbanDeparture::findRejoinedConflict(roads,added);
			context.expect(conflict.has_value()==test.rejected,test.name+U": only the sustained narrow rejoined departure is rejected");
			if (conflict)
			{
				context.expect(junctions.contains(conflict->startNode) && junctions.contains(conflict->rejoinNode)
					&& conflict->startNode!=conflict->rejoinNode,U"The rejected loop is bounded by two distinct real shared nodes");
				context.expect(!candidateIds.contains(conflict->departure.existingEdge),U"Candidate descendants never masquerade as the existing corridor");
				context.expect(conflict->departure.closeLength>33,U"The rejection covers a sustained departure beyond the junction mouth");
			}
			std::reverse(added.begin(),added.end());
			context.expect(UrbanDeparture::findRejoinedConflict(roads,added).has_value()==test.rejected,test.name+U": ownership order does not change the graph-based result");
			context.expectEqual(roads.nextNodeId(),nextNode,U"The rejoined query never allocates nodes");
			context.expectEqual(roads.nextEdgeId(),nextEdge,U"The rejoined query never allocates edges");
			context.expect(roads.getRoute(hostRoute)->edgeIds==routeBefore,U"The existing corridor route is unchanged by the query");
			for (size_t i=0;i<ids.size();++i)
			{
				const auto curve=*roads.getBezier(ids[i]);
				context.expect(curve.p0==before[i].p0 && curve.p1==before[i].p1 && curve.p2==before[i].p2 && curve.p3==before[i].p3,U"The query leaves all split geometry unchanged");
			}
			if (test.home)
			{
				const Vec2 center{origin.x+90,origin.z+10};
				const double half=buildingFootprintXZ(BuildingType::Detached)*.5+GenerationSettings::get().development_footprintMargin;
				const auto footprint=ParcelGeometry::footprint(center,half,0);
				context.expect(!ParcelRoadIndex{roads,true}.overlaps(footprint),test.name+U": the useful gap retains a real legal 9m home footprint");
				for (const Vec2 corner : footprint)
				{
					context.expect(world.sampleHeight(static_cast<float>(corner.x),static_cast<float>(corner.y))
						>=world.waterSurfaceHeight(corner.x,corner.y)+GenerationSettings::get().development_buildingFreeboard,U"The full positive footprint is dry");
				}
			}
		}
	});

	/// @brief The bounded visual fixture leaves generated data unchanged even on an exception.
	runner.add(U"UrbanStructure.GroundCropRestoresOnException", [](TestContext& context)
	{
		World world; installFlat(world,42); const Vec3 eye{32768,21.5,32768}; world.update(eye);
		auto* chunk=world.getChunk({32,32});
		chunk->buildingGrid[{50,50}].type=BuildingType::Detached;
		chunk->buildingGrid[{50,50}].angle=.73f;
		LandPatch patch; patch.id=91; patch.sourceParcelKey=701; patch.type=LandPatchType::GardenSoil;
		patch.polygon={{32780,32780},{32790,32780},{32790,32790},{32780,32790}};
		chunk->landPatches << patch;
		const auto original=world.getActiveChunks(); const uint64 before=UrbanGroundPreview::StreetCrop::fingerprint(original);
		bool caught=false;
		try
		{
			UrbanGroundPreview::StreetCrop crop{world,eye};
			context.expect(world.getActiveChunks().size()<original.size(),U"The fixture actually withholds distant chunk mesh preparation");
			throw 17;
		}
		catch (int value) { caught=value==17; }
		context.expect(caught,U"The controlled exception traverses the crop destructor");
		context.expectEqual(UrbanGroundPreview::StreetCrop::fingerprint(original),before,U"All terrain dimensions/values, buildings, parcels and chunk flags restore after failure");
		context.expect(world.getActiveChunks()==original,U"The public active cache is restored to its original ordered pointers");
	});

	/// @brief Entry strips preserve full width and ownership, including rotated high-coordinate lots.
	runner.add(U"UrbanStructure.ResidentialAccessOwnership", [](TestContext& context)
	{
		const Array<Vec2> parcel{{-4,.4},{4,.4},{4,12},{-4,12}};
		const Vec2 road{0,0},door{0,8};
		const auto path=ResidentialParcelAccess::create(parcel,road,door);
		context.expect(path.has_value(),U"A full-width strip joins the assigned street frontage to the real entry");
		if (path) { context.expectNear(Polygon{*path}.area(),9.6,.0001,U"The complete 1.2m by8m strip is retained, rather than disconnected clipped fragments"); }
		const auto transform=[](Vec2 point) { const double angle=.73; return Vec2{40000+point.x*Cos(angle)-point.y*Sin(angle),47000+point.x*Sin(angle)+point.y*Cos(angle)}; };
		Array<Vec2> rotated; for (const auto point : parcel) { rotated << transform(point); }
		String rotationReason;
		const auto rotatedPath=ResidentialParcelAccess::create(rotated,transform(road),transform(door),{},ResidentialParcelAccess::kWidth,&rotationReason);
		Array<Vec2> localRotated; for (const auto point : rotated) { localRotated << point-transform(road); }
		if (!Geometry2D::IsClockwise(localRotated)) { localRotated.reverse(); }
		JSON rotationReport; rotationReport[U"ownedArea"]=Polygon{localRotated}.area(); rotationReport[U"hullArea"]=Geometry2D::ConvexHull(localRotated).area();
		rotationReport[U"areaDelta"]=Abs(Polygon{localRotated}.area()-Geometry2D::ConvexHull(localRotated).area()); rotationReport[U"failureReason"]=rotationReason;
		rotationReport.save(U"TestResults/residential_access_rotation.json");
		context.expect(rotatedPath.has_value(),U"Parcel ownership survives rotated world coordinates above32768m: "+rotationReason);
		if (rotatedPath)
		{
			double twiceArea=0;
			for (size_t i=0;i<rotatedPath->size();++i)
			{
				const Vec2 a=(*rotatedPath)[i]-transform(road),b=(*rotatedPath)[(i+1)%rotatedPath->size()]-transform(road);
				twiceArea+=a.x*b.y-a.y*b.x;
			}
			rotationReport[U"returnedDoubleArea"]=Abs(twiceArea)*.5;
			rotationReport[U"worldFloatTriangulatedArea"]=Polygon{*rotatedPath}.area();
			rotationReport.save(U"TestResults/residential_access_rotation.json");
			context.expectNear(Abs(twiceArea)*.5,9.6,.000001,U"World translation preserves the returned double-coordinate entrance width and area; emitted Float3 coverage is tested separately");
		}
		const Array<Vec2> neighbor{{.4,2},{.8,2},{.8,4},{.4,4}};
		context.expect(!ResidentialParcelAccess::create(parcel,road,door,{neighbor}),U"A neighboring lot or building may not be crossed by the entry strip");
		context.expect(!ResidentialParcelAccess::create({{-4,2},{4,2},{4,12},{-4,12}},road,door),U"A larger unowned frontage gap stays unsupported rather than silently paved");
		context.expect(!ResidentialParcelAccess::create({{-.3,.4},{4,.4},{4,12},{-.3,12}},road,door),U"Side escape cannot be mistaken for an authorized frontage extension");
		context.expect(!ResidentialParcelAccess::create({{-4,.4},{4,.4},{4,4},{.2,4},{.2,6},{4,6},{4,12},{-4,12}},road,door),U"A concave or interrupted parcel is not accepted as a continuous route");
		context.expect(!ResidentialParcelAccess::create(parcel,road,{0,.2}),U"The full-width doorway end must be inside its actual parcel");
		context.expect(!ResidentialParcelAccess::create(parcel,road,{0,30}),U"Long improvised paths are outside this bounded frontage treatment");
		context.expect(!ResidentialParcelAccess::create(parcel,road,door,{},.8),U"The helper does not squeeze a sub-width path through a restricted lot");
		const Array<Vec2> transport{{-10,-4},{10,-4},{10,.2},{-10,.2}};
		context.expect(!ResidentialParcelAccess::create(parcel,road,door,{transport}),U"Road, sidewalk or rail exclusions cannot be overpainted");
	});
	/// @brief Entry endpoints use the current rendered doorway, not a guessed building-box edge.
	runner.add(U"UrbanStructure.ResidentialAccessMaskDomain", [](TestContext& context)
	{
		const RectF chunk{32768,32768,1024,1024};
		context.expect(ResidentialParcelAccess::insideMaskDomain(RectF{32770,32800,2,12},chunk),U"A fully covered resident frontage can use the complete cached transport masks");
		context.expect(!ResidentialParcelAccess::insideMaskDomain(RectF{32767.5,32800,2,12},chunk),U"A path crossing the west chunk seam is left unchanged when neighboring road masks are not available");
		context.expect(!ResidentialParcelAccess::insideMaskDomain(RectF{33790,32800,3,12},chunk),U"A path crossing the east seam cannot assume the adjacent transport corridor is clear");
		context.expect(!ResidentialParcelAccess::insideMaskDomain(RectF{32800,33790,2,3},chunk),U"The same conservative coverage rule applies to the other chunk axis");
	});
	runner.add(U"UrbanStructure.ResidentialAccessDoorTransform", [](TestContext& context)
	{
		constexpr double front=-3.1;
		const Vec2 connection=FrontageGeometry::groundEntry(front);
		bool foundStep=false;
		for (const auto& part : FrontageGeometry::build(6,front,false,0))
		{
			if (part.material!=117 || part.mesh.vertices.isEmpty()) { continue; }
			Vec3 minimum{Math::Inf,Math::Inf,Math::Inf},maximum{-Math::Inf,-Math::Inf,-Math::Inf};
			for (const auto& vertex : part.mesh.vertices)
			{
				minimum.x=Min(minimum.x,static_cast<double>(vertex.pos.x)); minimum.y=Min(minimum.y,static_cast<double>(vertex.pos.y)); minimum.z=Min(minimum.z,static_cast<double>(vertex.pos.z));
				maximum.x=Max(maximum.x,static_cast<double>(vertex.pos.x)); maximum.y=Max(maximum.y,static_cast<double>(vertex.pos.y)); maximum.z=Max(maximum.z,static_cast<double>(vertex.pos.z));
			}
			if (minimum.y>=-.00001 && maximum.y<=.15 && maximum.x-minimum.x>1.2)
			{
				foundStep=true;
				context.expectNear(connection.x,(minimum.x+maximum.x)*.5,.00001,U"The path aligns with the real entrance step's center");
				context.expectNear(connection.y,minimum.z,.00001,U"The path ends exactly at the rendered entrance step's front edge");
			}
		}
		context.expect(foundStep,U"The regression inspects actual existing FrontageGeometry step vertices");
		const auto pair=FrontageGeometry::pairedGroundEntries(9);
		context.expectEqual(pair.size(),2,U"Both paired-house doors receive separate entrance connections");
		context.expect(pair[0].x<0 && pair[1].x>0 && Abs(pair[0].x+pair[1].x)<.00001,U"The two paths follow the actual opposed doorway offsets");
		for (const auto point : pair) { context.expect(point.y>=-4.5 && point.y<=-4.5+FrontageGeometry::kPairedApronDepth,U"Paired access meets the existing concrete apron inside its footprint"); }
	});
	/// @brief Path surfaces meet real porch heights and reject cross-slope or floating/buried shortcuts.
	runner.add(U"UrbanStructure.ResidentialAccessRampHeights", [](TestContext& context)
	{
		const auto flat=[](Vec2) { return 20.0; };
		const Vec2 road{0,0},door{0,8};
		const auto mesh=ResidentialParcelAccess::ramp(road,{door,20.18},20.016,flat,.01);
		context.expect(mesh.has_value(),U"A gentle filled entrance ramp reaches the actual0.18m-high step top");
		if (mesh)
		{
			double startTop=-Math::Inf,endTop=-Math::Inf;
			for (const auto& vertex : mesh->vertices)
			{
				if (Abs(vertex.pos.z)<.00001) { startTop=Max(startTop,static_cast<double>(vertex.pos.y)); }
				if (Abs(vertex.pos.z-8)<.00001) { endTop=Max(endTop,static_cast<double>(vertex.pos.y)); }
				context.expect(Abs(vertex.pos.x)<=.60001 && vertex.pos.z>=-.00001 && vertex.pos.z<=8.00001,U"Every emitted top and side vertex stays in the legal1.2m strip");
			}
			context.expectNear(startTop,20.016,.00001,U"The road-end top uses the measured covered-gutter surface plus the explicit1cm seam allowance");
			context.expectNear(endTop,20.18,.00001,U"The emitted doorway end exactly matches the existing step top");
		}
		context.expect(ResidentialParcelAccess::ramp(road,{door,20.40},20.06,[](Vec2 p) { return 20+.03*p.y+.02*p.x; },.01).has_value(),U"A gentle sloped parcel supports a continuous filled ramp");
		context.expect(!ResidentialParcelAccess::ramp(road,{door,20.13},20.13,[](Vec2 p) { return 20+.16*p.x; },.01),U"A steep cross-width ground step is rejected even when all fill depths and the longitudinal path grade fit");
		context.expect(!ResidentialParcelAccess::ramp(road,{door,20.8},20.016,flat,.01),U"A raised porch requiring an excessive unsupported fill is left unchanged");
		context.expect(!ResidentialParcelAccess::ramp(road,{{0,1},20.18},20.016,flat,.01),U"A short approach cannot hide a too-steep entry ramp");
		context.expect(!ResidentialParcelAccess::ramp(road,{door,20.18},20.005,flat,.01),U"The path cannot be buried in or z-fight the existing parcel surface at the street end");
	});
	/// @brief Actual generated lifts and parcel-boundary transitions are checked, not assumed uniform.
	runner.add(U"UrbanStructure.ResidentialAccessProductionContacts", [](TestContext& context)
	{
		const auto flat=[](Vec2) { return 20.0; };
		const double lift=GenerationSettings::get().development_parcelSurfaceLift;
		const Vec2 road{0,.01},door{0,8};
		const double streetTop=20+kRoadSurfaceLift+.01;
		const Array<Vec2> parcel{{-4,.03},{4,.03},{4,12},{-4,12}};
		const auto actual=ResidentialParcelAccess::ramp(road,{door,20.18},streetTop,flat,lift,1.2,parcel);
		context.expect(actual.has_value(),U"The production0.01m parcel lift and actual0.006m road surface lift admit a real entry ramp");
		context.expect(!ResidentialParcelAccess::ramp(road,{door,20.18},streetTop,flat,.02,1.2,parcel),U"A raised custom yard immediately at the road seam is rejected at the exact ownership boundary");
		const Array<Vec2> laterParcel{{-4,1},{4,1},{4,12},{-4,12}};
		context.expect(ResidentialParcelAccess::ramp(road,{door,20.18},streetTop,flat,.02,1.2,laterParcel).has_value(),U"A later raised parcel does not incorrectly raise the floor of its unpaved frontage gap");
		const Vec2 offset{40000,47000}; Array<Vec2> translated; for (const auto point : parcel) { translated << point+offset; }
		const auto high=ResidentialParcelAccess::ramp(road+offset,{door+offset,20.18},streetTop,flat,lift,1.2,translated);
		context.expect(high.has_value(),U"The actual rendered ramp survives high-coordinate Float3 conversion");
		if (high)
		{
			for (const auto triangle : high->indices)
			{
				const Vec3 a{high->vertices[triangle.i0].pos},b{high->vertices[triangle.i1].pos},c{high->vertices[triangle.i2].pos};
				const Vec3 cross=(b-a).cross(c-a),normal{high->vertices[triangle.i0].normal};
				context.expect(std::isfinite(cross.lengthSq()) && cross.lengthSq()>0,U"Every emitted Float3 triangle has finite positive area");
				context.expect(cross.dot(normal)>0,U"Every top and side triangle winds toward its actual outward normal");
			}
		}
	});
	/// @brief Closely spaced ownership samples may collapse to Float3 rows, but never open a nonzero hole.
	runner.add(U"UrbanStructure.ResidentialAccessFloatCoverage", [](TestContext& context)
	{
		const Vec2 offset{40000,47000},road=offset+Vec2{0,.01},door=offset+Vec2{0,8};
		const Array<Vec2> parcel{offset+Vec2{-4,.0107},offset+Vec2{4,.0107},offset+Vec2{4,12},offset+Vec2{-4,12}};
		const auto mesh=ResidentialParcelAccess::ramp(road,{door,20.18},20.016,[](Vec2) { return 20.0; },.01,1.2,parcel);
		context.expect(mesh && !mesh->indices.isEmpty(),U"Near-coincident parcel-boundary samples still produce a finite nonempty ramp");
		if (!mesh) { return; }
		std::map<std::array<float,4>,int> edges;
		double area=0; float minX=Math::InfF,maxX=-Math::InfF,minZ=Math::InfF,maxZ=-Math::InfF;
		for (const auto triangle : mesh->indices)
		{
			if (mesh->vertices[triangle.i0].normal.y<.5f) { continue; }
			const std::array<Float3,3> points{mesh->vertices[triangle.i0].pos,mesh->vertices[triangle.i1].pos,mesh->vertices[triangle.i2].pos};
			const double projected=(Vec3{points[1]}-Vec3{points[0]}).cross(Vec3{points[2]}-Vec3{points[0]}).y*.5;
			context.expect(std::isfinite(projected) && projected>=0,U"Rendered top triangles never invert after Float3 conversion");
			if (projected<=0) { continue; } area+=projected;
			for (size_t i=0;i<3;++i)
			{
				const auto a=points[i],b=points[(i+1)%3];
				minX=Min(minX,a.x);maxX=Max(maxX,a.x);minZ=Min(minZ,a.z);maxZ=Max(maxZ,a.z);
				const bool forward=a.x<b.x || (a.x==b.x && a.z<b.z);
				++edges[forward ? std::array<float,4>{a.x,a.z,b.x,b.z} : std::array<float,4>{b.x,b.z,a.x,a.z}];
			}
		}
		context.expectNear(area,static_cast<double>(maxX-minX)*(maxZ-minZ),.000001,U"Top projected area covers the complete rounded strip, with no missing positive-area pieces");
		for (const auto& [edge,count] : edges)
		{
			const bool boundary=(edge[0]==minX && edge[2]==minX) || (edge[0]==maxX && edge[2]==maxX)
				|| (edge[1]==minZ && edge[3]==minZ) || (edge[1]==maxZ && edge[3]==maxZ);
			context.expect(count==2 || (count==1 && boundary),U"Every interior projected edge is paired; only the four intended outside boundaries remain open");
		}
	});

	runner.add(U"UrbanStructure.TransportObstacleParity", [](TestContext& context)
	{
		const Vec2 origin{32780,32780};
		const auto make=[&](RoadNetwork& roads,double width)
		{
			StreetBlocks::Block block; block.outline={origin,origin+Vec2{width,0},origin+Vec2{width,40},origin+Vec2{0,40}};
			block.bounds={origin,Vec2{width,40}}; block.center=block.bounds.center(); block.area=width*40;
			Array<int> nodes; for (const Vec2 point : block.outline) { nodes << roads.addNode({point.x,20,point.y}); }
			for (size_t i=0;i<nodes.size();++i)
			{
				const Vec3 a=roads.getNode(nodes[i])->position,b=roads.getNode(nodes[(i+1)%nodes.size()])->position;
				const int id=*roads.addEdge(nodes[i],nodes[(i+1)%nodes.size()],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
				GeneratedStreet::apply(*roads.getEdge(id),GeneratedStreet::describe(GeneratedStreet::Role::Village)); roads.getEdge(id)->edgeState=EdgeState::Existing; block.edges << id;
			}
			return block;
		};
		const auto track=[&](TrainNetwork& trains,double x)
		{
			const Vec3 a{origin.x+x,20,origin.y-20},b{origin.x+x,20,origin.y+60};
			trains.addEdge(trains.addNode(a),trains.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3));
		};
		const double radius=buildingFootprintXZ(BuildingType::Detached)*.5+GenerationSettings::get().development_footprintMargin;
		RoadNetwork roads; const auto block=make(roads,22); TrainNetwork trains; track(trains,11);
		const auto roadOnly=StreetBlocks::freeCenterArea(roads,block,radius);
		context.expect(roadOnly && *roadOnly>0,U"The boundary roads alone leave candidate house centers");
		ParcelRoadIndex combined{roads,true}; combined.addRailway(trains);
		const auto blocked=StreetBlocks::freeCenterArea(roads,block,radius,&combined);
		context.expect(blocked && *blocked==0,U"The same railway quads that reject placement also remove impossible house centers");
		const auto open=StreetBlocks::freeCenterArea(roads,block,0,&combined);
		context.expect(open && *open>0,U"Narrow railway verges remain distinct from buildable house sites");
		RoadNetwork wide; const auto wideBlock=make(wide,40); TrainNetwork sideRail; track(sideRail,35);
		ParcelRoadIndex adjacent{wide,true}; adjacent.addRailway(sideRail); const Vec2 witness=origin+Vec2{12,20};
		context.expect(!adjacent.overlaps(ParcelGeometry::footprint(witness,radius,0)),U"The rail-adjacent positive control retains a full legal house footprint");
		const auto remaining=StreetBlocks::freeCenterArea(wide,wideBlock,radius,&adjacent);
		context.expect(remaining && *remaining>0,U"A nearby railway never creates a blanket vacant-land exemption");
		RoadNetwork innerRoads=roads;
		const Vec3 a{origin.x+11,20,origin.y-20},b{origin.x+11,20,origin.y+60};
		const int inner=*innerRoads.addEdge(innerRoads.addNode(a),innerRoads.addNode(b),a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
		GeneratedStreet::apply(*innerRoads.getEdge(inner),GeneratedStreet::describe(GeneratedStreet::Role::Village)); innerRoads.getEdge(inner)->edgeState=EdgeState::Existing;
		const auto interior=StreetBlocks::freeCenterArea(innerRoads,block,radius);
		context.expect(interior && *interior==0,U"A registered nonboundary road is not omitted from the necessary-center-area check");
	});
	runner.add(U"UrbanStructure.SmallPublicPocket", [](TestContext& context)
	{
		Fixture fixture; World world; installFlat(world); TrainNetwork trains;
		const auto make=[&](RoadNetwork& roads,double width,double depth,bool allOld=false)
		{
			StreetBlocks::Block block; const Vec2 origin{32780,32780};
			block.outline={origin,origin+Vec2{width,0},origin+Vec2{width,depth},origin+Vec2{0,depth}};
			block.bounds={origin,Vec2{width,depth}}; block.center=block.bounds.center(); block.area=width*depth;
			Array<int> nodes; for (const Vec2 point : block.outline) { nodes << roads.addNode({point.x,20,point.y}); }
			for (size_t i=0;i<nodes.size();++i)
			{
				const Vec3 a=roads.getNode(nodes[i])->position,b=roads.getNode(nodes[(i+1)%nodes.size()])->position;
				const int id=*roads.addEdge(nodes[i],nodes[(i+1)%nodes.size()],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::LocalRoad,2);
				GeneratedStreet::apply(*roads.getEdge(id),GeneratedStreet::describe(GeneratedStreet::Role::Village)); roads.getEdge(id)->edgeState=EdgeState::Existing; block.edges << id;
				if (i==0 || allOld) { roads.addRoute(RoadRouteKind::CityRoute,i==0 ? U"検証旧村道1-1" : U"検証旧集落連絡道1",{id},0); }
			}
			return block;
		};
		RoadNetwork roads; const auto block=make(roads,20,20); const auto pockets=UrbanPocketGreen::create(world,roads,trains,block);
		context.expect(pockets && !pockets->isEmpty(),U"A deliberately small inherited/modern corner supports explicit public ground");
		if (pockets) for (const auto& patch : *pockets)
		{
			const Polygon shape{patch.polygon}; context.expect(patch.sourceParcelKey<0 && patch.type==LandPatchType::GardenSoil,U"Public ground is distinct from occupied housing parcels");
			for (const Vec2 point : patch.polygon) { context.expect(Polygon{block.outline}.contains(point),U"Public pocket stays inside its actual street face"); }
			for (const auto& edge : roads.edges())
			{
				ParcelRoadGeometry::forEachRibbon(edge,*roads.getBezier(edge.id),[&](const ParcelGeometry::Quad& quad)
				{
					Array<Vec2> points; for (const Vec2 point : quad) { points << point; }
					context.expect(!shape.intersects(Geometry2D::ConvexHull(points)),U"Public pocket does not cover road clearance ribbons");
				});
			}
		}
		RoadNetwork large; context.expect(!UrbanPocketGreen::create(world,large,trains,make(large,40,40)),U"Larger buildable vacant blocks are not converted to small-pocket exemptions");
		RoadNetwork sliver; context.expect(!UrbanPocketGreen::create(world,sliver,trains,make(sliver,40,8)),U"Thin residual strips do not become nominal public greens");
		RoadNetwork old; context.expect(!UrbanPocketGreen::create(world,old,trains,make(old,20,20,true)),U"Old connecting routes are inherited too, not a false modern boundary");
		RoadNetwork offsetRoads=roads;
		const int offsetA=offsetRoads.addNode({32755,20,32760}),offsetB=offsetRoads.addNode({32755,20,32820});
		const int offsetId=*offsetRoads.addEdge(offsetA,offsetB,{32755,20,32780},{32755,20,32800},RoadType::LocalRoad,2);
		auto* shifted=offsetRoads.getEdge(offsetId); GeneratedStreet::apply(*shifted,GeneratedStreet::describe(GeneratedStreet::Role::Village)); shifted->edgeState=EdgeState::Existing;
		for (auto& part : shifted->parts) { part.offsetA_L+=40;part.offsetA_R+=40;part.offsetB_L+=40;part.offsetB_R+=40; }
		const auto clipped=UrbanPocketGreen::create(world,offsetRoads,trains,block); bool crosses=false;
		ParcelRoadGeometry::forEachRibbon(*shifted,*offsetRoads.getBezier(offsetId),[&](const ParcelGeometry::Quad& quad)
		{
			Array<Vec2> points; for (const Vec2 point : quad) { points << point; } const Polygon obstacle=Geometry2D::ConvexHull(points);
			crosses |= obstacle.intersects(Polygon{block.outline});
			if (clipped) for (const auto& patch : *clipped) { context.expect(!Polygon{patch.polygon}.intersects(obstacle),U"A laterally offset road outside the centerline bounds still clips the pocket"); }
		});
		context.expect(crosses,U"The offset-road fixture actually reaches the candidate public ground");
		const int railA=trains.addNode({32790,20,32760}),railB=trains.addNode({32790,20,32820});
		trains.addEdge(railA,railB,{32790,20,32780},{32790,20,32800});
		context.expect(!UrbanPocketGreen::create(world,roads,trains,block),U"Active rail landscape clearance is never consumed by public pockets");
		const TrainNetwork noTrains; auto* chunk=world.getChunk({32,32}); LandPatch existing;
		existing.type=LandPatchType::ParcelAsphalt; existing.polygon=RectF{32780,32780,20,20}.asPolygon().outer(); chunk->landPatches << existing;
		context.expect(!UrbanPocketGreen::create(world,roads,noTrains,block),U"Existing private yards or parking surfaces are not repainted as public land");
		chunk->landPatches.clear(); chunk->heightMap[{1,1}]=-5;
		context.expect(!UrbanPocketGreen::create(world,roads,noTrains,block),U"A low terrain-cell corner cannot hide below a pocket surface");
	});
	/// @brief A fixed half-metre frontage window requires the production refined search.
	runner.add(U"UrbanStructure.RefinedFrontageKeepsLegalFootprint", [](TestContext& context)
	{
		Fixture fixture; World world; installFlat(world,42); RoadNetwork roads;
		const auto& settings=GenerationSettings::get();
		const double half=buildingFootprintXZ(BuildingType::Detached)*.5;
		const double neighborMargin=ParcelGeometry::kPlacementNeighborClearance;
		context.expectNear(half,4.5,.0001,U"The fixed regression retains the full 9m detached home");
		const Vec2 target{32776,32776};
		RoadEdge prototype; GeneratedStreet::apply(prototype,GeneratedStreet::describe(GeneratedStreet::Role::Village));
		const auto prototypeRange=RoadGeometry::structuralRangeAt(prototype,0);
		const double roadZ=target.y-(-prototypeRange.left+half+settings.development_minimumRoadSetback);
		const int first=roads.addNode({target.x-70,20,roadZ}),last=roads.addNode({target.x+50,20,roadZ});
		const Vec3 start=roads.getNode(first)->position,end=roads.getNode(last)->position;
		const int edgeId=*roads.addEdge(first,last,start.lerp(end,1.0/3),start.lerp(end,2.0/3),RoadType::LocalRoad,2);
		GeneratedStreet::apply(*roads.getEdge(edgeId),GeneratedStreet::describe(GeneratedStreet::Role::Village));
		roads.getEdge(edgeId)->edgeState=EdgeState::Existing; roads.rebuildNodeConnectivity(first,last);
		const auto curve=roads.getBezier(edgeId);
		const Vec2 span=StreetBlocks::frontageSpan(roads,edgeId);
		context.expectNear(span.x,9,.0001,U"The endpoint setback fixes the shared sample phase");
		context.expectNear(curve->positionAt(static_cast<float>(span.x+61)).x,target.x,.001,U"The intended gap is 61m after the frontage sample origin");

		// The production neighbor check expands BOTH houses by .25m. Two centers
		// at +/-9.75m therefore leave a .5m legal interval for a third 9m house.
		const double neighborOffset=2*(half+neighborMargin)+.25;
		const Array<Vec2> neighbors{target-Vec2{neighborOffset,0},target+Vec2{neighborOffset,0}};
		Point targetChunk; int targetCol=0,targetRow=0;
		ZoneGrid::worldToZoneCell(static_cast<float>(target.x),static_cast<float>(target.y),targetChunk,targetCol,targetRow);
		const auto install=[&](Vec2 position,float arc,float angle)
		{
			Point coord; int col=0,row=0;
			ZoneGrid::worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
			auto* chunk=world.getChunk(coord);
			context.expect(chunk && chunk->buildingGrid[{col,row}].type==BuildingType::None,U"Each real home has its own empty storage cell");
			if (!chunk || chunk->buildingGrid[{col,row}].type!=BuildingType::None) { return false; }
			Building building; building.type=BuildingType::Detached; building.angle=angle; building.edgeId=edgeId; building.edgeT=curve->tFromArcLength(arc);
			const Vec2 cell=ZoneGrid::cellCenterXZ(coord,col,row);
			building.offsetX=static_cast<float>(position.x-cell.x); building.offsetZ=static_cast<float>(position.y-cell.y);
			chunk->buildingGrid[{col,row}]=building; chunk->zoneMap[{col,row}]=ZoneType::LowResidential; chunk->meshDirty=true;
			return true;
		};
		for (const Vec2 position : neighbors)
		{
			Point coord; int col=0,row=0;
			ZoneGrid::worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
			context.expect(coord!=targetChunk || col!=targetCol || row!=targetRow,U"Offset neighbors do not occupy the target's storage cell");
			if (!install(position,static_cast<float>(position.x-start.x),0)) { return; }
		}
		const ParcelRoadIndex roadIndex{roads,true};
		struct Candidate { Vec2 position; float arc=0,angle=0; };
		Array<int> legalCounts,neighborRejects;
		Optional<Candidate> selected;
		for (int pass=0;pass<StreetBlocks::kFrontagePassCount;++pass)
		{
			const auto sampling=StreetBlocks::frontageSampling(roads,edgeId,pass);
			int legal=0,blockedByNeighbor=0;
			for (float arc=static_cast<float>(sampling.span.x);arc<sampling.span.y;arc+=sampling.arcStep)
			{
				const auto range=RoadGeometry::structuralRangeAt(*roads.getEdge(edgeId),arc/curve->totalLength);
				const Vec3 point=curve->positionAt(arc),right=tangentToRight(curve->tangentAt(arc));
				for (const int side : {-1,1})
				for (double setback=sampling.firstSetback;setback<=sampling.lastSetback;setback+=StreetBlocks::FrontageSampling::kSetbackStep)
				{
					const Vec2 direction{right.x*side,right.z*side}; const double outer=side<0 ? -range.left : range.right;
					const Vec2 position=Vec2{point.x,point.z}+direction*(outer+half+setback);
					Point coord; int col=0,row=0;
					ZoneGrid::worldToZoneCell(static_cast<float>(position.x),static_cast<float>(position.y),coord,col,row);
					// A bounded unresolved search window. Do not accept unrelated free
					// frontage elsewhere on the 120m road as evidence for this pocket.
					if (coord!=targetChunk || col!=targetCol || row!=targetRow) { continue; }
					const auto* chunk=world.getChunk(coord);
					if (!chunk || chunk->buildingGrid[{col,row}].type!=BuildingType::None) { continue; }
					const float angle=static_cast<float>(std::atan2(-direction.x,direction.y));
					const auto footprint=ParcelGeometry::footprint(position,half+settings.development_footprintMargin,angle);
					if (roadIndex.overlaps(footprint)) { continue; }
					double low=Math::Inf,high=-Math::Inf; bool dry=true;
					for (const Vec2 corner : footprint)
					{
						const double height=world.sampleHeight(static_cast<float>(corner.x),static_cast<float>(corner.y));
						low=Min(low,height); high=Max(high,height);
						dry &= height>=world.waterSurfaceHeight(corner.x,corner.y)+settings.development_buildingFreeboard;
					}
					if (!dry || low<settings.development_coastalBuildHeight || high-low>settings.development_maximumSuburbanRelief) { continue; }
					bool overlap=false;
					for (const Vec2 neighbor : neighbors)
					{
						overlap |= ParcelGeometry::overlaps(ParcelGeometry::footprint(position,half+neighborMargin,angle),
							ParcelGeometry::footprint(neighbor,half+neighborMargin,0));
					}
					if (overlap) { ++blockedByNeighbor; continue; }
					++legal;
					if (pass==2 && !selected) { selected=Candidate{position,arc,angle}; }
				}
			}
			legalCounts << legal; neighborRejects << blockedByNeighbor;
		}
		context.expectEqual(legalCounts[0],0,U"The unchanged 6m coarse schedule misses the legal half-metre window");
		context.expectEqual(legalCounts[1],0,U"The unchanged 1.5m fine schedule also misses the window");
		context.expect(neighborRejects[0]>0 && neighborRejects[1]>0,U"Earlier passes really attempt this window and fail the legal neighbor clearance");
		context.expect(legalCounts[2]>0 && selected.has_value(),U"The production .5m refined schedule finds an unchanged full-size home");
		if (!selected) { return; }
		context.expectNear(selected->position.x,target.x,.01,U"Refinement reaches the deliberately off-phase legal gap");
		context.expectNear(selected->position.y,target.y,.01,U"The first refined success keeps the existing minimum legal setback");
		if (!install(selected->position,selected->arc,selected->angle)) { return; }

		MapGenerator::Settlement town; town.center=target; town.kind=MapGenerator::SettlementKind::RegionalCity;
		town.plan=UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle,0,{},42,true); UrbanStructure::apply(town.plan,Type::HistoricGrid);
		const Array<MapGenerator::Settlement> towns{town}; const TrainNetwork trains;
		GenerationOptions options; options.selected[static_cast<size_t>(GenerationOptions::Element::Farms)]=false;
		SettlementDevelopment development{world,roads,trains,towns,42,options}; development.generateLandPatches();
		const auto validation=development.validateGeneratedCityConstraints();
		context.expect(validation.passed,U"All three real homes retain parcels, access, terrain and whole-city clearance constraints: "+validation.summary);
	});
	runner.add(U"UrbanStructure.AlreadyConnectedSeam", [](TestContext& context)
	{
		Fixture fixture; World world; installFlat(world);
		MapGenerator::Settlement town; town.center={32768,32768};
		for (const bool alreadyConnected : {false,true})
		{
			RoadNetwork roads;
			const int outside=roads.addNode({32668,20,32768}),back=roads.addNode({32608,20,32768});
			const int first=roads.addNode({32768,20,32688}),last=roads.addNode({32768,20,32848});
			const auto add=[&](int a,int b)
			{
				const Vec3 start=roads.getNode(a)->position,end=roads.getNode(b)->position;
				const int id=*roads.addEdge(a,b,start.lerp(end,1.0/3),start.lerp(end,2.0/3),RoadType::LocalRoad,2);
				roads.getEdge(id)->edgeState=EdgeState::Existing; return id;
			};
			const int approach=add(outside,back),oldLane=add(first,last);
			roads.addRoute(RoadRouteKind::CityRoute,U"検証旧村道1-1",{oldLane},0);
			if (alreadyConnected) { roads.addRoute(RoadRouteKind::CityRoute,U"検証旧村道1-2",{approach},0); }
			UrbanMosaic::Layout layout; layout.cores << UrbanMosaic::Core{{0,0},{1,0},60,60,{32768,20,32768}};
			const int nextNode=roads.nextNodeId(),nextEdge=roads.nextEdgeId();
			const bool connected=UrbanMosaic::connectInfill(layout,town,world,roads,outside,first,{-100,0},{0,0});
			context.expect(connected!=alreadyConnected,U"Only a modern node without existing inherited access needs a seam link");
			if (alreadyConnected)
			{
				context.expectEqual(roads.nextNodeId(),nextNode,U"Existing access does not consume node IDs"); context.expectEqual(roads.nextEdgeId(),nextEdge,U"Existing access does not consume edge IDs");
				context.expect(layout.seamConnections==0 && layout.transactionCopies==0 && layout.seamLanes.isEmpty(),U"Existing access is recognized before fitting or copying a redundant parallel approach");
			}
			else { context.expect(layout.seamConnections==1 && !layout.seamLanes.isEmpty(),U"A nearby but unjoined old street gains real modern access"); }
		}
	});
	const auto registerHamletCase=[&](int visual)
	{
		runner.add(visual==2 ? U"UrbanStructure.GroundGutterPreview" : (visual ? U"UrbanStructure.GroundPreview" : U"UrbanStructure.AbsorbedHamletTopology"), [visual](TestContext& context)
		{
			Fixture fixture;
			for (const uint64 seed : {7u,42u,130u})
			{
				if (visual && seed!=42) { continue; }
				World world; installFlat(world,seed);
				MapGenerator::Settlement town;
				town.center={32768,32768}; town.kind=MapGenerator::SettlementKind::RegionalCity; town.name=U"検証市";
				town.plan=UrbanMorphology::makePlan(UrbanMorphology::Origin::Market,0,{},seed,true);
				UrbanStructure::apply(town.plan,Type::RegionalHub);
				RoadNetwork roads;
				Array<int> trunkNodes,trunkEdges;
				for (const double offset : {-1800.0,-1300.0,0.0,1300.0,1800.0}) { trunkNodes << roads.addNode({32768+offset,20,32768}); }
				for (size_t i=1;i<trunkNodes.size();++i)
				{
					const Vec3 a=roads.getNode(trunkNodes[i-1])->position,b=roads.getNode(trunkNodes[i])->position;
					trunkEdges << *roads.addEdge(trunkNodes[i-1],trunkNodes[i],a.lerp(b,1.0/3),a.lerp(b,2.0/3),RoadType::Arterial,2);
				}
				const int trunk=roads.addRoute(RoadRouteKind::NationalRoute,U"継承する国道",trunkEdges,314);
				DistrictRoads::generateSettlement(seed,0,town,DistrictRoads::extractKaido(town,roads,2000),world,roads);
				HashSet<int> originalOldRoutes;
				for (const auto& route : roads.routes())
				{
					if (route.id>=0 && (route.name.contains(U"旧村道") || route.name.contains(U"旧集落連絡道"))) { originalOldRoutes.insert(route.id); }
				}
				HashSet<int> primaryGatewayNodes;
				const auto routeEnds=[&](const RoadRoute& route)->Optional<std::pair<int,int>>
				{
					if (route.edgeIds.isEmpty()) { return none; }
					const auto* first=roads.getEdge(route.edgeIds.front()); if (!first) { return none; }
					for (const int start : {first->nodeA,first->nodeB})
					{
						int current=start; bool valid=true;
						for (const int id : route.edgeIds)
						{
							const auto* edge=roads.getEdge(id);
							if (!edge || (edge->nodeA!=current && edge->nodeB!=current)) { valid=false; break; }
							current=edge->nodeA==current ? edge->nodeB : edge->nodeA;
						}
						if (valid) { return std::pair{start,current}; }
					}
					return none;
				};
				for (const auto& route : roads.routes())
				{
					if (route.id>=0 && route.name.contains(U"旧村道") && route.name.ends_with(U"-1"))
					{
						if (const auto ends=routeEnds(route)) { primaryGatewayNodes.insert(ends->first); primaryGatewayNodes.insert(ends->second); }
					}
				}
				int connectorGatewayJoins=0,connectorInteriorJoins=0;
				for (const auto& route : roads.routes())
				{
					if (route.id<0 || !route.name.contains(U"旧集落連絡道")) { continue; }
					const auto ends=routeEnds(route);
					const bool joins=ends && (primaryGatewayNodes.contains(ends->first) || primaryGatewayNodes.contains(ends->second));
					connectorGatewayJoins+=joins; connectorInteriorJoins+=!joins;
					context.expect(joins,U"Historical connecting lanes enter a primary-street gateway, avoiding a duplicate parallel midpoint approach");
				}
				JSON cleanupStages;
				const auto auditCleanup=[&](StringView stage)
				{
					JSON snapshot; snapshot[U"stage"]=String{stage}; int inheritedEdges=0,violations=0;
					for (const auto& edge : roads.edges())
					{
						if (edge.id<0) { continue; }
						bool oldLane=false;
						for (const int routeId : edge.routeIds)
						{
							const auto* route=roads.getRoute(routeId);
							oldLane |= route && (route->name.contains(U"旧村道") || route->name.contains(U"旧集落連絡道"));
						}
						if (!oldLane) { continue; }
						++inheritedEdges; const auto curve=roads.getBezier(edge.id);
						if (RoadAlignment::respectsLimits(*curve,edge.roadType)) { continue; }
						++violations; double grade=0;
						for (int i=0;i<=32;++i)
						{
							const Vec3 tangent=curve->tangent(i/32.0f);
							grade=Max(grade,Abs(tangent.y)/Max(1e-12,Vec2{tangent.x,tangent.z}.length()));
						}
						JSON failure; failure[U"edgeId"]=edge.id; failure[U"routeIds"]=edge.routeIds;
						failure[U"minimumRadius"]=curve->minimumHorizontalRadius(); failure[U"maximumGrade"]=grade;
						failure[U"requiredRadius"]=RoadDesignLimits::forType(edge.roadType).minimumRadius;
						failure[U"allowedGrade"]=RoadDesignLimits::forType(edge.roadType).maximumGrade;
						failure[U"designGrade"]=edge.designGrade; failure[U"length"]=curve->totalLength;
						failure[U"p0"]=Array<double>{curve->p0.x,curve->p0.y,curve->p0.z}; failure[U"p1"]=Array<double>{curve->p1.x,curve->p1.y,curve->p1.z};
						failure[U"p2"]=Array<double>{curve->p2.x,curve->p2.y,curve->p2.z}; failure[U"p3"]=Array<double>{curve->p3.x,curve->p3.y,curve->p3.z};
						snapshot[U"failures"].push_back(failure);
					}
					HashTable<int64,int> pairs;
					for (const auto& edge : roads.edges())
					{
						if (edge.id<0) { continue; }
						const int64 key=static_cast<int64>(Min(edge.nodeA,edge.nodeB))*0x100000000LL+Max(edge.nodeA,edge.nodeB);
						if (const auto previous=pairs.find(key); previous!=pairs.end())
						{
							JSON pair;
							for (const int id : {previous->second,edge.id})
							{
								const auto* member=roads.getEdge(id); const auto curve=roads.getBezier(id); JSON detail;
								detail[U"id"]=id; detail[U"designGrade"]=member->designGrade; detail[U"nodes"]=Array<int>{member->nodeA,member->nodeB};
								detail[U"length"]=curve->totalLength; detail[U"roadType"]=static_cast<int>(member->roadType);
								for (const int routeId : member->routeIds) { if (const auto* route=roads.getRoute(routeId)) { detail[U"routes"].push_back(route->name); } }
								for (const Vec3 point : {curve->p0,curve->p1,curve->p2,curve->p3}) { detail[U"controls"].push_back(Array<double>{point.x,point.y,point.z}); }
								pair.push_back(detail);
							}
							snapshot[U"duplicatePairs"].push_back(pair);
						}
						else { pairs.emplace(key,edge.id); }
					}
					snapshot[U"inheritedEdges"]=inheritedEdges; snapshot[U"violations"]=violations;
					cleanupStages.push_back(snapshot);
					cleanupStages.save(fixture.directory+U"TestResults/absorbed_hamlet_cleanup_seed{}.json"_fmt(seed));
				};
				auditCleanup(U"generated");
				int acutePasses=0; while (acutePasses<1000 && roads.fixSharpAngles(45.0f)) { ++acutePasses; }
				auditCleanup(U"fixSharpAngles");
				roads.smoothAllCurves(); auditCleanup(U"smoothAllCurves");
				DistrictRoads::straightenCastleTownRoads({town},world,roads); auditCleanup(U"straighten");
				RoadDesignLimits::smoothThroughChains(roads); auditCleanup(U"smoothThroughChains");
				for (const auto& edge : roads.edges()) { if (edge.id>=0) { RoadDesignLimits::constrainCurve(roads,edge.id); roads.getEdge(edge.id)->edgeState=EdgeState::Open; } }
				auditCleanup(U"constrainCurve");
				bool crossing=false;
				for (int pass=0;pass<4;++pass) { crossing=roads.resolveIntersections(); if (!crossing) { break; } }
				auditCleanup(U"resolveIntersections");
				context.expect(!crossing,U"Inherited and modern streets have explicit intersection nodes, not remaining same-level crossings");
				for (int pass=0;pass<2;++pass) { roads.consolidateOverlappingRoads(); auditCleanup(U"consolidate{}"_fmt(pass)); roads.resolveIntersections(); auditCleanup(U"resolveAfterConsolidate{}"_fmt(pass)); }
				roads.removeDuplicateEdges(seed); auditCleanup(U"removeDuplicateEdges");
				StreetBlocks::mergeNarrowFaces(roads); auditCleanup(U"mergeNarrowFaces");
				HashSet<int64> endpointPairs; int duplicatePairs=0;
				for (const auto& edge : roads.edges())
				{
					if (edge.id<0) { continue; }
					const int64 key=static_cast<int64>(Min(edge.nodeA,edge.nodeB))*0x100000000LL+Max(edge.nodeA,edge.nodeB);
					duplicatePairs+=!endpointPairs.insert(key).second;
				}
				context.expectEqual(duplicatePairs,0,U"Fresh city generation does not leave duplicate endpoint-pair corridors behind protected cleanup guards");
				HashSet<int> inherited;
				int inheritedRoutes=0,gatewayFailures=0;
				HashSet<String> oldCoreGroups;
				for (const auto& route : roads.routes())
				{
					if (route.id<0 || (!route.name.contains(U"旧村道") && !route.name.contains(U"旧集落連絡道"))) { continue; }
					++inheritedRoutes;
					context.expect(originalOldRoutes.contains(route.id),U"Cleanup must not fragment an inherited route into replacement identities");
					if (route.name.contains(U"旧村道")) { oldCoreGroups.insert(route.name.substr(0,route.name.lastIndexOf(U'-'))); }
					const auto walk = [&](int start)->Optional<int>
					{
						int current=start;
						for (const int id : route.edgeIds)
						{
							const auto* edge=roads.getEdge(id);
							if (!edge || (edge->nodeA!=current && edge->nodeB!=current)) { return none; }
							current=edge->nodeA==current ? edge->nodeB : edge->nodeA;
						}
						return current;
					};
					const auto* first=route.edgeIds.isEmpty() ? nullptr : roads.getEdge(route.edgeIds.front());
					context.expect(first!=nullptr,U"Inherited lane routes remain populated after intersection/block cleanup");
					if (!first) { continue; }
					int start=first->nodeA; auto end=walk(start);
					if (!end) { start=first->nodeB; end=walk(start); }
					context.expect(end.has_value(),U"Each inherited route keeps ordered, continuous live edges");
					if (end)
					{
						for (const int gate : {start,*end})
						{
							const bool joined=roads.getNode(gate)->attachments.any([&](const auto& attachment) { return !route.edgeIds.contains(attachment.edgeId); });
							gatewayFailures+=!joined;
							context.expect(joined,U"Both ends of each inherited lane join the surrounding street network");
						}
					}
					for (const int id : route.edgeIds)
					{
						const auto* edge=roads.getEdge(id); if (!edge) { continue; }
						inherited.insert(id);
						context.expect(edge->routeIds.contains(route.id),U"Inherited route membership is reciprocal after splitting");
						context.expect(RoadAlignment::respectsLimits(*roads.getBezier(id),edge->roadType),U"Inherited lanes retain real curvature/grade constraints");
					}
				}
				context.expectEqual(inheritedRoutes,originalOldRoutes.size(),U"Every original inherited route identity survives cleanup");
				const auto* national=roads.getRoute(trunk);
				bool ordered=national && national->number==314; int current=trunkNodes.front();
				if (national) for (const int id : national->edgeIds)
				{
					const auto* edge=roads.getEdge(id);
					if (!edge || (edge->nodeA!=current && edge->nodeB!=current)) { ordered=false; break; }
					current=edge->nodeA==current ? edge->nodeB : edge->nodeA;
				}
				ordered &= current==trunkNodes.back();
				context.expect(ordered,U"A national route keeps its identity and ordered connection across the mosaic");
				double allLength=0,nonGridLength=0,oldLength=0; int tees=0,seamJunctions=0;
				HashSet<int> reached{trunkNodes.front()}; Array<int> pending{trunkNodes.front()};
				for (size_t cursor=0;cursor<pending.size();++cursor)
				{
					for (const auto& attachment : roads.getNode(pending[cursor])->attachments)
					{
						const auto* edge=roads.getEdge(attachment.edgeId); if (!edge || !edge->hasRoadLanes()) { continue; }
						const int next=edge->nodeA==pending[cursor] ? edge->nodeB : edge->nodeA;
						if (reached.insert(next).second) { pending << next; }
					}
				}
				const Size size{720,720}; const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm};
				const auto screen=[&](Vec3 p)
				{
					const Vec2 delta{p.x-town.center.x,p.z-town.center.y};
					return Vec2{360+delta.dot(town.gridAxisX)*320/town.plan.halfExtent.x,360+delta.dot(town.gridAxisZ)*320/town.plan.halfExtent.y};
				};
				{
					const ScopedRenderTarget2D output{target.clear(ColorF{.95,.95,.92})};
					for (const auto& edge : roads.edges())
					{
						if (edge.id<0 || !edge.hasRoadLanes()) { continue; }
						context.expect(reached.contains(edge.nodeA) && reached.contains(edge.nodeB),U"No old core or seam road is an isolated component");
						const auto curve=roads.getBezier(edge.id); const Vec3 mid=curve->evaluate(.5f);
						const Vec2 delta{mid.x-town.center.x,mid.z-town.center.y};
						const Vec2 local{delta.dot(town.gridAxisX),delta.dot(town.gridAxisZ)};
						if (!UrbanMorphology::inCore(town.plan,local,-30)) { continue; }
						const Vec3 tangent=curve->p3-curve->p0;
						const Vec2 direction=Vec2{tangent.x,tangent.z}.normalized();
						allLength+=curve->totalLength; oldLength+=inherited.contains(edge.id) ? curve->totalLength : 0;
						if (Min(Abs(direction.dot(town.gridAxisX)),Abs(direction.dot(town.gridAxisZ)))>Sin(8_deg)) { nonGridLength+=curve->totalLength; }
						const int samples=Max(2,static_cast<int>(Ceil(curve->totalLength/12)));
						for (int i=0;i<samples;++i)
						{
							Line{screen(curve->positionAt(curve->totalLength*i/samples)),screen(curve->positionAt(curve->totalLength*(i+1)/samples))}.draw(inherited.contains(edge.id) ? 2.0 : 1.0,inherited.contains(edge.id) ? ColorF{.80,.31,.13} : ColorF{.22,.34,.46});
						}
					}
				}
				Graphics2D::Flush(); Image image; target.readAsImage(image);
				image.save(fixture.directory+U"Screenshot/absorbed_hamlets_seed{}.png"_fmt(seed));
				for (const auto& node : roads.nodes()) { tees+=node.id>=0 && node.attachments.size()==3; }
				for (const auto& node : roads.nodes())
				{
					if (node.id<0) { continue; }
					bool oldLane=false,modernLocal=false;
					for (const auto& attachment : node.attachments)
					{
						const auto* edge=roads.getEdge(attachment.edgeId); if (!edge) { continue; }
						oldLane |= inherited.contains(edge->id);
						modernLocal |= !inherited.contains(edge->id) && edge->roadType==RoadType::LocalRoad;
					}
					seamJunctions+=oldLane && modernLocal;
				}
				context.expect(oldCoreGroups.size()>=2,U"At least two distinct older hamlet cores survive inside the expanded city");
				context.expect(seamJunctions>=2,U"Modern local infill has explicit junctions onto inherited lanes at district seams");
				context.expect(tees>=4,U"Inherited and infill streets retain meaningful T junctions");
				context.expect(nonGridLength/Max(1.0,allLength)>.05,U"Inherited access-directed lanes create meaningful non-grid street length");
				context.expect(nonGridLength/Max(1.0,allLength)<.60,U"Regular modern districts remain part of the city mosaic");
				JSON blockAudit;
				int protectedFaces=0,smallGreenFaces=0,unresolvedPavementFaces=0;
				for (const auto& block : StreetBlocks::collect(roads,.1))
				{
					int protectedEdges=0;
					for (const int id : block.edges) { protectedEdges+=inherited.contains(id); }
					if (protectedEdges==0) { continue; }
					++protectedFaces;
					struct Boundary { Vec2 a,b; double halfWidth; };
					Array<Boundary> boundaries;
					for (const int id : block.edges)
					{
						const auto* edge=roads.getEdge(id); const auto curve=roads.getBezier(id);
						context.expect(edge && curve,U"Every inherited-adjacent face has live boundary references");
						if (!edge || !curve) { continue; }
						const int count=Max(1,static_cast<int>(Ceil(curve->totalLength/4)));
						for (int sample=0;sample<count;++sample)
						{
							const Vec3 a=curve->positionAt(curve->totalLength*sample/count),b=curve->positionAt(curve->totalLength*(sample+1)/count);
							boundaries << Boundary{{a.x,a.z},{b.x,b.z},edge->totalWidth()*.5};
						}
					}
					const auto scan=[&](double step)
					{
						double best=-Math::Inf;
						for (double z=block.bounds.y+step*.5;z<block.bounds.br().y;z+=step)
						for (double x=block.bounds.x+step*.5;x<block.bounds.br().x;x+=step)
						{
							const Vec2 point{x,z}; if (!block.contains(point)) { continue; }
							double clearance=Math::Inf;
							for (const auto& boundary : boundaries)
							{
								const Vec2 delta=boundary.b-boundary.a;
								const double along=Clamp((point-boundary.a).dot(delta)/Max(.001,delta.lengthSq()),0.0,1.0);
								clearance=Min(clearance,point.distanceFrom(boundary.a+delta*along)-boundary.halfWidth);
							}
							best=Max(best,clearance);
						}
						return best;
					};
					double clearance=scan(2.0);
					if (clearance<=0) { clearance=Max(clearance,scan(.5)); }
					// Sampled lower bounds flag a review, never prove absence of a tiny free island.
					unresolvedPavementFaces+=clearance<=0;
					smallGreenFaces+=clearance>0 && clearance<GenerationSettings::get().parcels_minimumBuildingHalfWidth;
					JSON face; face[U"areaM2"]=block.area; face[U"protectedEdges"]=protectedEdges;
					face[U"maximumSampledClearanceM"]=std::isfinite(clearance) ? clearance : -1e6;
					face[U"edgeIds"]=block.edges;
					blockAudit.push_back(face);
				}
				blockAudit.save(fixture.directory+U"TestResults/absorbed_hamlet_faces_seed{}.json"_fmt(seed));
				const TrainNetwork trains; const Array<MapGenerator::Settlement> towns{town};
				SettlementDevelopment development{world,roads,trains,towns,seed}; development.applyZonesGlobal();
				const auto validation=development.placeInitialBuildings(true);
				UrbanEmptyFaceDiagnostics::capture(fixture.directory,seed,town,world,roads,validation);
				if (visual && validation.passed) { UrbanGroundPreview::capture(context,fixture.directory,world,roads,validation,visual==2); }
				context.expect(validation.passed,U"Seam parcels retain frontage and avoid roads/other occupied sites: "+validation.summary);
				JSON report; report[U"seed"]=seed; report[U"duplicateEndpointPairs"]=duplicatePairs; report[U"connectorGatewayJoins"]=connectorGatewayJoins; report[U"connectorInteriorJoins"]=connectorInteriorJoins; report[U"oldCoreGroups"]=oldCoreGroups.size(); report[U"seamJunctions"]=seamJunctions; report[U"inheritedRoutes"]=inheritedRoutes; report[U"inheritedMeters"]=oldLength;
				report[U"nonGridFraction"]=nonGridLength/Max(1.0,allLength); report[U"roadMeters"]=allLength;
				report[U"teeJunctions"]=tees; report[U"gatewayFailures"]=gatewayFailures; report[U"nationalRouteContinuous"]=ordered;
				report[U"protectedFaces"]=protectedFaces; report[U"smallGreenFaces"]=smallGreenFaces; report[U"unresolvedPavementFaces"]=unresolvedPavementFaces;
				report[U"validation"]=validation.summary; report[U"unfilledBlocks"]=validation.unfilledBlocks; report[U"landscapedPockets"]=validation.landscapedPockets;
				report.save(fixture.directory+U"TestResults/absorbed_hamlets_seed{}.json"_fmt(seed));
			}
		});
	};
	registerHamletCase(false); registerHamletCase(true);
	registerHamletCase(2);
	runner.add(U"UrbanStructure.CentersAndPersistence", [](TestContext& context)
	{
		for (int index = 1; index <= 7; ++index)
		{
			const auto type = static_cast<Type>(index);
			auto plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle, 0, {}, 42, true);
			apply(plan, type);
			const double oldRadius = plan.centers.front().radius;
			UrbanMorphology::rescale(plan, .72);
			auto rebuilt = plan;
			rebuildCommercialStreets(rebuilt);
			context.expectEqual(plan.commercialStreets.size(), rebuilt.commercialStreets.size(), U"Rescaling refreshes derived commercial streets");
			for (size_t i = 0; i < plan.commercialStreets.size(); ++i)
			{
				context.expect(plan.commercialStreets[i] == rebuilt.commercialStreets[i], U"Rescaled collector axes are not stale scaled copies");
			}
			alignCenters(plan);
			context.expectNear(plan.centers.front().radius, oldRadius * .72, .001,
				U"All center influence areas shrink with the terrain fit");
			const JSON saved = saveLayout(plan);
			auto restored = plan;
			restored.centers.clear();
			restored.commercialStreets.clear();
			restored.structure = Type::None;
			restored.neighborhoodParks.clear();
			restoreLayout(restored, saved);
			context.expectEqual(restored.commercialStreets.size(), plan.commercialStreets.size(), U"Restoring derives collector axes without saved-format fields");
			for (size_t i = 0; i < plan.commercialStreets.size(); ++i)
			{
				context.expect(restored.commercialStreets[i] == plan.commercialStreets[i], U"Restored commercial streets retain their exact axes");
			}
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
	/// Commercial ribbons must use the same collector axes as the road planner.
	runner.add(U"UrbanStructure.BuildingOverlapAudit", [](TestContext& context)
	{
		Fixture fixture;
		World world;
		installFlat(world);
		RoadNetwork roads;
		const TrainNetwork trains;
		const Array<MapGenerator::Settlement> towns;
		SettlementDevelopment development{world, roads, trains, towns, 42};
		auto* chunk = world.getChunk({32, 32});
		Building first;
		first.type = BuildingType::Detached;
		chunk->buildingGrid[{2, 2}] = first;
		Building second = first;
		constexpr float kCellSize = static_cast<float>(CHUNK_SIZE) / ZONE_CELLS;
		second.offsetX = buildingFootprintXZ(BuildingType::Detached) - kCellSize;
		chunk->buildingGrid[{3, 2}] = second;
		context.expect(development.validateGeneratedCityConstraints().summary.contains(U"buildingOverlap=0"), U"Touching adjacent building footprints are allowed");
		chunk->buildingGrid[{3, 2}].offsetX -= 1;
		context.expect(development.validateGeneratedCityConstraints().summary.contains(U"buildingOverlap=2"), U"Actual intersection reports both affected buildings");
		// Stored offsets are float, but world centers retain their sub-millimetre offset.
		// Rounding the whole 32 km position to float before SAT creates false contacts.
		for (const float angle : {.07f, .38f, .64f, .79f, 1.04f, 1.4f})
		{
			first.angle = angle; first.offsetX = .00191f; first.offsetZ = .00191f;
			second = first;
			second.offsetX += static_cast<float>(buildingFootprintXZ(first.type) * Cos(angle) - kCellSize);
			second.offsetZ += static_cast<float>(buildingFootprintXZ(first.type) * Sin(angle));
			chunk->buildingGrid[{2, 2}] = first; chunk->buildingGrid[{3, 2}] = second;
			context.expect(development.validateGeneratedCityConstraints().summary.contains(U"buildingOverlap=0"), U"Rotated touching sites at high world coordinates do not lose center precision");
			chunk->buildingGrid[{3, 2}].offsetX -= .02f * Cos(angle);
			chunk->buildingGrid[{3, 2}].offsetZ -= .02f * Sin(angle);
			context.expect(development.validateGeneratedCityConstraints().summary.contains(U"buildingOverlap=2"), U"A rotated 2 cm penetration still reports both sites");
		}
		chunk->buildingGrid[{3, 2}].type = BuildingType::None;
		chunk->buildingGrid[{2, 2}].type = BuildingType::UrbanHousePair;
		context.expect(development.validateGeneratedCityConstraints().summary.contains(U"buildingOverlap=0"), U"The two dwellings within one authored rowhouse site are not a collision");
	});
	runner.add(U"UrbanStructure.CollectorFrontage", [](TestContext& context)
	{
		int checked = 0, mismatches = 0;
		for (const uint64 seed : {7u, 42u, 130u})
		{
			for (int index = 1; index <= 7; ++index)
			{
				auto plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Market, 0, {}, seed, true);
				apply(plan, static_cast<Type>(index));
				if (!plan.station) { continue; }
				const auto x = UrbanStructure::streetCoordinates(plan, false), z = UrbanStructure::streetCoordinates(plan, true);
				const auto nearest = [](const Array<float>& coordinates, double desired)
				{
					double value = coordinates.front();
					for (const double candidate : coordinates)
					{
						if (Abs(candidate - desired) < Abs(value - desired)) { value = candidate; }
					}
					return value;
				};
				const Vec2 start{nearest(x, plan.station->x), nearest(z, plan.station->y)};
				const double ribbon = GenerationSettings::get().settlements_stationStreetRadius;
				for (const auto& center : plan.centers)
				{
					const Vec2 end{nearest(x, center.position.x), nearest(z, center.position.y)};
					const Vec2 corner{end.x, start.y};
					for (const auto segment : {Line{start, corner}, Line{corner, end}})
					{
						const Vec2 delta = segment.end - segment.begin;
						if (delta.length() < 80) { continue; }
						const Vec2 side = Vec2{-delta.y, delta.x}.normalized();
						for (const double t : {.25, .5, .75})
						{
							for (const int sign : {-1, 1})
							{
								const Vec2 point = segment.begin + delta * t + side * (sign * ribbon * .72);
								const auto district = UrbanStructure::sample(plan, point).district;
								const bool commercial = district == UrbanMorphology::District::OldTown || district == UrbanMorphology::District::Station;
								++checked; mismatches += !commercial;
								context.expect(commercial, U"Collector frontage remains commercial on both sides: type={} seed={} point={}"_fmt(index, seed, point));
							}
						}
					}
				}
				if (seed == 42 && (index == static_cast<int>(Type::TransitCorridor) || index == static_cast<int>(Type::PlannedGrid)))
				{
					constexpr int kMapSize = 256;
					Image map{Size{kMapSize, kMapSize}, Palette::White};
					for (int row = 0; row < kMapSize; ++row)
					{
						for (int col = 0; col < kMapSize; ++col)
						{
							const Vec2 point{(2.0 * col / (kMapSize - 1) - 1) * plan.halfExtent.x, (2.0 * row / (kMapSize - 1) - 1) * plan.halfExtent.y};
							const auto district = UrbanStructure::sample(plan, point).district;
							Color color = district == UrbanMorphology::District::OldTown ? Color{221, 155, 113} : district == UrbanMorphology::District::Station ? Color{199, 109, 112} : Color{217, 228, 194};
							if (Abs(nearest(x, point.x) - point.x) < 3 || Abs(nearest(z, point.y) - point.y) < 3) { color = Color{72, 76, 78}; }
							if (plan.centers.any([&](const Center& center) { return point.distanceFrom(center.position) < 15; })) { color = Color{35, 72, 160}; }
							map[Point{col, row}] = color;
						}
					}
					map.save(U"Screenshot/collector_frontage_{}.png"_fmt(id(static_cast<Type>(index))));
				}
			}
		}
		JSON report;
		report[U"samples"] = checked; report[U"mismatches"] = mismatches;
		report.save(U"TestResults/collector_frontage.json");
		context.expect(checked > 100, U"Collector frontage is checked across seven structures and three seeds");
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
	for (const uint64 seed : {42u,7u,130u})
	{
		for (int index = 1; index <= 7; ++index)
		{
			const auto type = static_cast<Type>(index);
			const auto registerCase=[&](bool visual)
			{
				runner.add((visual ? U"UrbanStructure.Visual." : U"UrbanStructure.Generated.") + String{id(type)} + (seed == 42 ? U"" : U".seed{}"_fmt(seed)), [type,seed,visual](TestContext& context)
				{
					Fixture fixture;
					const String suffix = (seed == 42 ? String{} : U"_seed{}"_fmt(seed)) + (visual ? U"_visual" : U"");
					if (visual) { RegisterAssets(); }
					DebugLog::initialize(fixture.directory + U"TestResults/urban_structure_" + id(type) + suffix + U".log");
					World world;
					installFlat(world,seed);
					MapGenerator::Settlement town;
					town.center = {32768, 32768};
					town.kind = MapGenerator::SettlementKind::RegionalCity;
					town.name = U"検証市";
					town.plan = UrbanMorphology::makePlan(UrbanMorphology::Origin::Castle, 0, {}, seed, true);
					apply(town.plan, type);
					RoadNetwork roads;
					const Vec3 from{30968, 20, 32768}, to{34568, 20, 32768};
					roads.addEdge(roads.addNode(from), roads.addNode(to), from.lerp(to, 1.0 / 3), from.lerp(to, 2.0 / 3),
						RoadType::Arterial, 2);
					const auto kaido = DistrictRoads::extractKaido(town, roads, 2000);
					DistrictRoads::generateSettlement(seed, 0, town, kaido, world, roads);
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
					SettlementDevelopment actualDevelopment{world, roads, trains, towns, seed};
					actualDevelopment.applyZonesGlobal();
					const auto validation = actualDevelopment.placeInitialBuildings(true);
					int buildings = 0, high = 0, mid = 0, detached = 0, pairedHomes = 0, offices = 0, railEdges = 0;
					HashSet<int64> occupiedParcelKeys;
					int centralBuildings = 0, centralDevelopableCells = 0, centralOccupiedCells = 0;
					double area = 0, centralFootprintArea = 0;
					const Vec2 centralLandmark = town.plan.centers.front().position;
					for (int z = 30; z <= 33; ++z)
						for (int x = 30; x <= 33; ++x)
						{
							const auto& chunk = *world.getChunk({x, z});
							for (int row = 0; row < ZONE_CELLS; ++row)
								for (int col = 0; col < ZONE_CELLS; ++col)
								{
									const auto& building = chunk.buildingGrid[{col, row}];
									const Vec2 cellPoint = ZoneGrid::cellCenterXZ({x, z}, col, row);
									const Vec2 cellDelta = cellPoint - town.center;
									const Vec2 cellLocal{cellDelta.dot(town.gridAxisX), cellDelta.dot(town.gridAxisZ)};
									if (Abs(cellLocal.x - centralLandmark.x) < 300 && Abs(cellLocal.y - centralLandmark.y) < 300)
									{
										const ZoneType zone = chunk.zoneMap[{col, row}];
										if (zone == ZoneType::Residential || zone == ZoneType::LowResidential || zone == ZoneType::Commercial)
										{
											++centralDevelopableCells;
											centralOccupiedCells += building.type != BuildingType::None && building.type != BuildingType::Farmland;
										}
									}
									if (building.type == BuildingType::None || building.type == BuildingType::Farmland)
										continue;
									const Vec2 point =
										ZoneGrid::cellCenterXZ({x, z}, col, row) + Vec2{building.offsetX, building.offsetZ};
									const Vec2 delta = point - town.center,
											   local{delta.dot(town.gridAxisX), delta.dot(town.gridAxisZ)};
									if (!UrbanMorphology::inCore(town.plan, local))
										continue;
									++buildings; occupiedParcelKeys.insert(ZoneGrid::zoneCellKey({x,z},col,row));
									const double footprintArea = Square(buildingFootprintXZ(building.type));
									area += footprintArea;
									if (Abs(local.x - centralLandmark.x) < 300 && Abs(local.y - centralLandmark.y) < 300)
									{
										++centralBuildings;
										centralFootprintArea += footprintArea;
									}
									high += building.type == BuildingType::HighApartment;
									mid += building.type == BuildingType::MidApartment;
									detached += building.type == BuildingType::Detached;
									pairedHomes += building.type == BuildingType::UrbanHousePair;
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
					double occupiedParcelArea=0,totalRoadMeters=0,inheritedRoadMeters=0; int parcelPieces=0,inheritedRoutes=0;
					HashSet<String> oldCoreGroups;
					for (int z=30;z<=33;++z)
					{
						for (int x=30;x<=33;++x)
						{
							for (const auto& patch : world.getChunk({x,z})->landPatches)
							{
								if (!occupiedParcelKeys.contains(patch.sourceParcelKey) || patch.polygon.size()<3) { continue; }
								double twiceArea=0; const Vec2 origin=patch.polygon.front();
								for (size_t i=0;i<patch.polygon.size();++i) { twiceArea+=(patch.polygon[i]-origin).cross(patch.polygon[(i+1)%patch.polygon.size()]-origin); }
								occupiedParcelArea+=Abs(twiceArea)*.5; ++parcelPieces;
							}
						}
					}
					for (const auto& route : roads.routes())
					{
						if (route.id<0 || (!route.name.contains(U"旧村道") && !route.name.contains(U"旧集落連絡道"))) { continue; }
						++inheritedRoutes;
						if (route.name.contains(U"旧村道")) { oldCoreGroups.insert(route.name.substr(0,route.name.lastIndexOf(U'-'))); }
					}
					for (const auto& edge : roads.edges())
					{
						if (edge.id<0 || !edge.hasRoadLanes()) { continue; }
						const auto curve=roads.getBezier(edge.id); if (!curve) { continue; }
						totalRoadMeters+=curve->totalLength; if (UrbanMosaic::inherited(roads,edge)) { inheritedRoadMeters+=curve->totalLength; }
					}
					JSON report;
					report[U"occupiedSiteParcelAreaSumM2"]=occupiedParcelArea;
					report[U"occupiedSitesPerAssignedParcelHectare"]=buildings/Max(1.0,occupiedParcelArea/10000);
					report[U"parcelAreaIsSumNotUnion"]=true; report[U"occupiedParcelPieces"]=parcelPieces;
					report[U"oldCoreGroups"]=oldCoreGroups.size(); report[U"inheritedRoutes"]=inheritedRoutes; report[U"requestedOldCores"]=UrbanMosaic::enabled(town.plan) ? UrbanMosaic::kRequestedCores : 0;
					report[U"totalRoadMeters"]=totalRoadMeters; report[U"inheritedRoadMeters"]=inheritedRoadMeters;
					report[U"type"] = String{id(type)};
					report[U"seed"] = seed;
					report[U"buildings"] = buildings;
					report[U"high"] = high;
					report[U"mid"] = mid;
					report[U"detached"] = detached;
					report[U"pairedHomes"] = pairedHomes;
					report[U"offices"] = offices;
					report[U"footprintArea"] = area;
					report[U"centralBuildings"] = centralBuildings;
					report[U"centralDevelopableCells"] = centralDevelopableCells;
					report[U"centralOccupiedCells"] = centralOccupiedCells;
					report[U"central600mRoofFraction"] = centralFootprintArea / 360000.0;
					report[U"roads"] = streetCount;
					report[U"arterials"] = arterials;
					report[U"threeWay"] = threeWay;
					report[U"junctions"] = junctions;
					int actualStations = 0;
					for (const auto& node : trains.nodes())
					{
						actualStations += node.id >= 0 && node.type == TrackNodeType::Station && node.stationKind != StationKind::Underground;
					}
					report[U"stations"] = actualStations;
					report[U"railEdges"] = railEdges;
					report[U"validation"] = validation.summary; report[U"unfilledBlocks"]=validation.unfilledBlocks; report[U"landscapedPockets"]=validation.landscapedPockets;
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
						context.expect(pairedHomes > 100 && (seed != 42 || centralFootprintArea / 360000.0 > 0.25),
							U"Historic cities retain compact narrow homes; the original seed42 keeps its >25% central roof regression");
					if (type == Type::HistoricGrid)
						context.expectEqual(
							high + offices, 0, U"Historic fine-grid districts preserve their low and medium skyline");
					if (type == Type::Metropolitan)
						context.expect(high > 40, U"Metropolitan subcenters produce an identifiable high-rise skyline");
					if (visual)
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
						pixels.save(fixture.directory + U"Screenshot/urban_structure_" + id(type) + suffix + U".png");
					}
					report.save(fixture.directory + U"TestResults/urban_structure_" + id(type) + suffix + U".json");
					if (!visual && !validation.passed) { UrbanEmptyFaceDiagnostics::capture(fixture.directory,seed,town,world,roads,validation,&trains,String{id(type)}); }
				});
			};
			registerCase(false);
			if (seed==42 && (type==Type::PlannedGrid || type==Type::Metropolitan || type==Type::HistoricGrid)) { registerCase(true); }
		}
	}
}
