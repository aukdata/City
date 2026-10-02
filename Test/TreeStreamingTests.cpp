#include "TestCases.hpp"
#include "src/render/ShaderAsset.hpp"
#include "TestRunner.hpp"
#include "src/render/TreeInstances.hpp"
#include "src/render/TreeGeometry.hpp"
#include "src/render/LandscapeMaterials.hpp"
#include "src/render/WorldRenderer.hpp"
#include "src/render/CityLighting.hpp"
#include "src/asset/AssetRegistrar.hpp"
#include <thread>

namespace
{
	/// @brief ゲームと同じシェーダー資源を使い、テスト終了時に作業場所を戻す。
	struct GameAssets
	{
		FilePath previous = FileSystem::CurrentDirectory();
		GameAssets() { FileSystem::ChangeCurrentDirectory(previous + U"../../App/"); }
		~GameAssets() { FileSystem::ChangeCurrentDirectory(previous); }
	};
}

void registerTreeStreamingTests(TestRunner& runner)
{
	runner.add(U"RenderDistance.SettingsAndBounds", [](TestContext& context)
	{
		for (const String input : {U"0",U"100",U"1500.5",U"20000",U"100.5",U"+500",U"1e3"})
		{
			context.expect(RenderDistance::parse(input).has_value(),U"CLI accepts valid meters: " + input);
		}
		for (const String input : {U"",U"-1",U"99",U"20001",U"nan",U"inf",U"500m",U"500.0junk",U"500 600",U"1e3m",U"500e",U"500,1",U"+-0",U"++500",U"0x1p10",U"1e309",U"1e-9999",U"--new"})
		{
			context.expect(!RenderDistance::parse(input),U"CLI rejects invalid meters: " + input);
		}
		const Vec3 eye{0,0,0};
		context.expect(RenderDistance::contains(eye,Sphere{Vec3{200,0,0},100},100),U"A large object's near edge on the boundary is retained");
		context.expect(!RenderDistance::contains(eye,Sphere{Vec3{201,0,0},100},100),U"A sphere wholly outside the cutoff is culled");
		context.expect(RenderDistance::contains(eye,Box{Vec3{200,0,0},Vec3{200,20,20}},100),U"A batch is tested by its nearest face, not its origin");
		context.expect(!RenderDistance::contains(eye,Box{Vec3{201,0,0},Vec3{200,20,20}},100),U"A wholly distant batch is culled");
		context.expect(RenderDistance::contains(eye,Box{Vec3{100000,0,0},Vec3{20,20,20}},0),U"Zero preserves the previous additional-distance behavior");
		const auto model=RenderDistance::transformBox(Box{Vec3{20,10,0},Vec3{100,20,10}},2,90_deg,Vec3{100,0,0});
		context.expectNear(model.center.distanceFrom(Vec3{100,20,-40}),0,.0001,U"Off-center model bounds follow scale, rotation and translation");
		context.expect(RenderDistance::contains(eye,RenderDistance::buildingSphere(model),100),U"A wide rotated building remains visible when its edge is near");
		for (int material : {100,101,119,127,130,131})
		{
			context.expect(!RenderDistance::limitedMaterial(material),U"Ground and cultivated surfaces retain their existing range");
		}
		GameAssets assets; WorldRenderer renderer;
		const uint64 geometry=renderer.geometryRevision(),initial=renderer.shadowRevision();
		context.expect(renderer.setRenderDistance(500),U"The renderer accepts a valid runtime setting");
		const uint64 changed=renderer.shadowRevision();
		context.expect(changed>initial && renderer.geometryRevision()==geometry,U"Changing distance invalidates shadows without rebuilding world geometry");
		renderer.prepareView({100,30,50});const uint64 moved=renderer.shadowRevision();
		context.expect(moved>changed,U"Moving the eye invalidates finite-distance shadow selection");
		renderer.prepareView({100,30,50});
		context.expect(renderer.shadowRevision()==moved,U"An unchanged eye reuses shadows");
		context.expect(!renderer.setRenderDistance(99) && renderer.renderDistance()==500 && renderer.shadowRevision()==moved,U"Invalid settings do not mutate the renderer");
		(void)renderer.setRenderDistance(0);const uint64 restored=renderer.shadowRevision();renderer.prepareView({200,30,50});
		context.expect(renderer.shadowRevision()==restored && renderer.geometryRevision()==geometry,U"Default mode preserves the original shadow caching policy");
	});

	runner.add(U"RenderDistance.TreeBoundsAndSubmission", [](TestContext& context)
	{
		Array<TreeInstance> trees{
			{{Float4{40,0,0,10},Float4{20,1,0,0}},0,0,0},
			{{Float4{500,0,0,10},Float4{20,1,0,0}},0,0,1},
			{{Float4{110,0,0,30},Float4{20,1,0,0}},0,0,0}
		};
		TreeInstanceRenderer renderer;
		const auto submit=[&](double distance,Vec3 eye)
		{
			renderer.clear();renderer.append(trees,{0,0},{0,0},eye,distance);
			return renderer.nearInstances()+renderer.farInstances();
		};
		context.expect(submit(0,{0,0,0})==3,U"Default submits all three trees with existing LOD rules");
		context.expect(submit(100,{0,0,0})==2,U"A crown intersecting the cutoff survives even when its trunk origin is outside");
		context.expect(submit(100,{0,500,0})==0,U"Distance is measured in 3D from the camera eye");
		context.expect(submit(600,{0,0,0})==3 && submit(0,{0,0,0})==3,U"Increasing and resetting distance restores cached trees without regeneration");
		bool allInside=true;
		for (uint32 model=0;model<TreeInstanceRenderer::kModelCount;++model)
		{
			const auto geometry=model==TreeInstanceRenderer::kShrubModel ? TreeGeometry::build(37,false)
				: TreeGeometry::build((model%8)*31,(model%8)>=4,model>=8);
			const TreeInstance tree{{Float4{500,30,200,12},Float4{24,Cos(.73f),Sin(.73f),0}},static_cast<uint8>(model),0,0};
			const Box bounds=TreeInstanceRenderer::bounds(tree);
			for (const auto* source : {&geometry.wood,&geometry.leaves,&geometry.distant})
			{
				auto transformed=*source;transformed.scale(12,24,12).rotate(Quaternion::RotateY(.73)).translate(500,30,200);
				for (const auto& vertex : transformed.vertices)
				{
					const Vec3 delta=Vec3{vertex.pos}-bounds.center;
					allInside &= Abs(delta.x)<=bounds.size.x*.5 && Abs(delta.y)<=bounds.size.y*.5 && Abs(delta.z)<=bounds.size.z*.5;
				}
			}
		}
		context.expect(allInside,U"Actual wood, leaves and distant geometry for every shared tree prototype fit the production culling bounds");
	});

	runner.add(U"RenderDistance.BatchedShadowReadback", [](TestContext& context)
	{
		GameAssets assets; RegisterAssets();
		World world; world.reserveChunks();
		for (int z=0;z<WORLD_CHUNKS;++z)
		{
			for (int x=0;x<WORLD_CHUNKS;++x)
			{
				if (x!=0 || z!=0) { world.getChunk({x,z})->heightMap.clear(); }
			}
		}
		world.installChunkDirect({0,0},HeightMapResult{Grid<float>(HEIGHT_CELLS+1,HEIGHT_CELLS+1,20.0f),20,20});
		auto* chunk=world.getChunk({0,0});
		chunk->buildingGrid[{20,16}].type=BuildingType::IndustrialWarehouse;
		const Vec3 focus{512,20,264},nearEye{300,200,264},farEye{724,200,264};
		world.update(focus);
		RoadNetwork roads; WorldRenderer renderer; renderer.setWoodlandEnabled(false);
		const Size size{320,240};
		const RenderTexture colorTarget{size,TextureFormat::R8G8B8A8_Unorm,HasDepth::Yes};
		const BasicCamera3D colorCamera{size,45_deg,farEye,Vec3{328,24,264}};
		const auto renderColor=[&](double distance)
		{
			(void)renderer.setRenderDistance(distance);
			{
				const ScopedRenderTarget3D target{colorTarget.clear(ColorF{.1,.2,.3})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullNone};
				Graphics3D::SetCameraTransform(colorCamera);
				Graphics3D::SetSunDirection(Vec3{1,2,-1}.normalized());
				Graphics3D::SetSunColor(ColorF{.8}); Graphics3D::SetGlobalAmbientColor(ColorF{.4});
				renderer.render(world,roads,colorCamera);
			}
			Graphics3D::Flush(); Image pixels; colorTarget.readAsImage(pixels); return pixels;
		};
		const auto original=renderColor(0),limited=renderColor(300),restored=renderColor(0);
		size_t removedPixels=0,restoredDifferences=0;
		for (int y=0;y<size.y;++y)
		{
			for (int x=0;x<size.x;++x)
			{
				removedPixels += original[y][x]!=limited[y][x];
				restoredDifferences += original[y][x]!=restored[y][x];
			}
		}
		context.expect(removedPixels>50 && restoredDifferences==0,U"A non-OBJ warehouse's merged material batches disappear at finite distance and restore identically");
		const auto bounds=renderer.buildingHitBox(*chunk,world,20,16);
		const uint64 geometry=renderer.geometryRevision(),terrainBuilds=renderer.terrainBuilds(),treeBuilds=renderer.treePlacementBuilds();
		CityLighting lighting; const bool ready=lighting.initialize();
		context.expect(ready,U"Production shadow shaders compile for actual cached-depth readback");
		if (!ready) { return; }
		size_t callbacks=0; Array<size_t> depthPixels; Grid<float> baseline;
		const auto updateShadow=[&](Vec3 eye,double distance)
		{
			(void)renderer.setRenderDistance(distance); renderer.prepareView(eye);
			const BasicCamera3D camera{size,45_deg,eye,focus};
			lighting.update(camera,focus,Vec3{1,2,-1}.normalized(),1.0,renderer.shadowRevision(),[&](Vec3 shadowFocus,double radius)
			{
				++callbacks; renderer.renderShadowCasters(shadowFocus,radius);
			});
			Grid<float> depth; lighting.readStaticShadowDepth(depth);
			size_t occupied=0; for (const float value : depth) { occupied += value>0.0f; }
			depthPixels << occupied; return depth;
		};
		baseline=updateShadow(farEye,0);
		(void)updateShadow(farEye,300);
		const auto restoredDepth=updateShadow(farEye,0);
		size_t changedDepth=0;
		for (size_t i=0;i<baseline.num_elements();++i) { changedDepth += baseline.data()[i]!=restoredDepth.data()[i]; }
		context.expect(depthPixels[0]>100 && depthPixels[1]==0 && changedDepth==0,U"0-to-finite-to-0 updates the actual shadow texture and restores its original depth exactly");
		(void)updateShadow(farEye,300);
		(void)updateShadow(nearEye,300);
		context.expect(bounds && renderer.buildingWithinRenderDistance(*chunk,20,16,*bounds,nearEye),U"The retained non-OBJ batch is selectable near the camera");
		const size_t beforeReuse=callbacks;
		(void)updateShadow(nearEye,300);
		context.expect(callbacks==beforeReuse,U"An unchanged finite-distance view reuses the actual cached shadow texture");
		(void)updateShadow(farEye,300);
		context.expect(depthPixels[3]==0 && depthPixels[4]>100 && depthPixels[5]==depthPixels[4] && depthPixels[6]==0,
			U"Equal-focus and equal-extent eye movement refreshes shadow visibility in both directions");
		context.expect(callbacks==6,U"Only the six changed views draw shadow casters");
		context.expect(bounds && !renderer.buildingWithinRenderDistance(*chunk,20,16,*bounds,farEye),U"The culled non-OBJ batch cannot be picked");
		context.expect(renderer.geometryRevision()==geometry && renderer.terrainBuilds()==terrainBuilds && renderer.treePlacementBuilds()==treeBuilds,
			U"Shadow-distance changes leave geometry and placement caches intact");
		JSON report; report[U"colorRemovedPixels"]=removedPixels; report[U"colorRestoredDifferences"]=restoredDifferences;
		report[U"shadowPixels"]=depthPixels; report[U"shadowRestoredDifferences"]=changedDepth; report[U"shadowDrawCallbacks"]=callbacks;
		report.save(assets.previous+U"TestResults/render_distance_batched_shadows.json");
	});

	runner.add(U"TreeStreaming.SharedGeometryReadback", [](TestContext& context)
	{
		GameAssets assets;
		CityLighting lighting;
		context.expect(lighting.initialize(), U"Tree and depth shaders compile");
		const PixelShader depth{ShaderAsset::pixel(U"shaders/hlsl/city_forward.hlsl", U"Depth_PS")};
		TreeInstanceRenderer::preload();
		std::array<TreeGeometry::Geometry, 17> originals;
		for (uint32 i = 0; i < originals.size(); ++i)
		{
			originals[i] = i == 16 ? TreeGeometry::build(37, false) : TreeGeometry::build((i % 8) * 31, (i % 8) >= 4, i >= 8);
		}
		JSON report;
		for (const bool detailed : {true, false})
		{
			const int count = detailed ? 87 : 1046, columns = detailed ? 12 : 33;
			Array<TreeInstance> trees;
			std::array<MeshData, 3> reference;
			for (int i = 0; i < count; ++i)
			{
				const uint8 model = static_cast<uint8>(i < (detailed ? 17 : 16) ? i : 8);
				const uint8 palette = static_cast<uint8>(i < 16 ? i % 2 : 0);
				const float angle = static_cast<float>(i % 9) * .37f;
				const float width = 3.4f + static_cast<float>(i % 3) * .3f, height = 7.0f + static_cast<float>(i % 5);
				const Float3 position{static_cast<float>(i % columns) * 7, 0, static_cast<float>(i / columns) * 7};
				trees << TreeInstance{{Float4{position.x, position.y, position.z, width}, Float4{height, Cos(angle), Sin(angle), 0}}, model, palette};
				const auto& original = originals[model];
				auto appendReference = [&](const MeshData& data, int material)
				{
					MeshData transformed = data;
					transformed.scale(width, height, width).rotate(Quaternion::RotateY(angle)).translate(position);
					TreeGeometry::append(reference[material], transformed);
				};
				if (detailed)
				{
					if (model != 16) { appendReference(original.wood, 0); }
					appendReference(original.leaves, 1 + palette);
				}
				else { appendReference(original.distant, 1 + palette); }
			}
			std::array<Mesh, 3> referenceMeshes;
			for (size_t i = 0; i < reference.size(); ++i)
			{
				if (!reference[i].indices.isEmpty()) { referenceMeshes[i] = Mesh{reference[i]}; }
			}
			TreeInstanceRenderer renderer;
			renderer.append(trees, {0, 0}, {0, 0}, detailed ? Vec3{50, 15, 50} : Vec3{50, 3000, 50});
			context.expectEqual(static_cast<int64>(detailed ? renderer.nearInstances() : renderer.farInstances()), count, U"All instances enter the requested LOD including full and partial batches");
			const Size size{640, 480};
			const double extent = columns * 7.0;
			const BasicCamera3D camera{size, 45_deg, Vec3{extent * .5, extent * 1.15, -extent * .65}, Vec3{extent * .5, 0, extent * .5}};
			const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm, HasDepth::Yes};
			const RenderTexture shadow{size, TextureFormat::R32_Float, HasDepth::Yes};
			Array<Image> images;
			Array<Grid<float>> depths;
			for (const bool depthPass : {false, true}) for (int mode = 0; mode < 2; ++mode)
			{
				{
					const ScopedRenderTarget3D scope{(depthPass ? shadow : target).clear(ColorF{0, 0, 0, 0})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite, RasterizerState::SolidCullNone, BlendState::Opaque};
					Graphics3D::SetCameraTransform(camera);
					Graphics3D::SetSunDirection(Vec3{1, 2, -1}.normalized());
					Graphics3D::SetSunColor(ColorF{.8}); Graphics3D::SetGlobalAmbientColor(ColorF{.4}); lighting.bind();
					const ScopedCustomShader3D shader{depthPass ? depth : lighting.shader()};
					if (mode == 1) { renderer.draw(lighting.foliageShader(), depthPass); }
					else
					{
						if (referenceMeshes[0]) { referenceMeshes[0].draw(LandscapeMaterials::detailColorForKey(105)); }
						Optional<ScopedCustomShader3D> foliage;
						if (!depthPass) { foliage.emplace(lighting.foliageShader()); }
						for (int palette = 0; palette < 2; ++palette)
						{
							if (referenceMeshes[1 + palette]) { referenceMeshes[1 + palette].draw(LandscapeMaterials::detailColorForKey(125 + palette)); }
						}
					}
				}
				Graphics3D::Flush();
				if (depthPass) { Grid<float> values; shadow.read(values); depths << std::move(values); }
				else { Image pixels; target.readAsImage(pixels); images << std::move(pixels); }
			}
			size_t occupied = 0, silhouetteDifferences = 0, depthDifferences = 0;
			double colorError = 0;
			for (int y = 0; y < size.y; ++y) for (int x = 0; x < size.x; ++x)
			{
				const auto a = images[0][y][x], b = images[1][y][x];
				occupied += a.a > 0;
				silhouetteDifferences += (a.a > 0) != (b.a > 0);
				if (a.a && b.a) { colorError += Abs(static_cast<int>(a.r) - b.r) + Abs(static_cast<int>(a.g) - b.g) + Abs(static_cast<int>(a.b) - b.b); }
				depthDifferences += Abs(depths[0][y][x] - depths[1][y][x]) > .00001f;
			}
			const String label = detailed ? U"near" : U"far";
			report[label][U"pixels"] = occupied;
			report[label][U"silhouetteDifferences"] = silhouetteDifferences;
			report[label][U"depthDifferences"] = depthDifferences;
			report[label][U"meanColorError"] = colorError / Max<size_t>(1, occupied * 3);
			context.expect(occupied > 1000, U"Tree geometry is visible on the GPU");
			context.expect(silhouetteDifferences < occupied / 100 + 5, U"Shared geometry matches legacy world-space silhouettes within raster rounding");
			context.expect(depthDifferences < occupied / 100 + 5, U"Shadow depth uses the same individual transforms and partial batch sizes");
			context.expect(colorError / Max<size_t>(1, occupied * 3) < .5, U"Leaf palettes and nonuniform-scale lighting match the reference");
			images[1].save(assets.previous + U"Screenshot/shared_trees_{}.png"_fmt(label));
		}
		report.save(assets.previous + U"TestResults/shared_tree_readback.json");
	});
	runner.add(U"TreeStreaming.LodReuseAndInvalidation", [](TestContext& context)
	{
		GameAssets assets; RegisterAssets();
		World world; world.reserveChunks();
		for (int z = 0; z < WORLD_CHUNKS; ++z) for (int x = 0; x < WORLD_CHUNKS; ++x)
		{
			if (x != 0 || z != 0) { world.getChunk({x, z})->heightMap.clear(); }
		}
		world.installChunkDirect({0, 0}, HeightMapResult{Grid<float>(HEIGHT_CELLS + 1, HEIGHT_CELLS + 1, 20.0f), 20, 20});
		auto* chunk = world.getChunk({0, 0});
		LandPatch patch; patch.type = LandPatchType::GardenSoil; patch.sourceParcelKey = -1;
		patch.polygon = {{100, 100}, {230, 100}, {230, 230}, {100, 230}};
		chunk->landPatches << patch;
		chunk->buildingGrid[{20, 20}].type = BuildingType::ParkBuilding;
		chunk->buildingGrid[{8, 8}].type = BuildingType::Detached;
		world.update({160, 20, 160});
		WorldRenderer renderer; renderer.setAsyncTerrain(true); renderer.setWoodlandEnabled(false);
		CityLighting lighting; lighting.initialize(); renderer.setTerrainShader(lighting.terrainShader());
		renderer.setLandscapeShaders(lighting.fieldShader(), lighting.paddyShader(), lighting.foliageShader());
		RoadNetwork roads;
		const Size size{320, 240}; const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm, HasDepth::Yes};
		auto render = [&](Vec3 eye)
		{
			const BasicCamera3D camera{size, 45_deg, eye, Vec3{165, 20, 165}};
			{ const ScopedRenderTarget3D scope{target.clear(ColorF{0})}; const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(camera); renderer.render(world, roads, camera); }
			Graphics3D::Flush();
		};
		auto settle = [&]
		{
			const Stopwatch timeout{StartImmediately::Yes};
			do { render({165, 100, 50}); std::this_thread::sleep_for(std::chrono::milliseconds{1}); }
			while (renderer.pendingTerrainJobs() && timeout.sF() < 10);
			context.expect(renderer.pendingTerrainJobs() == 0, U"Independent tree and terrain jobs complete");
		};
		settle();
		const size_t count = renderer.treeInstanceCount();
		const uint64 treeBuilds = renderer.treePlacementBuilds(), terrainBuilds = renderer.terrainBuilds();
		context.expect(renderer.buildingsSubmitted()>0,U"The render-distance fixture contains a visible production building");
		const auto buildingBounds=renderer.buildingHitBox(*chunk,world,8,8);
		(void)renderer.setRenderDistance(100);render({165,500,50});
		context.expect(renderer.buildingsSubmitted()==0 && renderer.treesSubmitted()==0,U"Finite distance removes distant production buildings and tree submissions");
		context.expect(buildingBounds && !renderer.buildingWithinRenderDistance(*chunk,8,8,*buildingBounds,{165,500,50}),U"Hidden buildings cannot be picked");
		context.expect(renderer.buildingHitBox(*chunk,world,8,8).has_value(),U"Construction still obtains geometric bounds for a hidden building");
		(void)renderer.setRenderDistance(0);render({165,100,50});
		context.expect(renderer.buildingsSubmitted()>0 && renderer.treeInstanceCount()==count,U"Resetting restores the same cached building and trees");
		context.expect(renderer.treePlacementBuilds()==treeBuilds && renderer.terrainBuilds()==terrainBuilds,U"Render distance never regenerates terrain or planting");
		context.expect(count > 10, U"Public garden planting is cached independently of natural woodland");
		for (int repeat = 0; repeat < 3; ++repeat) { render({165, 2200, 50}); render({165, 100, 50}); }
		context.expect(renderer.treePlacementBuilds() == treeBuilds && renderer.terrainBuilds() == terrainBuilds, U"Crossing both LOD and former eviction distances does not start any regeneration");
		context.expect(renderer.treeInstanceCount() == count, U"Returning to the same garden reuses identical placement");
		renderer.invalidateAllTerrain(); settle();
		context.expect(renderer.treeInstanceCount() == count, U"A terrain cache reset preserves the trees owned by still-cached building lots");
		chunk->meshDirty = true; render({165, 100, 50});
		chunk->landPatches.clear(); chunk->buildingGrid[{20, 20}].type = BuildingType::None; chunk->buildingGrid[{8, 8}].type = BuildingType::None; chunk->meshDirty = true;
		settle();
		context.expect(renderer.treeInstanceCount() == 0, U"An edit supersedes old in-flight placement and removes deleted garden trees");
		context.expect(renderer.treePlacementBuilds() > treeBuilds, U"Actual world edits still invalidate tree placement");
	});
}
