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
		context.expect(count > 10, U"Public garden planting is cached independently of natural woodland");
		for (int repeat = 0; repeat < 3; ++repeat) { render({165, 2200, 50}); render({165, 100, 50}); }
		context.expect(renderer.treePlacementBuilds() == treeBuilds && renderer.terrainBuilds() == terrainBuilds, U"Crossing both LOD and former eviction distances does not start any regeneration");
		context.expect(renderer.treeInstanceCount() == count, U"Returning to the same garden reuses identical placement");
		renderer.invalidateAllTerrain(); settle();
		context.expect(renderer.treeInstanceCount() == count, U"A terrain cache reset preserves the trees owned by still-cached building lots");
		chunk->meshDirty = true; render({165, 100, 50});
		chunk->landPatches.clear(); chunk->buildingGrid[{20, 20}].type = BuildingType::None; chunk->meshDirty = true;
		settle();
		context.expect(renderer.treeInstanceCount() == 0, U"An edit supersedes old in-flight placement and removes deleted garden trees");
		context.expect(renderer.treePlacementBuilds() > treeBuilds, U"Actual world edits still invalidate tree placement");
	});
}
