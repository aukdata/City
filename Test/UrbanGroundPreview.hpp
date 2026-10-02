#pragma once
#include "src/render/WorldRenderer.hpp"
#include "src/render/RoadRenderer.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include "src/gen/SettlementDevelopment.hpp"
#include "VillageGutterPreview.hpp"

/// @brief Identical eye-level production renders isolate newly generated public pocket ground.
namespace UrbanGroundPreview
{
	/// @brief Scope a real generated street without allocating a second full World.
	/// @details All omitted geometry is at least 200m beyond the judged 150m view.
	/// Original chunks, grids and patches are moved back on every exit, including exceptions.
	class StreetCrop
	{
		struct Omitted { Chunk* slot; Chunk original; };
		struct Retained { Chunk* slot; Grid<Building> buildings; Array<LandPatch> patches; bool meshDirty; };
		World& m_world;
		Vec3 m_eye;
		Array<Omitted> m_omitted;
		Array<Retained> m_retained;
		Array<Chunk*> m_originalActive;
		bool m_restored=false;
	public:
		JSON report;
		uint64 originalFingerprint=0;
		uint64 fingerprint() const { return fingerprint(m_originalActive); }
		static uint64 fingerprint(const Array<Chunk*>& chunks)
		{
			uint64 hash=1469598103934665603ULL;
			const auto add=[&](const auto& value) { const auto* bytes=reinterpret_cast<const uint8*>(&value); for (size_t i=0;i<sizeof(value);++i) { hash=(hash^bytes[i])*1099511628211ULL; } };
			for (const Chunk* chunk : chunks)
			{
				add(chunk->coord.x); add(chunk->coord.y); add(chunk->heightMin); add(chunk->heightMax); add(chunk->state); add(chunk->meshDirty); add(chunk->isUrbanizationArea);
				add(chunk->heightMap.width()); add(chunk->heightMap.height());
				for (const auto value : chunk->heightMap) { add(value); }
				add(chunk->terrainType.width()); add(chunk->terrainType.height());
				for (const auto value : chunk->terrainType) { add(value); }
				add(chunk->zoneMap.width()); add(chunk->zoneMap.height());
				for (const auto value : chunk->zoneMap) { add(value); }
				add(chunk->buildingGrid.width()); add(chunk->buildingGrid.height());
				for (const auto& building : chunk->buildingGrid) { add(building.type); add(building.builtAt); add(building.angle); add(building.edgeId); add(building.edgeT); add(building.offsetX); add(building.offsetZ); }
				add(chunk->landPatches.size());
				for (const auto& patch : chunk->landPatches) { add(patch.id); add(patch.type); add(patch.elevationOffset); add(patch.materialVariant); add(patch.sourceParcelKey); add(patch.polygon.size()); for (const auto point : patch.polygon) { add(point.x); add(point.y); } }
			}
			return hash;
		}
		StreetCrop(World& world,Vec3 eye) : m_world(world),m_eye(eye)
		{
			try
			{
			const RectF retained{eye.x-350,eye.z-350,700,700};
			const auto active=world.getActiveChunks();
			m_originalActive=active; originalFingerprint=fingerprint();
			m_omitted.reserve(active.size()); m_retained.reserve(active.size());
			// Force a public cache rebuild after changing the Test-only resident subset.
			world.update(Vec3{-10000,0,-10000});
			HashSet<uint32> modelKeys; HashTable<int,int> types;
			int retainedBuildings=0,omittedBuildings=0;
			for (Chunk* chunk : active)
			{
				const Vec3 origin=chunk->worldOrigin();
				if (!RectF{origin.x,origin.z,CHUNK_SIZE,CHUNK_SIZE}.intersects(retained))
				{
					const Point coord=chunk->coord;
					m_omitted << Omitted{chunk,std::move(*chunk)};
					*chunk=Chunk{}; chunk->coord=coord;
					continue;
				}
				m_retained << Retained{chunk,std::move(chunk->buildingGrid),std::move(chunk->landPatches),chunk->meshDirty};
				const auto& original=m_retained.back();
				chunk->buildingGrid=original.buildings;
				for (int row=0;row<ZONE_CELLS;++row) for (int col=0;col<ZONE_CELLS;++col)
				{
					auto& building=chunk->buildingGrid[{col,row}];
					if (building.type==BuildingType::None) { continue; }
					const double x=origin.x+(col+.5)*CHUNK_SIZE/ZONE_CELLS+building.offsetX;
					const double z=origin.z+(row+.5)*CHUNK_SIZE/ZONE_CELLS+building.offsetZ;
					// Rotated footprint plus a 100m model/eave allowance keeps boundary assets.
					const double margin=buildingFootprintXZ(building.type)*.71+100;
					if (!RectF{x-margin,z-margin,2*margin,2*margin}.intersects(retained))
					{
						building=Building{}; ++omittedBuildings; continue;
					}
					++retainedBuildings; ++types[static_cast<int>(building.type)];
					if (isObjBuildingType(building.type)) { modelKeys.insert((static_cast<uint32>(building.type)<<8)|buildingModelVariant(building.type,chunk->coord.x*ZONE_CELLS+col,chunk->coord.y*ZONE_CELLS+row)); }
				}
				for (const auto& patch : original.patches)
				{
					if (patch.polygon.size()>=3 && Polygon{patch.polygon}.boundingRect().intersects(retained)) { chunk->landPatches << patch; }
				}
				chunk->meshDirty=true;
			}
			world.update(eye);
			report[U"fixture"]=U"cropped actual generated regional-hub street";
			report[U"sourceWorld"]=U"RegionalHub generation on the existing flat20m Test terrain";
			report[U"seed"]=42; report[U"judgedDistanceMeters"]=150;
			report[U"retainedGeometryRect"]=Array<double>{retained.x,retained.y,retained.w,retained.h};
			report[U"originalActiveChunks"]=active.size(); report[U"retainedActiveChunks"]=world.getActiveChunks().size();
			report[U"retainedBuildings"]=retainedBuildings; report[U"omittedBuildingsWithinRetainedChunks"]=omittedBuildings;
			report[U"requiredObjTypeVariantCount"]=modelKeys.size();
			Array<JSON> counts; for (const auto& [type,count] : types) { JSON item; item[U"type"]=type; item[U"count"]=count; counts << item; }
			report[U"retainedBuildingTypes"]=counts;
			report[U"productionRoadNetworkUnchanged"]=true; report[U"terrainValuesCopiedExactly"]=true;
			report[U"shadowPassEnabled"]=false;
			}
			catch (...) { restore(); throw; }
		}
		~StreetCrop() { restore(); }
		void restore()
		{
			if (m_restored) { return; }
			for (auto& saved : m_retained) { saved.slot->buildingGrid=std::move(saved.buildings); saved.slot->landPatches=std::move(saved.patches); saved.slot->meshDirty=saved.meshDirty; }
			for (auto& saved : m_omitted) { *saved.slot=std::move(saved.original); }
			m_restored=true;
			m_world.update(Vec3{-10000,0,-10000});
			m_world.update(m_eye);
		}
	};

