#include "TestCases.hpp"
#include "TestRunner.hpp"
#include "src/zone/Building.hpp"
#include "src/road/ObjParser.hpp"

/// @brief 配布モデル・焼き込み画像のロードと全バリエーションの到達性を検証する。
void registerBuildingAssetTests(TestRunner& runner)
{
	runner.add(U"RoadAssets.CurbFacesSurviveWhitespace", [](TestContext& context)
	{
		const auto meshes = ObjParser::parse(U"../../App/assets/road_parts/curb_concrete.obj");
		int triangles = 0;
		for (const auto& mesh : meshes)
		{
			for (const auto& triangle : mesh.indices)
			{
				const auto a = mesh.vertices[triangle.i0].pos;
				const auto b = mesh.vertices[triangle.i1].pos;
				const auto c = mesh.vertices[triangle.i2].pos;
				context.expect((b-a).cross(c-a).lengthSq() > 1e-10f, U"Curb top and vertical faces have area");
				++triangles;
			}
		}
		context.expectEqual(triangles, 8, U"All four OBJ quads are intact");
	});
	runner.add(U"Buildings.RealisticAssetsLoad", [](TestContext& context)
	{
		const Array<String> stems{
			U"shop_001",
			U"shop_002",
			U"shop_003",
			U"factory_001",
			U"factory_002",
			U"public_001",
			U"public_002",
			U"residential_011",
			U"residential_012",
			U"residential_013",
			U"residential_014",
			U"residential_015",
			U"residential_016",
			U"residential_017",
			U"residential_018",
			U"office_003",
			U"office_004",
			U"shop_004",
			U"shop_005",
			U"shop_006",
			U"factory_003",
			U"factory_004",
			U"public_003",
			U"public_004"
		};
		for (const auto& stem : stems)
		{
			const String folder = stem.starts_with(U"residential_") ? U"residential" : U"commercial";
			const String basePath = U"../../App/assets/buildings/{}/{}"_fmt(folder, stem);
			const Model model{ basePath + U".obj" };
			context.expect(!model.isEmpty(), U"OBJ load: " + stem);
			if (model.isEmpty())
			{
				continue;
			}
			context.expectEqual(static_cast<int64>(model.materials().size()), 1, U"Baked material: " + stem);
			const Box bounds = model.boundingBox();
			context.expectNear(bounds.center.y - bounds.size.y * 0.5, 0.0, 0.001, U"Ground origin: " + stem);
			context.expect(bounds.size.x <= 11.0 && bounds.size.z <= 11.0, U"Plot dimensions: " + stem);
			context.expect(bounds.size.y > 3.0 && bounds.size.y <= 48.0, U"Height: " + stem);
			const Image diffuse{ basePath + U"_diffuse.png" };
			context.expect((diffuse.width() == 1024 || diffuse.width() == 2048)
				&& diffuse.height() == diffuse.width(), U"Baked albedo/AO image: " + stem);
		}
	});
	runner.add(U"Buildings.AllVariantsReachable", [](TestContext& context)
	{
		struct ModelFamily
		{
			BuildingType type;
			int64 expectedCount;
		};
		const ModelFamily families[] = {
			{ BuildingType::Detached, 8 }, { BuildingType::LowApartment, 4 },
			{ BuildingType::MidApartment, 3 }, { BuildingType::HighApartment, 3 },
			{ BuildingType::Shop, 10 }, { BuildingType::Office, 6 },
			{ BuildingType::Factory, 4 }, { BuildingType::PublicFacility, 8 }, { BuildingType::Parking, 2 }
		};
		for (const auto& family : families)
		{
			HashSet<String> found;
			for (int coordinate = -64; coordinate <= 64; ++coordinate)
			{
				String stem;
				context.expect(tryGetBuildingModelStem(family.type, coordinate, 0, stem), U"Model selection succeeds");
				found.insert(stem);
			}
			context.expectEqual(static_cast<int64>(found.size()), family.expectedCount, U"All family variants reachable");
			const String folder = isResidentialBuildingType(family.type) ? U"residential" : U"commercial";
			for (const auto& stem : found)
			{
				const String path = U"../../App/assets/buildings/{}/{}"_fmt(folder, stem);
				context.expect(FileSystem::IsFile(path + U".obj"), U"Selected OBJ exists: " + stem);
				const Model model{ path + U".obj" };
				context.expect(!model.isEmpty(), U"Every selected model, including refined legacy variants, loads: " + stem);
				if (!model.isEmpty())
				{
					const Box bounds = model.boundingBox();
					context.expectNear(bounds.center.y - bounds.size.y * 0.5, 0.0, 0.001, U"Model rests on its ground origin: " + stem);
				}
				const Model distant{ U"../../App/assets/buildings/lod/{}.obj"_fmt(stem) };
				context.expect(!distant.isEmpty(), U"Distance mesh loads: " + stem);
				if (!distant.isEmpty() && !model.isEmpty())
				{
					const Box bounds = model.boundingBox(), reduced = distant.boundingBox();
					context.expect(reduced.size.x <= bounds.size.x + 0.05 && reduced.size.z <= bounds.size.z + 0.05,
						U"Reduction cannot enlarge the lot footprint: " + stem);
					context.expectNear(reduced.size.y, bounds.size.y, 0.15, U"Distance mesh preserves roof height: " + stem);
					context.expectNear(reduced.center.y - reduced.size.y * 0.5, 0.0, 0.01, U"Distance mesh retains ground contact: " + stem);
				}
				context.expect(FileSystem::IsFile(path + U".toml"), U"Setback metadata exists: " + stem);
			}
		}
	});

	runner.add(U"Assets.CivicTransportLoad", [](TestContext& context)
	{
		const Array<String> paths{
			U"../../App/assets/buildings/commercial/parking_001",
			U"../../App/assets/buildings/commercial/parking_002",
			U"../../App/assets/buildings/commercial/public_005",
			U"../../App/assets/buildings/commercial/public_006",
			U"../../App/assets/buildings/commercial/public_007",
			U"../../App/assets/buildings/commercial/public_008",
			U"../../App/assets/railway/station_001",
			U"../../App/assets/railway/station_002",
			U"../../App/assets/vehicles/sedan",
			U"../../App/assets/vehicles/kei_wagon",
			U"../../App/assets/vehicles/patrol_car",
			U"../../App/assets/vehicles/fire_engine",
			U"../../App/assets/vehicles/city_bus",
			U"../../App/assets/railway/commuter_001",
			U"../../App/assets/railway/commuter_002"
		};
		for (const auto& path : paths)
		{
			const Model model{ path + U".obj" };
			context.expect(!model.isEmpty(), U"Transport/civic OBJ load: " + path);
			if (model.isEmpty())
			{
				continue;
			}
			const Box bounds = model.boundingBox();
			context.expectNear(bounds.center.y - bounds.size.y * 0.5, 0.0, 0.001, U"Ground/tyre origin");
			context.expectEqual(static_cast<int64>(model.materials().size()), 1, U"Single baked material");
			context.expect(bounds.size.x <= 32 && bounds.size.y <= 20 && bounds.size.z <= 32, U"Physical dimensions");
			const Image diffuse{ path + U"_diffuse.png" };
			context.expect((diffuse.width() == 1024 || diffuse.width() == 2048)
				&& diffuse.width() == diffuse.height(), U"Baked transport texture");
		}
	});
}
