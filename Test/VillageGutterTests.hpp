#pragma once
#include "TestRunner.hpp"
#include "src/gen/StreetProfile.hpp"
#include "src/road/RoadPartRegistry.hpp"
#include "src/asset/ModelLodPath.hpp"
#include <cmath>

/// @brief Geometry and registry checks for the profile-specific covered Village gutter.
namespace VillageGutterTests
{
	inline constexpr StringView kCoveredId = U"roadside_gutter_covered_concrete";
	inline constexpr StringView kLegacyId = U"roadside_gutter_concrete";
	inline constexpr double kEpsilon = 0.000001;

	/// @brief Measure the actual parsed mesh, not TOML height_offset alone.
	struct Bounds
	{
		Float3 minimum{Math::InfF, Math::InfF, Math::InfF};
		Float3 maximum{-Math::InfF, -Math::InfF, -Math::InfF};
	};

	inline Bounds bounds(const PartModelData& model)
	{
		Bounds result;
		for (const auto& vertex : model.vertices)
		{
			result.minimum.x = Min(result.minimum.x, vertex.pos.x);
			result.minimum.y = Min(result.minimum.y, vertex.pos.y);
			result.minimum.z = Min(result.minimum.z, vertex.pos.z);
			result.maximum.x = Max(result.maximum.x, vertex.pos.x);
			result.maximum.y = Max(result.maximum.y, vertex.pos.y);
			result.maximum.z = Max(result.maximum.z, vertex.pos.z);
		}
		return result;
	}

