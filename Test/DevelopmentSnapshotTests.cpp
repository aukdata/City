#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/save/DevelopmentSnapshot.hpp"
#include "src/save/WorldSnapshotValidation.hpp"
#include "src/save/SaveTransaction.hpp"
#include "src/save/RoadBinary.hpp"
#include <limits>
#include "src/zone/ZoneManager.hpp"
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
	constexpr auto kSnapshotPath = U"TestResults/development_snapshot.bin";
	void prepareWorld(World& world)
	{
		world.reserveChunks();
		for (int y=0; y<WORLD_CHUNKS; ++y)
		{
			for (int x=0; x<WORLD_CHUNKS; ++x)
			{
				world.getChunk({x,y})->heightMap.clear();
			}
		}
		for (Point coord : {Point{0,0}, Point{63,63}})
		{
			world.installChunkDirect(coord, HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,10),10,10});
		}
	}
	void populate(World& world)
	{
		auto& chunk = *world.getChunk({0,0});
		chunk.isUrbanizationArea = true;
		for (int i=0; i<static_cast<int>(BuildingType::Count); ++i)
		{
			const Point cell{i,0};
			chunk.zoneMap[cell] = static_cast<ZoneType>(i%7);
			chunk.buildingGrid[cell] = Building{static_cast<BuildingType>(i),123.4567890123+i,.123f,17,.35f,-3.25f,2.125f};
		}
		chunk.zoneMap[{63,63}] = ZoneType::Agriculture;
		chunk.buildingGrid[{62,63}].angle = -0.0f;
		world.getChunk({63,63})->buildingGrid[{63,63}] = Building{BuildingType::RuralHouse,4,1,18,.6f,2,3};
		LandPatch patch;
		patch.id=12; patch.type=LandPatchType::GardenSoil; patch.materialVariant=123;
		patch.elevationOffset=.034f; patch.sourceParcelKey=1;
		patch.polygon={{1.1234567890123,2.987654321098},{30,2},{30,30},{1,30}};
		chunk.landPatches << patch;
	}
	std::vector<char> bytes()
	{
		std::ifstream input{"TestResults/development_snapshot.bin",std::ios::binary};
		return {std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
	}
	void replaceBytes(const std::vector<char>& data)
	{
		std::ofstream output{"TestResults/development_snapshot.bin",std::ios::binary|std::ios::trunc};
		output.write(data.data(),static_cast<std::streamsize>(data.size()));
	}
}

