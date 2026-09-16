#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/render/ModelBatch.hpp"
#include "src/render/VehicleRenderer.hpp"
#include "src/render/GpuFrameTimer.hpp"
#include "src/traffic/SignalRegistry.hpp"
#include "src/asset/AssetRegistrar.hpp"

namespace
{
	struct AppDirectory
	{
		FilePath previous = FileSystem::CurrentDirectory();
		AppDirectory() { FileSystem::ChangeCurrentDirectory(previous + U"../../App/"); RegisterAssets(); }
		~AppDirectory() { FileSystem::ChangeCurrentDirectory(previous); }
	};
}

void registerModelLodTests(TestRunner& runner)
{
	runner.add(U"ModelLod.AllAssets", [](TestContext& context)
	{
		AppDirectory directory;
		const auto manifest = JSON::Load(U"assets/lod/manifest.json");
		context.expect(static_cast<bool>(manifest), U"All-model LOD inventory exists");
		if (!manifest) { return; }
		context.expect(modelLodPath(FileSystem::FullPath(U"assets/signals/signal.obj"),2) ==
			modelLodPath(U"assets/signals/signal.obj",2), U"Registry absolute paths resolve the same LOD asset");
		SignalRegistry signals; signals.load(U"assets/signals/");
		const auto* signal=signals.getModel(U"signal_3lamp");
		context.expect(signal!=nullptr,U"Production signal registry loads");
		if (signal) { for (const auto& level : signal->lodMeshes) { for (const String part : {U"body",U"lamp_0",U"lamp_1",U"lamp_2",U"sub_body",U"sub_lamp"})
		{
			context.expect(level.contains(part) && !level.at(part).indices.isEmpty(),U"LOD retains renderable signal part: "+part);
		} } }
		int64 originalTriangles = 0, farTriangles = 0, count = 0;
		for (const auto& item : manifest.arrayView())
		{
			const String source = U"assets/" + item[U"source"].get<String>();
			const Model original{source}; context.expect(!original.isEmpty(), U"Source model loads: " + source);
			if (original.isEmpty()) { continue; }
			const auto bounds = original.boundingBox();
			for (int level = 1; level <= 2; ++level)
			{
				const Model model{modelLodPath(source, level)};
				context.expect(!model.isEmpty(), U"Every source has both LODs: " + source);
				if (model.isEmpty()) { continue; }
				context.expectNear(model.boundingBox().size.distanceFrom(bounds.size), 0, .15, U"LOD retains original dimensions: " + source);
				context.expectNear(model.boundingBox().center.distanceFrom(bounds.center), 0, .1, U"LOD retains anchor: " + source);
				for (const auto& material : model.materials())
				{
					if (!material.diffuseTextureName.isEmpty()) { context.expect(FileSystem::IsFile(material.diffuseTextureName), U"LOD texture path resolves: " + source); }
				}
			}
			originalTriangles += item[U"sourceTriangles"].get<int64>();
			farTriangles += item[U"levels"][1][U"triangles"].get<int64>(); ++count;
		}
		context.expect(count >= 88 && farTriangles < originalTriangles / 5, U"All source models have substantially lighter far geometry");
	});

	runner.add(U"ModelLod.NativeAndBatchedReadback", [](TestContext& context)
	{
		AppDirectory directory; JSON report;
		const Size size{640,480};
		const RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		for (const String source : {U"assets/vehicles/sedan.obj", U"assets/buildings/residential/residential_012.obj", U"assets/railway/urban_cab.obj"})
		{
			const String path = modelLodPath(source, 1);
			const Model model{path}; Model::RegisterDiffuseTextures(model, TextureDesc::MippedSRGB);
			const auto geometry = loadModelMeshSource(path, model);
			Array<StaticModelBatch> batches;
			for (const auto& part : geometry) { batches << StaticModelBatch{Mesh{part.geometry}, part.material}; }
			const auto bounds = model.boundingBox(); const double radius = bounds.size.length();
			const BasicCamera3D camera{size, 40_deg, bounds.center + Vec3{radius * .8, radius * .5, -radius}, bounds.center};
			const auto draw = [&](bool batched)
			{
				{
					const ScopedRenderTarget3D scope{target.clear(ColorF{.1,.2,.3})};
					const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite, RasterizerState::SolidCullBack};
					Graphics3D::SetCameraTransform(camera);
					Graphics3D::SetSunDirection(Vec3{1,1,-1}.normalized()); Graphics3D::SetSunColor(ColorF{.8}); Graphics3D::SetGlobalAmbientColor(ColorF{.4});
					if (batched) { for (const auto& batch : batches) { batch.draw(); } }
					else { model.draw(); }
				}
				Graphics3D::Flush(); Image image; target.readAsImage(image); return image;
			};
			const auto native = draw(false), batched = draw(true);
			double difference = 0; int changed = 0;
			for (int y = 0; y < size.y; ++y) for (int x = 0; x < size.x; ++x)
			{
				const auto a = native[y][x], b = batched[y][x];
				difference += Abs(static_cast<int>(a.r) - b.r) + Abs(static_cast<int>(a.g) - b.g) + Abs(static_cast<int>(a.b) - b.b);
				changed += a != native[0][0];
			}
			difference /= size.x * size.y * 3;
			context.expect(changed > 1000, U"Real geometry survives front-face culling");
			context.expect(difference < 1.0, U"Batched shape, winding, UV and material match native Model");
			report[source][U"meanChannelDifference"] = difference; report[source][U"visiblePixels"] = changed;
		}
		report.save(directory.previous + U"TestResults/model_batch_readback.json");
	});

	runner.add(U"ModelLod.GpuInstancesMatchNative", [](TestContext& context)
	{
		AppDirectory directory;
		const String path=modelLodPath(U"assets/vehicles/sedan.obj",2);
		const Model model{path}; const auto parts=loadModelMeshSource(path,model);
		Array<VehicleInstanceBatch> batches(parts.size()); Array<Mat4x4> transforms;
		for (int i=0;i<257;++i)
		{
			const auto matrix=Mat4x4::Scale(1.2f)*Mat4x4::RotateY(i*.37f)*Mat4x4::Translate(Float3{static_cast<float>(i%17)*8,static_cast<float>(i%3),static_cast<float>(i/17)*8});
			transforms << matrix;
			for (size_t part=0;part<parts.size();++part) { batches[part].append(parts[part],matrix); }
		}
		const Size size{640,480}; const RenderTexture target{size,TextureFormat::R8G8B8A8_Unorm_SRGB,HasDepth::Yes};
		const BasicCamera3D camera{size,40_deg,Vec3{80,140,-130},Vec3{64,0,60}};
		const auto draw=[&](bool batched)
		{
			{
				const ScopedRenderTarget3D scope{target.clear(ColorF{.1,.2,.3})};
				const ScopedRenderStates3D states{DepthStencilState::DepthTestWrite,RasterizerState::SolidCullBack};
				Graphics3D::SetCameraTransform(camera);
				Graphics3D::SetSunDirection(Vec3{1,1,-1}.normalized()); Graphics3D::SetSunColor(ColorF{.8}); Graphics3D::SetGlobalAmbientColor(ColorF{.4});
				if (batched) { for (auto& batch : batches) { batch.draw(); } }
				else { for (const auto& matrix : transforms) { model.draw(matrix); } }
			}
			Graphics3D::Flush(); Image pixels; target.readAsImage(pixels); return pixels;
		};
		const auto expected=draw(false),actual=draw(true); double difference=0; int visible=0;
		for (int y=0;y<size.y;++y) for (int x=0;x<size.x;++x)
		{
			const auto a=expected[y][x],b=actual[y][x]; visible += a!=expected[0][0];
			difference += Abs(static_cast<int>(a.r)-b.r)+Abs(static_cast<int>(a.g)-b.g)+Abs(static_cast<int>(a.b)-b.b);
		}
		difference/=size.x*size.y*3;
		context.expect(visible>2000 && difference<1, U"GPU matrices preserve native placement, rotation, material and batch boundary 256/257");
		JSON report; report[U"visiblePixels"]=visible;report[U"meanChannelDifference"]=difference;
		report.save(directory.previous+U"TestResults/gpu_instances_readback.json");
	});

	runner.add(U"ModelLod.DenseVehicleRendering", [](TestContext& context)
	{
		AppDirectory directory; JSON report;
		Array<Vehicle> vehicles;
		for (int i = 0; i < 12000; ++i)
		{
			Vehicle vehicle; vehicle.id = i; vehicle.type = i % 3 == 0 ? VehicleType::KeiCar : VehicleType::PassengerCar;
			vehicle.position = {static_cast<double>(i % 120) * 5, 0, static_cast<double>(i / 120) * 9}; vehicles << vehicle;
		}
		const Size size{1280,720};
		const Vec3 eye{300,1400,-600};
		const BasicCamera3D camera{size, 45_deg, eye, Vec3{300,0,450}};
		RenderTexture target{size, TextureFormat::R8G8B8A8_Unorm_SRGB, HasDepth::Yes};
		GpuFrameTimer gpu; VehicleRenderer renderer;
		Array<double> cpuTimes, gpuTimes;
		for (int frame = 0; frame < 50; ++frame)
		{
			if (!System::Update()) { break; }
			gpu.begin(target);
			Stopwatch timer{StartImmediately::Yes};
			{
				const ScopedRenderTarget3D scope{target.clear(ColorF{.1,.2,.3})};
				const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
				Graphics3D::SetCameraTransform(camera);
				renderer.render(vehicles, eye, &camera);
			}
			Graphics3D::Flush(); const double milliseconds = timer.msF(); gpu.end();
			if (frame >= 20) { cpuTimes << milliseconds; if (gpu.milliseconds() >= 0) { gpuTimes << gpu.milliseconds(); } }
		}
		context.expect(renderer.submitted() > 8000, U"Dense benchmark actually renders thousands of vehicles");
		context.expect(renderer.drawCalls() < 200, U"Far cars share material batches rather than one draw per car");
		cpuTimes.sort(); gpuTimes.sort();
		report[U"cars"] = renderer.submitted(); report[U"drawCalls"] = renderer.drawCalls();
		if (!cpuTimes.isEmpty()) { report[U"cpuMedianMs"] = cpuTimes[cpuTimes.size()/2]; }
		if (!gpuTimes.isEmpty()) { report[U"gpuMedianMs"] = gpuTimes[gpuTimes.size()/2]; }
		Image image; target.readAsImage(image); image.save(directory.previous + U"Screenshot/dense_vehicle_lod.png");
		// Shrinking the active range must not draw stale vertices left in the fixed capacity buffer.
		{
			const ScopedRenderTarget3D scope{target.clear(ColorF{.1,.2,.3})};
			const ScopedRenderStates3D state{DepthStencilState::DepthTestWrite};
			Graphics3D::SetCameraTransform(camera); renderer.render({}, eye, &camera);
		}
		Graphics3D::Flush(); target.readAsImage(image);
		bool uniform = true; for (const auto pixel : image) { uniform &= pixel == image[0][0]; }
		context.expect(uniform && renderer.drawCalls() == 0, U"An empty traffic frame draws no previous cars");
		report.save(directory.previous + U"TestResults/dense_vehicle_lod.json");
	});
}