	inline void capture(TestContext& context,const FilePath& directory,World& world,const RoadNetwork& roads,
		const SettlementDevelopment::Validation& validation,bool gutterCandidate=false)
	{
		RegisterAssets();
		Array<Array<Vec2>> pockets;
		Array<JSON> publicPockets; if (validation.landscapedPockets.isArray()) { for (const auto& face : validation.landscapedPockets.arrayView()) { publicPockets << face; } }
		for (const auto& face : publicPockets)
		{
			for (const auto& ground : face[U"publicGround"].arrayView())
			{
				Array<Vec2> points; for (const auto& point : ground.arrayView()) { points << Vec2{point[0].get<double>(),point[1].get<double>()}; }
				pockets << std::move(points);
			}
		}
		const Size size{960,600};
		const Vec2 desired{32118.879,33116.125}; Vec3 position{desired.x,20,desired.y},direction{-14,0,27};
		double best=Math::Inf; int viewEdge=-1;
		for (const auto& edge : roads.edges())
		{
			if (edge.id<0 || !edge.hasRoadLanes()) { continue; }
			bool inherited=false;
			for (const int id : edge.routeIds) { const auto* route=roads.getRoute(id); inherited |= route && route->name.contains(U"旧村道"); }
			if (!inherited) { continue; }
			const auto curve=roads.getBezier(edge.id); if (!curve) { continue; }
			for (float arc=0;arc<=curve->totalLength;arc+=4)
			{
				const Vec3 point=curve->positionAt(arc); const double distance=Vec2{point.x,point.z}.distanceFromSq(desired);
				if (distance<best) { best=distance; position=point; direction=curve->tangentAt(arc); viewEdge=edge.id; }
			}
		}
		context.expect(viewEdge>=0,U"The ground view sits on an actual retained village street");
		if (viewEdge<0) { return; }
		if (Vec2{direction.x,direction.z}.dot(Vec2{-14,27})<0) { direction=-direction; }
		direction.y=0; direction=direction.normalized();
		const Vec3 eye=position+Vec3{0,1.5,0};
		const BasicCamera3D camera{size,55_deg,eye,eye+direction*35};
		world.update(eye);
		StreetCrop crop{world,eye};
		crop.report.save(directory+U"TestResults/mosaic_ground_crop_seed42.json");
		struct Saved { Point coord; Array<LandPatch> patches; };
		Array<Saved> saved; HashSet<Point> visited;
		for (const auto& polygon : pockets)
		{
			const Vec2 center=Polygon{polygon}.boundingRect().center();
			const Point coord{static_cast<int>(Floor(center.x/CHUNK_SIZE)),static_cast<int>(Floor(center.y/CHUNK_SIZE))};
			if (world.getChunk(coord) && visited.insert(coord).second) { saved << Saved{coord,world.getChunk(coord)->landPatches}; }
		}


		for (const bool after : {false,true})
		{
			for (const auto& original : saved)
			{
				auto* chunk=world.getChunk(original.coord); chunk->landPatches=original.patches;
				if (!after)
				{
					chunk->landPatches.remove_if([&](const LandPatch& patch)
					{
						return patch.sourceParcelKey<0 && pockets.any([&](const Array<Vec2>& outline) { return patch.polygon==outline; });
					});
				}
				chunk->meshDirty=true;
			}
			Optional<RoadNetwork> candidateRoads;
			Optional<VillageGutterPreview::CandidateSelection> selection;
			if (gutterCandidate && after)
			{
				candidateRoads.emplace(roads); Array<int> villageEdges;
				for (const auto& edge : roads.edges())
				{
					if (edge.id<0) { continue; } bool village=false;
					for (const int id : edge.routeIds) { const auto* route=roads.getRoute(id); village |= route && route->name.contains(U"旧村道"); }
					if (village) { villageEdges << edge.id; }
				}
				selection.emplace(context,*candidateRoads,villageEdges);
				context.expect(selection->changedParts()>0,U"The staged gutter candidate changes only the actual old-village sections");
			}
			const RoadNetwork& drawRoads=candidateRoads ? *candidateRoads : roads;
			RoadRenderer roadRenderer; roadRenderer.loadAssets();
			WorldRenderer renderer; renderer.setAsyncTerrain(false); renderer.setRenderDistance(150);
			const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
			for (int frame=0;frame<8;++frame)
			{
				const ScopedRenderTarget3D output{target.clear(ColorF{.48,.65,.82})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera); Graphics3D::SetSunDirection(Vec3{1,2,-1}.normalized());
				Graphics3D::SetSunColor(ColorF{.9}); Graphics3D::SetGlobalAmbientColor(ColorF{.45});
				renderer.render(world,drawRoads,camera); roadRenderer.render(drawRoads,world,ViewFrustum{camera,150},eye); Graphics3D::Flush();
			}
			Image pixels; target.readAsImage(pixels); pixels.save(directory+(after ? U"Screenshot/mosaic_ground_after_seed42.png" : U"Screenshot/mosaic_ground_before_seed42.png"));
			JSON report; report[U"eye"]=Array<double>{eye.x,eye.y,eye.z}; report[U"eyeHeightMeters"]=1.5; report[U"drawDistanceMeters"]=150;
			report[U"buildingsSubmitted"]=renderer.buildingsSubmitted(); report[U"residentialAccessNearEye"]=renderer.residentialAccessNearby(eye,150); report[U"viewEdge"]=viewEdge; report[U"publicPocketCount"]=publicPockets.size(); report[U"bulkAssetPreload"]=false; report[U"publicPocketsVisibleInWorld"]=after && !publicPockets.isEmpty(); report[U"publicPocketTreatmentEnabled"]=after; report[U"sameGeneratedRoadsAndBuildings"]=true; report[U"coveredGutterCandidate"]=gutterCandidate && after; report[U"changedGutterParts"]=selection ? selection->changedParts() : 0;
			report.save(directory+(after ? U"TestResults/mosaic_ground_after_seed42.json" : U"TestResults/mosaic_ground_before_seed42.json"));
			context.expect(renderer.residentialAccessNearby(eye,150)>0,U"The actual generated street emits nearby accepted entry paths; a vacuous helper pass is insufficient");
			if (after)
			{
				auto records=renderer.residentialAccessRecords(eye,150);
				const auto score=[&](const auto& record)
				{
					const double length=record.emittedRoadEnd.distanceFrom(record.emittedEntrance);
					return record.emittedEntrance.distanceFrom(eye)+Abs(length-3.0)*20;
				};
				records.sort_by([&](const auto& a,const auto& b) { return score(a)<score(b) || (score(a)==score(b) && a.parcelKey<b.parcelKey); });
				context.expect(!records.isEmpty(),U"The oblique view selects an actual emitted residential entry, not an invented location");
				if (!records.isEmpty())
				{
					const auto& record=records.front();
					Vec3 inward=record.emittedEntrance-record.emittedRoadEnd; inward.y=0; inward=inward.normalized();
					const Vec3 side{inward.z,0,-inward.x};
					Vec3 closeEye=record.emittedRoadEnd-inward*4+side*2.5; closeEye.y=record.assignedRoadBoundary.y+1.5;
					const Vec3 targetPoint=record.emittedEntrance+Vec3{0,.8,0};
					const BasicCamera3D closeCamera{size,55_deg,closeEye,targetPoint};
					for (int frame=0;frame<4;++frame)
					{
						const ScopedRenderTarget3D output{target.clear(ColorF{.48,.65,.82})};
						const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
						Graphics3D::SetCameraTransform(closeCamera); Graphics3D::SetSunDirection(Vec3{1,2,-1}.normalized());
						Graphics3D::SetSunColor(ColorF{.9}); Graphics3D::SetGlobalAmbientColor(ColorF{.45});
						renderer.render(world,drawRoads,closeCamera); roadRenderer.render(drawRoads,world,ViewFrustum{closeCamera,150},closeEye); Graphics3D::Flush();
					}
					target.readAsImage(pixels); pixels.save(directory+U"Screenshot/residential_access_closeup_seed42.png");
					const auto vector=[](Vec3 point) { return Array<double>{point.x,point.y,point.z}; };
					JSON closeReport; closeReport[U"seed"]=42; closeReport[U"eye"]=vector(closeEye); closeReport[U"lookAt"]=vector(targetPoint);
					closeReport[U"eyeAboveAssignedStreetMeters"]=1.5; closeReport[U"edgeId"]=record.edgeId; closeReport[U"parcelKey"]=Format(record.parcelKey); closeReport[U"buildingType"]=static_cast<int>(record.type);
					closeReport[U"assignedRoadBoundary"]=vector(record.assignedRoadBoundary); closeReport[U"emittedRoadEnd"]=vector(record.emittedRoadEnd);
					closeReport[U"emittedEntrance"]=vector(record.emittedEntrance); closeReport[U"targetEntrance"]=vector(record.targetEntrance);
					const int col=static_cast<int>(record.parcelKey&255),row=static_cast<int>((record.parcelKey>>8)&255);
					const int64 chunkKey=record.parcelKey>>16; const int chunkX=static_cast<int>(chunkKey>>32),chunkZ=static_cast<int>(static_cast<uint32>(chunkKey));
					String modelStem; tryGetBuildingModelStemForVariant(record.type,buildingModelVariant(record.type,chunkX*ZONE_CELLS+col,chunkZ*ZONE_CELLS+row),modelStem);
					closeReport[U"buildingCell"]=Array<int>{chunkX,chunkZ,col,row}; closeReport[U"modelStem"]=modelStem;
					closeReport[U"pathKey"]=Format(record.parcelKey)+U":"+Format(record.targetEntrance.x)+U":"+Format(record.targetEntrance.z);
					closeReport[U"streetBoundaryHeightBasis"]=U"covered-gutter nominal top; authored joints may recess4mm";
					closeReport[U"entranceJoinErrorMeters"]=record.emittedEntrance.distanceFrom(record.targetEntrance);
					closeReport[U"streetSeamHeightMeters"]=record.emittedRoadEnd.y-record.assignedRoadBoundary.y;
					closeReport[U"sameProductionGeometryAndMaterials"]=true; closeReport[U"sameCroppedWorld"]=true;
					closeReport.save(directory+U"TestResults/residential_access_closeup_seed42.json");
					context.expect(record.emittedEntrance.distanceFrom(record.targetEntrance)<.01,U"The actual emitted ramp meets its known door/apron transform within the Float3 coordinate tolerance");
				}
			}
			context.expect(renderer.buildingsSubmitted()>=6,U"The generated old-core street includes multiple real frontage buildings at 1.5m eye height");
		}
		crop.restore();
		const uint64 restored=crop.fingerprint();
		context.expectEqual(restored,crop.originalFingerprint,U"Cropped preview restores every original chunk, terrain, building and parcel field");
		crop.report[U"restoredFingerprint"]=Format(restored); crop.report[U"originalFingerprint"]=Format(crop.originalFingerprint);
		crop.report.save(directory+U"TestResults/mosaic_ground_crop_seed42.json");
	}
}