void registerDevelopmentSnapshotTests(TestRunner& runner)
{
	runner.add(U"DevelopmentSnapshot.DoublePrecisionRoadCoordinates",[](TestContext& context)
	{
		RoadNetwork source;
		const Vec3 first{58653.90886878967,12.123456789,52836.327600717545};
		const Vec3 last = first + Vec3{100.000000123,1.0000000123,40.0000000123};
		const int a = source.addNode(first), b = source.addNode(last);
		const Vec3 controlA = first + Vec3{30.000000123,2.123456789,0.000000123};
		const Vec3 controlB = last - Vec3{30.000000123,2.123456789,0.000000123};
		const auto edge = source.addEdge(a,b,controlA,controlB);
		context.expect(edge.has_value(),U"High-coordinate precision fixture created");
		if (!edge) { return; }
		RoadObject object;
		object.type = RoadObjectType::Pier; object.parentEdgeId = *edge; object.arcPos = 17.25f;
		const int objectId = source.addObject(object);
		const FilePath path = U"TestResults/double_precision_roads.bin";
		context.expect(RoadBinary::writeGlobal(path,source),U"Write current road format");
		RoadNetwork restored;
		const bool loaded = RoadBinary::readGlobal(path,restored,true);
		context.expect(loaded,U"Read current coordinate width including extended road data");
		if (!loaded) { return; }
		context.expect(restored.getNode(a)->position == first && restored.getNode(b)->position == last,U"Node doubles survive exactly at large map coordinates");
		context.expect(restored.getEdge(*edge)->ctrlA == controlA && restored.getEdge(*edge)->ctrlB == controlB,U"Bezier control-point doubles survive exactly");
		const auto* restoredObject = restored.getObject(objectId);
		context.expect(restoredObject && restoredObject->arcPos == object.arcPos,U"Nonempty trailing object survives version-aware record skipping");
		const auto originalCurve = source.getBezier(*edge), restoredCurve = restored.getBezier(*edge);
		context.expect(originalCurve.has_value() && restoredCurve.has_value(),U"Restored geometry provides Bezier samples");
		if (originalCurve && restoredCurve)
		{
			for (const float t : {0.0f,.125f,.5f,.875f,1.0f})
			{
				context.expect(originalCurve->evaluate(t) == restoredCurve->evaluate(t),U"Reconstructed Bezier is exactly unchanged");
			}
		}
		source.getEdge(*edge)->ctrlA.x = std::numeric_limits<double>::infinity();
		context.expect(!RoadBinary::writeGlobal(path,source),U"Nonfinite road geometry is rejected before writing");
		std::ifstream input{"TestResults/double_precision_roads.bin",std::ios::binary};
		const std::vector<char> valid{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
		input.close();
		// Two single-attachment nodes occupy 44 bytes each after the 30-byte header.
		constexpr size_t kControlOffset = 30 + 2 * 44 + 12;
		for (const size_t length : {size_t{34},size_t{38},size_t{44},size_t{50},kControlOffset+4,kControlOffset+28})
		{
			std::ofstream output{"TestResults/double_precision_roads.bin",std::ios::binary|std::ios::trunc};
			output.write(valid.data(),static_cast<std::streamsize>(length)); output.close();
			RoadNetwork rejected;
			context.expect(!RoadBinary::readGlobal(path,rejected,true),U"Reject truncated double coordinates");
			context.expect(rejected.nodes().isEmpty(),U"Coordinate decode failure cannot install a partial graph");
		}
		RoadNetwork legacy;
		context.expect(RoadBinary::readGlobal(U"../fixtures/road_v18.bin",legacy,true),U"Retained v18 float-coordinate fixture and extended state still decode");
		context.expectEqual(legacy.nodes().size(),size_t{2},U"Legacy node count");
		context.expectEqual(legacy.edges().size(),size_t{1},U"Legacy edge count");
		if (!legacy.edges().isEmpty())
		{
			context.expect(legacy.edges().front().ctrlA == Vec3{480,0,506} && legacy.edges().front().ctrlB == Vec3{544,0,518},U"Legacy control triples widen correctly");
		}
		context.expectEqual(legacy.plans().size(),size_t{1},U"Legacy trailing construction plan restored");
		if (!legacy.plans().isEmpty())
		{
			context.expect(legacy.plans().front().constructionStart == Optional<GameTime>{1000},U"Legacy construction clock remains aligned");
			context.expectNear(legacy.plans().front().constructionDuration,100,0,U"Legacy construction duration remains aligned");
		}

		if (!legacy.nodes().isEmpty())
		{
			context.expect(legacy.nodes().front().position == Vec3{432,0,512},U"Legacy float coordinates widen correctly");
		}
	});
	runner.add(U"DevelopmentSnapshot.RoadGeometryAndSignsRoundTrip",[](TestContext& context)
	{
		RoadNetwork source;
		const int center = source.addNode({0,0,0});
		Array<int> edges;
		for (const Vec3 end : { Vec3{-100,0,0}, Vec3{100,0,0}, Vec3{0,0,100}, Vec3{0,0,-100} })
		{
			const int endpoint = source.addNode(end);
			const auto edge = source.addEdge(center,endpoint,end/3,end*2/3,RoadType::Arterial,4);
			context.expect(edge.has_value(),U"Fixture edge created");
			if (!edge) { return; }
			edges << *edge;
			source.getEdge(*edge)->edgeState = EdgeState::Open;
			for (auto& part : source.getEdge(*edge)->parts) { part.build = BuildState::Built; }
		}
		source.getNode(center)->getAttachment(edges[0])->isThrough = true;
		source.getNode(center)->getAttachment(edges[1])->isThrough = true;
		source.rebuildLaneConnections(center);
		source.updateNodeCutoffs(center);
		source.getNode(center)->signalPlacement.reset();
		for (auto& attachment : source.getNode(center)->attachments) { attachment.control = TrafficControl::None; }
		source.getNode(center)->getAttachment(edges[0])->control = TrafficControl::Stop;
		for (const int id : edges)
		{
			auto* edge = source.getEdge(id);
			edge->cutoffA = 17.125f + id;
			edge->cutoffB = 3.5f + id;
			edge->signs.clear();
		}
		source.getNode(center)->attachments.reverse();
		for (const auto& node : source.nodes()) { source.updateLaneConnectionPaths(node.id); }
		const FilePath first = U"TestResults/authoritative_roads.bin", second = U"TestResults/authoritative_roads_again.bin";
		context.expect(RoadBinary::writeGlobal(first,source),U"Persist custom end geometry and deliberately absent automatic signs");
		RoadNetwork loaded;
		const bool restored = RoadBinary::readGlobal(first,loaded,true);
		context.expect(restored,U"Restore authoritative road state");
		if (!restored || !loaded.getNode(center)) { return; }
		context.expect(loaded.getNode(center)->type == NodeType::Diverge,U"Saved node classification survives topology reconstruction");
		context.expect(!loaded.getNode(center)->signalPlacement,U"Load preserves deliberately absent signal placement");
		for (const int id : edges)
		{
			const auto* before = source.getEdge(id);
			const auto* after = loaded.getEdge(id);
			context.expect(after != nullptr,U"Saved edge retained");
			if (!after) { return; }
			context.expectNear(after->cutoffA,before->cutoffA,0,U"Start cutoff restored exactly");
			context.expectNear(after->cutoffB,before->cutoffB,0,U"End cutoff restored exactly");
			context.expect(after->signs.isEmpty(),U"Load does not regenerate deliberately absent signs");
		}
		const auto paths = loaded.getNode(center)->laneConnections;
		context.expect(paths.size()>8,U"Fixture exercises derived paths and automatic signal threshold");
		loaded.updateLaneConnectionPaths(center);
		const auto& refreshed = loaded.getNode(center)->laneConnections;
		context.expectEqual(paths.size(),refreshed.size(),U"Path refresh preserves connections");
		for (size_t index = 0; index < paths.size(); ++index)
		{
			context.expectEqual(paths[index].id,refreshed[index].id,U"Path restoration preserves logical IDs");
			context.expectNear((paths[index].path.p0-refreshed[index].path.p0).length(),0,0,U"Restored entry path already uses persisted cutoff");
			context.expectNear((paths[index].path.p3-refreshed[index].path.p3).length(),0,0,U"Restored exit path already uses persisted cutoff");
		}
		context.expect(RoadBinary::writeGlobal(second,loaded),U"Re-save restored road snapshot");
		auto fileBytes=[](const char* path)
		{
			std::ifstream input{path,std::ios::binary};
			return std::vector<char>{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
		};
		context.expect(fileBytes("TestResults/authoritative_roads.bin")==fileBytes("TestResults/authoritative_roads_again.bin"),U"All persisted road fields remain byte stable");
		RoadNetwork legacy;
		context.expect(RoadBinary::readGlobal(first,legacy),U"Default legacy road restoration remains available");
		context.expect(legacy.getEdge(edges[0]) && legacy.getEdge(edges[0])->cutoffA != source.getEdge(edges[0])->cutoffA,U"Legacy path continues its original cutoff recalculation");
		source.getEdge(edges[0])->cutoffA = std::numeric_limits<float>::infinity();
		context.expect(RoadBinary::writeGlobal(second,source),U"Create malformed-cutoff fixture");
		RoadNetwork rejected;
		context.expect(!RoadBinary::readGlobal(second,rejected,true),U"Reject nonfinite authoritative cutoff before installing roads");
		context.expect(rejected.nodes().isEmpty() && rejected.edges().isEmpty(),U"Malformed cutoff leaves target network untouched");
	});
	runner.add(U"DevelopmentSnapshot.GeneratedBaselineRoundTrip",[](TestContext& context)
	{
		World source; prepareWorld(source); populate(source);
		context.expect(DevelopmentSnapshot::write(kSnapshotPath,source),U"Write generated cells without an edited-cell overlay");
		context.expect(DevelopmentSnapshot::matches(kSnapshotPath,source),U"Verify every persisted scalar and polygon coordinate");
		World loaded; prepareWorld(loaded);
		loaded.getChunk({0,0})->buildingGrid[{40,40}].type=BuildingType::Detached;
		context.expect(DevelopmentSnapshot::read(kSnapshotPath,loaded),U"Restore baseline");
		context.expect(DevelopmentSnapshot::matches(kSnapshotPath,loaded),U"Full state equals source");
		context.expect(loaded.getChunk({0,0})->buildingGrid[{40,40}].type==BuildingType::None,U"Sparse defaults clear existing buildings");
		context.expect(loaded.getChunk({0,0})->meshDirty,U"Restoration invalidates render cache");
		const auto first=bytes();
		context.expect(DevelopmentSnapshot::write(kSnapshotPath,loaded),U"Second save succeeds");
		context.expect(first==bytes(),U"Second save is byte stable");
		World second; prepareWorld(second);
		context.expect(DevelopmentSnapshot::read(kSnapshotPath,second) && DevelopmentSnapshot::matches(kSnapshotPath,second),U"Second load is exact");
	});
	runner.add(U"DevelopmentSnapshot.CorruptionLeavesWorldUntouched",[](TestContext& context)
	{
		World source; prepareWorld(source); populate(source);
		context.expect(DevelopmentSnapshot::write(kSnapshotPath,source),U"Fixture saved");
		const auto valid=bytes();
		World target; prepareWorld(target);
		target.getChunk({0,0})->buildingGrid[{40,40}].type=BuildingType::Hospital;
		const FilePath before=U"TestResults/development_before.bin";
		context.expect(DevelopmentSnapshot::write(before,target),U"Keep target state");
		auto reject=[&](const std::vector<char>& damaged)
		{
			replaceBytes(damaged);
			context.expect(!DevelopmentSnapshot::read(kSnapshotPath,target),U"Reject malformed snapshot");
			context.expect(DevelopmentSnapshot::matches(before,target),U"Failed decode is atomic");
		};
		for (size_t length : {size_t{0},size_t{8},size_t{36},valid.size()-1})
		{
			reject(std::vector<char>(valid.begin(),valid.begin()+length));
		}
		auto u32=[&](size_t offset)
		{
			uint32 value=0;
			for (size_t i=0;i<4;++i) { value|=static_cast<uint32>(static_cast<unsigned char>(valid[offset+i]))<<(8*i); }
			return value;
		};
		const size_t zoneStart=37, buildingStart=zoneStart+3*u32(25);
		const size_t patchStart=buildingStart+31*u32(29);
		const size_t secondChunk=patchStart+25+16*u32(patchStart+21);
		auto damaged=valid; damaged[0]^=1; reject(damaged);
		damaged=valid; damaged[4]=127; reject(damaged);
		damaged=valid; damaged[6]=0; reject(damaged);
		damaged=valid; damaged[16]=static_cast<char>(255); damaged[17]=static_cast<char>(255); reject(damaged);
		damaged=valid; damaged.push_back(0); reject(damaged);
		damaged=valid; damaged[zoneStart+2]=static_cast<char>(255); reject(damaged);
		damaged=valid; damaged[buildingStart+2]=static_cast<char>(255); reject(damaged);
		damaged=valid; damaged[24]=2; reject(damaged);
		damaged=valid; damaged[33]=static_cast<char>(255); damaged[34]=static_cast<char>(255); damaged[35]=static_cast<char>(255); reject(damaged);
		damaged=valid;
		for (size_t i=3;i<11;++i) { damaged[buildingStart+i]=0; }
		damaged[buildingStart+9]=static_cast<char>(0xf8); damaged[buildingStart+10]=0x7f; reject(damaged);
		damaged=valid; damaged[buildingStart+31]=damaged[buildingStart]; damaged[buildingStart+32]=damaged[buildingStart+1]; reject(damaged);
		damaged=valid; damaged[zoneStart+3]=damaged[zoneStart]; damaged[zoneStart+4]=damaged[zoneStart+1]; reject(damaged);
		damaged=valid;
		for (size_t i=0;i<4;++i) { damaged[secondChunk+i]=valid[20+i]; }
		reject(damaged);
		// Infinity in each independently persisted floating-point field must fail atomically.
		for (const size_t offset : {buildingStart+3, patchStart+25, patchStart+33})
		{
			damaged=valid;
			for (size_t i=0;i<8;++i) { damaged[offset+i]=0; }
			damaged[offset+6]=static_cast<char>(0xf0); damaged[offset+7]=0x7f; reject(damaged);
		}
		for (const size_t offset : {buildingStart+11,buildingStart+19,buildingStart+23,buildingStart+27,patchStart+5})
		{
			damaged=valid;
			for (size_t i=0;i<4;++i) { damaged[offset+i]=0; }
			damaged[offset+2]=static_cast<char>(0x80); damaged[offset+3]=0x7f; reject(damaged);
		}

		replaceBytes(valid);
	});
	runner.add(U"DevelopmentSnapshot.ChunkSetAndSourceValidation",[](TestContext& context)
	{
		World source; prepareWorld(source); populate(source);
		context.expect(DevelopmentSnapshot::write(kSnapshotPath,source),U"Fixture saved");
		World missing; prepareWorld(missing);
		missing.getChunk({63,63})->heightMap.clear();
		context.expect(!DevelopmentSnapshot::read(kSnapshotPath,missing),U"Reject missing target terrain chunks");
		source.getChunk({0,0})->zoneMap[{0,0}]=static_cast<ZoneType>(255);
		context.expect(!DevelopmentSnapshot::write(kSnapshotPath,source),U"Reject invalid source enums");
	});
	runner.add(U"DevelopmentSnapshot.AuthoritativeWorldKeepsEdits",[](TestContext& context)
	{
		World original; prepareWorld(original); ZoneManager manager;
		manager.paintZone(original,{504,10,520},ZoneType::LowResidential,0);
		const JSON overlay=manager.saveState(original);
		World restored; prepareWorld(restored);
		restored.getChunk({0,0})->zoneMap[{31,32}]=ZoneType::Commercial;
		restored.getChunk({0,0})->buildingGrid[{31,32}].type=BuildingType::Shop;
		ZoneManager loaded; loaded.restoreState(overlay,restored,true);
		context.expect(restored.getChunk({0,0})->buildingGrid[{31,32}].type==BuildingType::Shop,U"Old overlay cannot replace authoritative snapshot");
		context.expect(restored.getChunk({0,0})->zoneMap[{31,32}]==ZoneType::Commercial,U"Authoritative zones remain exact");
		context.expect(loaded.saveState(restored).contains(U"edited"),U"Edited-cell bookkeeping survives for subsequent saves");
	});
	runner.add(U"DevelopmentSnapshot.DevelopmentTimingRoundTrip",[](TestContext& context)
	{
		World world; prepareWorld(world);
		world.getChunk({0,0})->zoneMap[{1,1}]=ZoneType::LowResidential;
		JSON state; state[U"count"]=1; state[U"completed"]=7; state[U"time"]=125.5; state[U"cursor"]=0;
		JSON plot; plot[U"x"]=1; plot[U"z"]=1; plot[U"zone"]=2; plot[U"progress"]=.4;
		plot[U"state"]=static_cast<int>(ZoneDevelopmentState::NeedsRoad);
		plot[U"checkedAt"]=124.5; plot[U"nextCheck"]=126.5; state[U"plots"][0]=plot;
		ZoneManager manager; manager.restoreDevelopment(state,world);
		const JSON restored=manager.developmentSnapshot();
		context.expectEqual(restored[U"completed"].get<int>(),7,U"Completion history preserved");
		context.expectNear(restored[U"time"].get<double>(),125.5,0,U"Development clock preserved");
		const auto restoredPlot=restored[U"plots"][0];
		context.expectEqual(restoredPlot[U"state"].get<int>(),static_cast<int>(ZoneDevelopmentState::NeedsRoad),U"Blocked status preserved");
		context.expectNear(restoredPlot[U"checkedAt"].get<double>(),124.5,0,U"Elapsed-work origin preserved");
		context.expectNear(restoredPlot[U"nextCheck"].get<double>(),126.5,0,U"Next retry time preserved");
		context.expectNear(restoredPlot[U"progress"].get<double>(),.4,0,U"Construction progress preserved");
	});

	runner.add(U"DevelopmentSnapshot.ReferenceValidation",[](TestContext& context)
	{
		World world; prepareWorld(world); RoadNetwork roads;
		const int a=roads.addNode({0,10,0}), b=roads.addNode({100,10,0});
		const auto edge=roads.addEdge(a,b,{30,10,0},{70,10,0});
		context.expect(edge.has_value(),U"Fixture road exists");
		if (!edge) { return; }
		auto& chunk=*world.getChunk({0,0});
		chunk.buildingGrid[{1,1}]=Building{BuildingType::Detached,0,0,*edge,.5f,0,0};
		LandPatch patch; patch.type=LandPatchType::GardenSoil; patch.sourceParcelKey=ZoneGrid::zoneCellKey({0,0},1,1);
		patch.polygon={{16,16},{32,16},{32,32},{16,32}}; chunk.landPatches << patch;
		context.expect(WorldSnapshotValidation::references(world,roads),U"Existing parcel and road accepted");
		const FilePath slot=U"TestResults/reference_guard_slot";
		auto commit=[&]()
		{
			return SaveTransaction::commit(slot,[&](const FilePath& directory)
			{
				if (!WorldSnapshotValidation::references(world,roads))
				{
					return SaveResult::failed(SaveError::VerificationFailed,U"Invalid references",directory);
				}
				return DevelopmentSnapshot::write(directory+U"/development.bin",world)
					? SaveResult::succeeded(directory) : SaveResult::failed(SaveError::WriteFailed,U"Write failed",directory);
			},[&](const FilePath& directory)
			{
				return DevelopmentSnapshot::matches(directory+U"/development.bin",world)
					? SaveResult::succeeded(directory) : SaveResult::failed(SaveError::VerificationFailed,U"Mismatch",directory);
			});
		};
		context.expect(static_cast<bool>(commit()),U"Valid snapshot published atomically");
		chunk.landPatches.front().sourceParcelKey=64;
		context.expect(!commit(),U"Malformed parcel key cannot overwrite a valid save");
		chunk.landPatches.front().sourceParcelKey=patch.sourceParcelKey;
		context.expect(DevelopmentSnapshot::matches(slot+U"/development.bin",world),U"Previous valid snapshot remains intact");
		chunk.landPatches.clear();
		context.expect(WorldSnapshotValidation::references(world,roads),U"Existing building with absent parcel remains preservable");
		chunk.landPatches << patch; chunk.buildingGrid[{1,1}].edgeId=999999;
		context.expect(WorldSnapshotValidation::references(world,roads),U"Historical frontage remains valid after road changes");
		chunk.buildingGrid[{1,1}].edgeId=*edge; chunk.landPatches.front().sourceParcelKey=64;
		context.expect(!WorldSnapshotValidation::references(world,roads),U"Out-of-grid parcel key rejected");
		chunk.landPatches.front().sourceParcelKey=patch.sourceParcelKey;
		roads.removeEdge(*edge);
		context.expect(WorldSnapshotValidation::references(world,roads),U"Deleting a road does not prevent saving its disconnected buildings");
		context.expect(static_cast<bool>(commit()),U"Road-deleted world remains savable");
		World loaded; prepareWorld(loaded);
		context.expect(DevelopmentSnapshot::read(slot+U"/development.bin",loaded),U"Road-deleted world loads");
		context.expect(DevelopmentSnapshot::matches(slot+U"/development.bin",loaded),U"Disconnected building state remains exact");
		context.expectEqual(loaded.getChunk({0,0})->buildingGrid[{1,1}].edgeId,*edge,U"Historical frontage is preserved without deleting house");
	});

}