	/// @brief Check that authored cover faces fully tile the structural strip without a raised barrier.
	inline void checkCover(TestContext& context, const PartModelData& model, const RoadPartDef& definition)
	{
		context.expect(!model.isEmpty() && !model.indices.isEmpty(), U"The covered gutter contains real geometry");
		if (model.isEmpty() || model.indices.isEmpty()) { return; }
		const auto box = bounds(model);
		context.expectNear(box.minimum.x, 0, kEpsilon, U"Cover begins at the structural strip boundary");
		context.expectNear(box.maximum.x, definition.modelUnitWidth, kEpsilon, U"Cover occupies precisely the declared gutter width");
		context.expectNear(box.minimum.z, 0, kEpsilon, U"Cover module begins at zero");
		context.expectNear(box.maximum.z, definition.modelUnitLen, kEpsilon, U"Cover module reaches the next joint");
		context.expectNear(box.maximum.y + definition.heightOffset, 0, kEpsilon, U"The highest cover vertex is flush with asphalt");
		context.expectNear(box.minimum.y + definition.heightOffset, -0.004, kEpsilon, U"Recessed joints are only four millimetres deep");
		double projectedArea = 0;
		for (const auto& triangle : model.indices)
		{
			const bool valid = triangle.i0 < model.vertices.size() && triangle.i1 < model.vertices.size() && triangle.i2 < model.vertices.size();
			context.expect(valid, U"Every cover face has valid vertex indices");
			if (!valid) { continue; }
			const auto a = model.vertices[triangle.i0].pos;
			const auto b = model.vertices[triangle.i1].pos;
			const auto c = model.vertices[triangle.i2].pos;
			const auto cross = (b - a).cross(c - a);
			context.expect(cross.lengthSq() > 1e-14f, U"Every cover face has nonzero area");
			context.expect(cross.y > 0, U"Cover faces wind upward consistently");
			projectedArea += Abs(cross.y) * 0.5;
		}
		context.expectNear(projectedArea, definition.modelUnitWidth * definition.modelUnitLen, kEpsilon,
			U"Cover faces tile the complete strip area without deleting its drainage allocation");
		for (const auto& vertex : model.vertices)
		{
			context.expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && std::isfinite(vertex.pos.z), U"Cover vertices are finite");
			context.expect(vertex.normal.y > 0.6f, U"Shallow cover joints do not create vertical curb faces");
		}
	}

	/// @brief Register before production profile wiring so the Village selection case first fails normally.
	/// @param assetDirectory Absolute directory containing original assets and the candidate pair for preview.
	inline void registerTests(TestRunner& runner, FilePath assetDirectory = FileSystem::FullPath(U"../../App/assets/road_parts/"))
	{
		runner.add(U"UrbanStructure.VillageGutterLodOverlayPaths", [assetDirectory](TestContext& context)
		{
			struct Restore { FilePath directory; ~Restore() { FileSystem::ChangeCurrentDirectory(directory); } } restore{FileSystem::CurrentDirectory()};
			FileSystem::ChangeCurrentDirectory(assetDirectory+U"/../../");
			Array<JSON> paths;
			for (const FilePath source : {U"assets/buildings/residential/residential_001.obj",U"assets/buildings/commercial/office_001.obj",U"assets/road_parts/curb_concrete.obj"})
			{
				const FilePath path=modelLodPath(source,2);
				context.expect(path.ends_with(U".lod2.obj") && FileSystem::IsFile(path),U"The isolated overlay selects the real production LOD2 asset instead of the full source model");
				context.expect(modelLodPath(FileSystem::FullPath(source),2)==path,U"Canonical absolute and relative model paths select identical LOD tiers");
				JSON item; item[U"source"]=source; item[U"lod2"]=path; paths << item;
			}
			JSON report; report[U"paths"]=paths; report.save(restore.directory+U"TestResults/gutter_lod_overlay_paths.json");
		});
		runner.add(U"UrbanStructure.VillageCoveredGutterGeometry", [assetDirectory](TestContext& context)
		{
			RoadPartRegistry registry;
			context.expect(registry.load(assetDirectory), U"The candidate fixture registry loads");
			const auto& definition = registry.get(kCoveredId);
			context.expect(definition.id == kCoveredId, U"The covered gutter resolves to its own definition, never fallback");
			if (definition.id != kCoveredId) { return; }
			context.expect(definition.type == RoadPartType::RoadsideGutter && definition.placement == RoadPartPlacement::Strip
				&& definition.envelopeRole == RoadPartEnvelopeRole::Structural, U"Covered drainage retains the structural gutter role");
			context.expect(definition.modelPath.ends_with(U"roadside_gutter_covered_concrete.obj"), U"Covered drainage has a dedicated model");
			context.expectNear(definition.modelUnitWidth, 0.32, kEpsilon, U"The authored gutter width remains 320 mm");
			context.expectNear(definition.modelUnitLen, 1.0, kEpsilon, U"The authored cover module is one metre");
			context.expectNear(definition.heightOffset, 0, kEpsilon, U"No hidden asset offset reintroduces the barrier");
			const auto& model = registry.getModel(kCoveredId);
			context.expect(model.inner.isEmpty() && model.outer.isEmpty(), U"The cover has no duplicated or protruding edge cap");
			checkCover(context, model.center, definition);
			context.expectEqual(static_cast<int64>(model.lods.size()), 1, U"A matching bounded distant cover is provided");
			for (const auto& lod : model.lods) { checkCover(context, lod.center, definition); }
		});

		runner.add(U"UrbanStructure.VillageGutterPreservesRaisedCurbs", [assetDirectory](TestContext& context)
		{
			RoadPartRegistry registry;
			context.expect(registry.load(assetDirectory), U"Protected curb definitions load");
			for (const StringView id : {StringView{U"curb_concrete"}, kLegacyId})
			{
				const auto& definition = registry.get(id);
				context.expect(definition.id == id && definition.modelPath.ends_with(U"curb_concrete.obj"), U"Existing raised curb and non-Village gutter retain their model");
				const auto& model = registry.getModel(id).center;
				context.expect(!model.isEmpty(), U"The protected raised model still resolves");
				if (model.isEmpty()) { continue; }
				const auto box = bounds(model);
				context.expectNear(box.minimum.y + definition.heightOffset, 0, kEpsilon, U"Existing raised model base is unchanged");
				context.expectNear(box.maximum.y + definition.heightOffset, 0.15, kEpsilon, U"Proper raised curb remains 150 mm high");
				context.expectNear(definition.modelUnitWidth, 0.1, kEpsilon, U"Protected curb authoring width is unchanged");
			}
		});

		runner.add(U"UrbanStructure.VillageCoveredGutterProfileScope", [](TestContext& context)
		{
			using namespace GeneratedStreet;
			const Array<Role> roles{Role::FarmAccess, Role::Village, Role::Local, Role::OneWay, Role::ResidentialWalkways,
				Role::Collector, Role::MainArterial, Role::Regional, Role::Mountain};
			const auto& settings = GenerationSettings::get();
			for (const auto role : roles)
			{
				RoadEdge edge;
				const auto profile = describe(role);
				apply(edge, profile);
				int gutterCount = 0, curbCount = 0;
				float minimum = 0, maximum = 0;
				for (const auto& part : edge.parts)
				{
					if (part.envelopeRole == RoadPartEnvelopeRole::Structural && part.placement == RoadPartPlacement::Strip)
					{
						minimum = Min(minimum, part.offsetA_L); maximum = Max(maximum, part.offsetA_R);
					}
					if (part.type == RoadPartType::RoadsideGutter)
					{
						++gutterCount;
						const bool covered = role == Role::Village && (part.offsetL() < 0 ? profile.walkwayLeft : profile.walkwayRight) <= 0;
						context.expect(part.defId == (covered ? kCoveredId : kLegacyId), U"Only Village sides without a sidewalk select the new covered asset");
						context.expectNear(part.widthA(), settings.streetProfiles_gutterWidth, kEpsilon, U"A-end structural gutter width is unchanged");
						context.expectNear(part.widthB(), settings.streetProfiles_gutterWidth, kEpsilon, U"B-end structural gutter width is unchanged");
					}
					if (part.type == RoadPartType::Curb)
					{
						++curbCount;
						context.expect(part.defId == U"curb_concrete", U"Proper sidewalks retain their original raised curb asset");
						context.expectNear(part.width(), settings.streetProfiles_curbWidth, kEpsilon, U"Proper curb widths are unchanged");
					}
				}
				context.expectEqual(gutterCount, 2, U"Both drainage strips remain present");
				context.expectEqual(curbCount, static_cast<int>(profile.walkwayLeft > 0) + static_cast<int>(profile.walkwayRight > 0), U"Sidewalk curb count remains profile-specific");
				const double expectedWidth = profile.lanes * profile.laneWidth + 2 * (profile.shoulder + settings.streetProfiles_gutterWidth)
					+ profile.walkwayLeft + profile.walkwayRight + curbCount * settings.streetProfiles_curbWidth;
				context.expectNear(maximum - minimum, expectedWidth, 0.00001, U"The entire legal structural footprint is unchanged");
				if (role == Role::Village)
				{
					context.expectEqual(static_cast<int64>(edge.lanes.size()), 2, U"Village logical lanes are unchanged");
					context.expectNear(profile.laneWidth, 2.5, kEpsilon, U"Village lane width remains 2.5 metres");
					context.expectNear(maximum - minimum, 6.34, 0.00001, U"Village structural width remains 6.34 metres");
				}
			}
			// A caller can add a real sidewalk to one Village side; do not flatten that side's treatment.
			auto asymmetric = describe(Role::Village);
			asymmetric.walkwayLeft = 1.8f;
			RoadEdge edge;
			apply(edge, asymmetric);
			int curbCount = 0;
			for (const auto& part : edge.parts)
			{
				if (part.type == RoadPartType::RoadsideGutter)
				{
					context.expect(part.defId == (part.offsetL() < 0 ? kLegacyId : kCoveredId), U"An explicit Village sidewalk retains its legacy side while only the flush side changes");
				}
				if (part.type == RoadPartType::Curb) { ++curbCount; context.expect(part.defId == U"curb_concrete", U"Asymmetric proper curb is protected"); }
			}
			context.expectEqual(curbCount, 1, U"Asymmetric Village keeps its actual sidewalk curb");
		});
	}
}
